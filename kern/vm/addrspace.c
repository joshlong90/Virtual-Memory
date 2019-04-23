/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

/* Called by a new process, sets up structures necessary to represent new process. */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/* Initialise the regions linked list to be empty. */
	as->regions = NULL;

	/* Initialise the 2-level page table by allocating memory for the 1st level table. */
	//as->pagetable = kmalloc(TABLE_SIZE * sizeof(paddr_t *));
	as->pagetable = kmalloc(TABLE_SIZE * sizeof(paddr_t *));
	if (as->pagetable == NULL) {
		kfree(as);
		return NULL;
	}

	/* zero fill the 1st-level table */
	unsigned int i;
	for (i = 0; i < TABLE_SIZE; i++) {
		as->pagetable[i] = NULL;
	}

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	// kprintf("========== AS COPY CALLED\n");
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}
	KASSERT(newas->pagetable != NULL);

	/*
	 * Write this.
	 */
	// for each page in new copy, it needs to be mapped to its own unique copy of the data that was in the frame.
	// called by fork. create new page table that points to new fresh frames.
	// copy the content of all frames pointed to by the old parent page table.
	// nested for loop two loops in a row.
	// adds all the same regions in parent.

	struct region *cur_reg;
	int permissions, readable, writeable, executable, result;
	int i, j;
	size_t memsize;
	vaddr_t vaddr;
	cur_reg = old->regions;

	/* copy the permissions and region structure */
	while (cur_reg != NULL) {
		permissions = cur_reg->permissions;
		readable    = permissions & RF_R;
		writeable   = permissions & RF_W;
		executable  = permissions & RF_X;
		memsize = cur_reg->reg_npages * PAGE_SIZE;
		vaddr = cur_reg->reg_vbase;
		result = as_define_region(newas, vaddr, memsize, readable, writeable, executable);
		if (result != 0) {
			return result;
		}

		cur_reg = cur_reg->reg_next;
	}

	paddr_t paddr, entryLo;
	for (i = 0; i < TABLE_SIZE; i++) {
		if (old->pagetable[i] != NULL) {
			newas->pagetable[i] = (paddr_t *)alloc_kpages(1);
			if (newas->pagetable[i] == NULL) {
				return ENOMEM;
			}
			for (j = 0; j < TABLE_SIZE; j++) {
				if (old->pagetable[i][j] != 0) {
					/* allocate a new frame for the copied entry. */
					paddr = (paddr_t)alloc_kpages(1);
					if (paddr == 0) {
						return ENOMEM;
					}
					/* copy data from old frame to new frame. */
					memmove((void *)paddr, (const void *)(PADDR_TO_KVADDR(old->pagetable[i][j]) & PAGE_FRAME), (size_t)PAGE_SIZE);
					/* copy permissions from old entry to new pagetable entry */
					entryLo = KVADDR_TO_PADDR(paddr) | TLBLO_VALID;
					if ((old->pagetable[i][j] & TLBLO_DIRTY) != 0) {
						entryLo |= TLBLO_DIRTY;
					}
					/* insert entry in pagetable. */
					newas->pagetable[i][j] = entryLo;
				} else {
					newas->pagetable[i][j] = 0;
				}
			}
		} else {
			newas->pagetable[i] = NULL;
		}
	}

	// kprintf("========== AS COPY FINISHED\n");
	/* copy the necessary page data to the destination */

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */
	unsigned int i, j;
	struct region *cur_reg;
	struct region *next_reg;

	/* free all 2nd level tables in page table */
	for (i = 0; i < TABLE_SIZE; i++) {
		if (as->pagetable[i] != NULL) {
			for (j = 0; j < TABLE_SIZE; j++) {
				if (as->pagetable[i][j] != 0) {
					//kprintf("free address 0x%08x, virtual address 0x%08x\n", as->pagetable[i][j] & PAGE_FRAME, i<<22 | j<<12);
					free_kpages((paddr_t)(PADDR_TO_KVADDR(as->pagetable[i][j]) & PAGE_FRAME));
				}
			}
			free_kpages((vaddr_t)as->pagetable[i]);
		}
	}
	/* free 1st level table in page table */
	free_kpages((vaddr_t)as->pagetable);

	/* free all regions in linked list */
	cur_reg = as->regions;
	while(cur_reg != NULL) {
		next_reg = cur_reg->reg_next;
		kfree(cur_reg);
		cur_reg = next_reg;
	}

	kfree(as);
}

