#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

#include <proc.h>
#include <current.h>

/* Place your page table functions here */

/* 
 * insert a pagetable entry that maps to the provided entry. 
 */
void pagetable_insert(paddr_t **pagetable, vaddr_t vaddr, paddr_t entry) {
    // frame has been allocated. Insert frame number into page table.
    // frame number is expected to be paddr >> 12.
    vaddr_t indexT1 = (vaddr >> 22) & PAGE_FRAME; // first-level table index.
    vaddr_t indexT2 = (vaddr >> 12) & PAGE_FRAME; // second-level table index.
    vaddr_t i;

    KASSERT(pagetable != NULL);

    /* if the second level page table does not yet exist, allocate it. */
    if (pagetable[indexT1] == NULL) {
        pagetable[indexT1] = kmalloc(TABLE_SIZE * sizeof(paddr_t));
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
    vaddr_t indexT1 = (vaddr >> 22) & PAGE_FRAME; // first-level table index.
    vaddr_t indexT2 = (vaddr >> 12) & PAGE_FRAME; // second-level table index.

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

    /* should not have entry with no permissions at frame number 0 */
    KASSERT(*entry != 0);

    return;
}

/*
 * updates the dirty bit in order to toggle readonly and read/write entries.
 */
void pagetable_update(paddr_t **pagetable, vaddr_t reg_vbase, size_t reg_npages) {
    vaddr_t indexT1;
    vaddr_t indexT2;
    vaddr_t i;
    vaddr_t reg_vend = reg_vbase + reg_npages*PAGE_SIZE;
    paddr_t entry;

    KASSERT(pagetable != NULL);
    KASSERT(reg_vend <= MIPS_KSEG0);

    for (i = reg_vbase; i < reg_vend; i = i + PAGE_SIZE) {
        indexT1 = (i >> 22) & PAGE_FRAME;
        /* if 2nd-level table does not exist simply skip to next 1st-level entry. */
        if (pagetable[indexT1] == NULL) {
            i = i + TABLE_SIZE*PAGE_SIZE - i%(TABLE_SIZE*PAGE_SIZE) - 1;
        } else {
            indexT2 = (i >> 12) & PAGE_FRAME;
            entry = pagetable[indexT1][indexT2];
            /* if there is an entry toggle its dirty bit. */
            if (entry != 0) {
                entry = entry ^ TLBLO_DIRTY;
            }
        }
    }
    return;
}

/*
 * zeros out a region in physical memory using KSEG0 offset first.
 */
// static void as_zero_region(paddr_t paddr, unsigned npages) {
//     KASSERT(paddr < MIPS_KSEG0); // check that the physical address is in lower section.
//     KASSERT(paddr != 0);         // check that the physical address is not zero.

// 	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
// }

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
    paddr_t entry;
    pagetable_lookup(as->pagetable, faultaddress, &entry);
    if (entry != 0) {
        panic("PTE found hasn't been written yet\n");
        // must be retrieved from disk.
        return 0;
    }

    /* check to see if the faultaddress lies within a valid region. */
    struct region *cur_reg;
    cur_reg = as->regions;
    while (cur_reg != NULL && (faultaddress < cur_reg->reg_vbase || 
             faultaddress >= cur_reg->reg_vbase + cur_reg->reg_npages)) {
        cur_reg = cur_reg->reg_next;
    }

    if (cur_reg != NULL) {
        panic("valid region found");
    }

    panic("vm_fault hasn't been written yet\n");

    return EFAULT;

    // called on a TLB refill miss.
    // vm_fault -> vm_fault READONLY -> yes -> EFAULT
    //             no ->   lookup PT -> yes -> load TLB
    //                     no ->   lookup region -> valid -> allocate frame, zero fill, insert PTE -> load TLB
    //                             non-valid ->    EFAULT

    // allocate frame can be done with alloc_kpage

    // tlb_random() can be used for TLB refill. Disable interrupts locally for tlb_random().

    // we can access the address space through curproc->addrspace see dumbvm.c.
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

