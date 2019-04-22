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
 * insert a pagetable entry that maps to the provided entry. 
 */
void pagetable_insert(paddr_t **pagetable, vaddr_t vaddr, paddr_t entry) {
    // frame has been allocated. Insert entry into pagetable.
    vaddr_t indexT1 = vaddr >> 22;                                 // first-level table index.
    vaddr_t indexT2 = (vaddr >> 12) & (~(vaddr_t)PAGE_FRAME >> 2); // second-level table index.
    unsigned int i;

    KASSERT(pagetable != NULL);

    /* if the second level page table does not yet exist, allocate it. */
    if (pagetable[indexT1] == NULL) {
        pagetable[indexT1] = (paddr_t *)alloc_kpages(1);
        /* fill the second level page table with empty slots. */
        for (i = 0; i < TABLE_SIZE; i++) {
            pagetable[indexT1][i] = 0;
        }
    }

    /* store the entry in the page table. */
    pagetable[indexT1][indexT2] = entry;

    return;
}

/*
 * lookup pagetable at location vaddr and return entry. Return null if non exists.
 */
void pagetable_lookup(paddr_t **pagetable, vaddr_t vaddr, paddr_t *entry) {
    vaddr_t indexT1 = vaddr >> 22; // first-level table index.
    vaddr_t indexT2 = (vaddr >> 12) & (~(vaddr_t)PAGE_FRAME >> 2); // second-level table index.

    KASSERT(pagetable != NULL);

    if (pagetable[indexT1] == NULL) {
        /* second-level table does not exist, therefore page table entry does not exist. */
        *entry = 0;
        return;
    }
    if (pagetable[indexT1][indexT2] == 0) {
        /* page table entry does not exist. */
        *entry = 0;
        return;
    }
    /* save entry in return address. */
    *entry = pagetable[indexT1][indexT2];

    return;
}

/*
 * updates the dirty bit in order to toggle readonly and read/write entries.
 */
void pagetable_update(paddr_t **pagetable, vaddr_t reg_vbase, size_t reg_npages) {
    vaddr_t indexT1;
    vaddr_t indexT2;
    unsigned int i;
    vaddr_t reg_vend = reg_vbase + reg_npages*PAGE_SIZE;

    KASSERT(pagetable != NULL);
    KASSERT(reg_vend <= MIPS_KSEG0);

    for (i = reg_vbase; i < reg_vend; i = i + PAGE_SIZE) {
        indexT1 = i >> 22;
        /* if 2nd-level table does not exist simply skip to next 1st-level entry. */
        if (pagetable[indexT1] == NULL) {
            i = i + TABLE_SIZE*PAGE_SIZE - i%(TABLE_SIZE*PAGE_SIZE) - PAGE_SIZE;
        } else {
            indexT2 = (i >> 12) & (~(vaddr_t)PAGE_FRAME >> 2);
            /* if there is an entry toggle its dirty bit. */
            if (pagetable[indexT1][indexT2] != 0) {
                pagetable[indexT1][indexT2] ^= TLBLO_DIRTY;
            }
        }
    }
    return;
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

    KASSERT(as->pagetable != NULL);
    KASSERT(as->regions != NULL);

    /* Check if faultaddress exists in page table and store entry*/
    paddr_t entryLo;
    pagetable_lookup(as->pagetable, faultaddress, &entryLo);
    if (entryLo != 0) {
        /* load the TLB. */
        /* Disable interrupts on this CPU while frobbing the TLB. */
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
        /* allocate a new frame for the faultaddress */
        paddr = (paddr_t)alloc_upages(1);

        KASSERT((paddr & ~(paddr_t)PAGE_FRAME) == 0);
        KASSERT(paddr < MIPS_KSEG0);

        /* zero fill the frame */
        //bzero((void *)paddr, (size_t)PAGE_SIZE);

        /* add permissions to pagetable entry */
        entryLo = paddr | TLBLO_VALID;
        if ((cur_reg->permissions & RF_W) != 0) {
            entryLo |= TLBLO_DIRTY;
        }

        /* place etnry in pagetable. */
        pagetable_insert(as->pagetable, faultaddress, entryLo);

        /* load the TLB. */
        /* Disable interrupts on this CPU while frobbing the TLB. */
	    int spl = splhigh();
        tlb_random(faultaddress & TLBHI_VPAGE, entryLo);
		splx(spl);

        return 0;
    }

    return EFAULT;

    // called on a TLB refill miss.
    // vm_fault -> vm_fault READONLY -> yes -> EFAULT
    //             no ->   lookup PT -> yes -> load TLB
    //                     no ->   lookup region -> valid -> allocate frame, zero fill, insert PTE -> load TLB
    //                             non-valid ->    EFAULT

    // allocate frame can be done with alloc_kpage

    // tlb_random() can be used for TLB refill. Disable interrupts locally for tlb_random().

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