void as_activate(void) {
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void as_deactivate(void) {
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	/* print the parameters for debug purposes */
	// kprintf("vaddr: 0x%08x, memsize: %d, r: 0x%08x, w: 0x%08x, e: 0x%08x\n", vaddr, memsize, readable, writeable, executable);

	struct region *cur_reg;
	struct region *new_reg;
	size_t npages;

	if ((readable | writeable | executable) == 0) {
		return EINVAL;
	}

	/* find the base location in virtual memory for the region. */
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* find the number of pages required for the region. */
	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;
	npages = memsize / PAGE_SIZE;

	/* allocate memory for new region */
	new_reg = kmalloc(sizeof(struct region));
	if (new_reg == NULL) {
		return ENOMEM;
	}

	/* save new region attributes */
	new_reg->reg_npages = npages;
	new_reg->reg_vbase = vaddr;
	new_reg->reg_next = NULL;
	new_reg->permissions = readable | writeable | executable;

	/* load the new region to the end of the linked list of regions. */
	if (as->regions == NULL) {
		as->regions = new_reg;
		KASSERT(as->regions != NULL);
		return 0;
	}

	cur_reg = as->regions;
	while(cur_reg->reg_next != NULL) {
		cur_reg = cur_reg->reg_next;
	}
	cur_reg->reg_next = new_reg;

	cur_reg = as->regions;
	while(cur_reg != NULL) {
		/* print the regions for debug purposes */
		// kprintf("vbase: 0x%08x, npages: %d, permissions: 0x%08x\n", cur_reg->reg_vbase, cur_reg->reg_npages, cur_reg->permissions);
		cur_reg = cur_reg->reg_next;
	}

	KASSERT(as->regions != NULL);

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	// make read only regions read/write for loading purposes.
	// the TLB is responsible for accesses and enforcing permissions. 
	// Make sure all loaded pages to TLB are writeable.
	// kprintf("as_prepare_load called\n");
	KASSERT(as != NULL);
	KASSERT(as->regions != NULL);

	struct region *cur_reg;
	cur_reg = as->regions;

	// set read / write permissions for all regions
	// shifting the old permissions to the left by 3 bits
	while (cur_reg != NULL) {
		cur_reg->permissions = cur_reg->permissions << 3 | RF_R | RF_W;
		cur_reg = cur_reg->reg_next;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	int i, spl;
	KASSERT(as != NULL);
	KASSERT(as->regions != NULL);
	struct region *cur_reg;
	cur_reg = as->regions;

	// reset write permissions to what they were originally
	// by shifting the original right permissions to the right
	// ANDing with 0b00000111 to ensure only the relevant bits are set for tidyness

	while (cur_reg != NULL) {
		cur_reg->permissions = cur_reg->permissions >> 3 & 0x7;
		/* update all readonly regions in the pagetable. */
		if ((cur_reg->permissions & RF_W) == 0) {
			pagetable_update(as->pagetable, cur_reg->reg_vbase, cur_reg->reg_npages);
		}
		cur_reg = cur_reg->reg_next;
	}

	/* flush the TLB. */
	spl = splhigh();
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	// kprintf("as_define_stack called\n");
	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	/* define the size of the stack. */
	size_t memsize = STACK_NPAGES*PAGE_SIZE;

	/* define the base virtual memory location of the stack. */
	vaddr_t vaddr = USERSTACK - memsize;

	/* setup permissions for stack: read/write, not executable. */
	int readable = RF_R;
	int writable = RF_W;
	int executable = 0;

	/* define the stack region within the address space. */
	int result = as_define_region(as, vaddr, memsize, readable, writable, executable); if (result != 0) {
		return result;
	}

	return 0;
}

