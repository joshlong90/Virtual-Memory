#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>

#include <proc.h>
#include <current.h>

/* Place your page table functions here */

/* 
 * insert a pagetable entry that maps to the provided entryLo. 
 */
int pagetable_insert(paddr_t **pagetable, vaddr_t vaddr, paddr_t entryLo) {
    unsigned int i;
    /* retrieve the first and second level page table indexes from vaddr. */
    vaddr_t indexT1 = vaddr >> 22;
    vaddr_t indexT2 = vaddr << 10 >> 22;

    /* the pagetable should not be null */
    if (pagetable == NULL) {
        return EINVAL;
    }

    /* if the second level pagetable does not yet exist, allocate it. */
    if (pagetable[indexT1] == NULL) {
        pagetable[indexT1] = (paddr_t *)alloc_kpages(1);
        if (pagetable[indexT1] == NULL) {
            return ENOMEM;
        }
        /* fill the second level pagetable with empty slots. */
        for (i = 0; i < TABLE_SIZE; i++) {
            pagetable[indexT1][i] = 0;
        }
    }

    /* store entryLo in the pagetable. */
    pagetable[indexT1][indexT2] = entryLo;

    return 0;
}

/*
 * lookup pagetable at location vaddr and return entry. Return null if non exists.
 */
int pagetable_lookup(paddr_t **pagetable, vaddr_t vaddr, paddr_t *entry) {
    /* retrieve the first and second level page table indexes from vaddr. */
    vaddr_t indexT1 = vaddr >> 22;       // first-level table index.
    vaddr_t indexT2 = vaddr << 10 >> 22; // second-level table index.

    /* the pagetable should not be null */
    if (pagetable == NULL) {
        return EINVAL;
    }

    if (pagetable[indexT1] == NULL) {
        /* second-level table does not exist, therefore page table entry does not exist. */
        *entry = 0;
        return 0;
    }
    if (pagetable[indexT1][indexT2] == 0) {
        /* page table entry does not exist. */
        *entry = 0;
        return 0;
    }
    /* save entry in return address. */
    *entry = pagetable[indexT1][indexT2];

    return 0;
}

/*
 * flips the dirty bit off in order to change read/write entries to readonly.
 */
int pagetable_update(paddr_t **pagetable, vaddr_t reg_vbase, size_t reg_npages) {
    vaddr_t indexT1;
    vaddr_t indexT2;
    unsigned int i;
    vaddr_t reg_vend = reg_vbase + reg_npages*PAGE_SIZE;

    /* the pagetable should not be null. */
    if (pagetable == NULL) {
        return EINVAL;
    }

    /* the end of the region should be within the user memory space. */
    if (reg_vend > MIPS_KSEG0) {
        return EINVAL;
    }

    for (i = reg_vbase; i < reg_vend; i = i + PAGE_SIZE) {
        indexT1 = i >> 22;
        /* if 2nd-level table does not exist simply skip to next 1st-level entry. */
        if (pagetable[indexT1] == NULL) {
            i = i + TABLE_SIZE*PAGE_SIZE - i%(TABLE_SIZE*PAGE_SIZE) - PAGE_SIZE;
        } else {
            indexT2 = i << 10 >> 22;
            /* if there is an entry flip its dirty bit off. */
            if (pagetable[indexT1][indexT2] != 0) {
                pagetable[indexT1][indexT2] &= ~(paddr_t)TLBLO_DIRTY;
            }
        }
    }
    return 0;
}

void vm_bootstrap(void)
{
    /* Initialise VM sub-system.  You probably want to initialise your 
       frame table here as well.
    */
    /* Not required to initialise frame table for this years submission. */
}

/*
 * called when faultaddress was not found in TLB.
 * retrieves virtual memory mapping from pagetable and loads the TLB.
 * if virtual memory mapping does not exist in pagetable it is retrieved from disk.
 * returns EFAULT if memory reference is invalid.
 */
int vm_fault(int faulttype, vaddr_t faultaddress) {

    switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* Attempt to violate READONLY memory permission, return EFAULT. */
		return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

    if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

    struct addrspace *as;
    as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

    /* the pagetable should not be null. */
    if (as->pagetable == NULL) {
        return EFAULT;
    }

    /* the valid regions list should not be null */
    if (as->regions == NULL) {
        return EFAULT;
    }

    /* Check if faultaddress exists in pagetable and store entryLo. */
    paddr_t entryLo;
    int result = pagetable_lookup(as->pagetable, faultaddress, &entryLo);
    if (result != 0) {
        return result;
    }

    if (entryLo != 0) {
        /* An entry was found, disable interrupts on this CPU and load the TLB. */
	    int spl = splhigh();
        tlb_random(faultaddress & TLBHI_VPAGE, entryLo);
        splx(spl);

        return 0;
    }

    /* check to see if the faultaddress lies within a valid region. */
    struct region *cur_reg;
    cur_reg = as->regions;
    while (cur_reg != NULL) {
        /* break if faultaddress is in current region. */
        if  (faultaddress >= cur_reg->reg_vbase && faultaddress < 
                    cur_reg->reg_vbase + cur_reg->reg_npages*PAGE_SIZE) {
            break;
        }
        cur_reg = cur_reg->reg_next;
    }

    if (cur_reg != NULL) {
        paddr_t paddr;

        /* allocate a new frame for the faultaddress. */
        paddr = (paddr_t)alloc_kpages(1);
        if (paddr == 0) {
            return ENOMEM;
        }

        /* zero fill the frame. */
        bzero((void *)paddr, (size_t)PAGE_SIZE);

        /* convert to physical address and add permissions to entryLo. */
        entryLo = KVADDR_TO_PADDR(paddr) | TLBLO_VALID;

        /* add the dirty bit if fault address is in a writable region. */
        if ((cur_reg->permissions & RF_W) != 0) {
            entryLo |= TLBLO_DIRTY;
        }

        /* place etnry in pagetable. */
        result = pagetable_insert(as->pagetable, faultaddress, entryLo);
        if (result != 0) {
            return result;
        }

        /* Disable interrupts on this CPU and load the TLB. */
	    int spl = splhigh();
        tlb_random(faultaddress & TLBHI_VPAGE, entryLo);
		splx(spl);

        return 0;
    }

    /* faultaddress in invalid region exit with EFAULT. */
    return EFAULT;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

