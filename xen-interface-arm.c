/*
 * uniprof: ARM-specfic Xen interface functions
 *
 * Authors: Florian Schmidt <florian.schmidt@neclab.eu>
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */

#include <sys/mman.h>
#include <string.h>
#include <inttypes.h>
#include "xen-interface.h"

/* On x86, we might have 32-bit domains running on 64-bit machines,
 * so we ask the hypervisor. On ARM, we simply return arch size. */
int get_word_size(domid_t _maybe_unused domid, unsigned int *wordsize) {
#if defined(__arm__)
	*wordsize = 4;
#elif defined(__aarch64__)
	*wordsize = 8;
#endif
	return 0;
}

#if defined(HYPERCALL_XENCALL)
xen_pfn_t xen_translate_foreign_address(domid_t domid, unsigned int vcpu, unsigned long long virt)
{
	vcpu_guest_context_t ctx;
	xen_pfn_t pt_base_addr;
	unsigned int arm_pt_base_length = 18;
	unsigned int arm_pt_index_length = 12;
	xen_pfn_t addr, offset;
	unsigned int N; /* N as defined in the the ARM TTBCR specification */
	int err, entry_type;
	void *map;

	get_vcpu_context(domid, vcpu, &ctx);
	/* N defines the split between the two page tables. If all the most-significant
	 * N bits of a virtual address are 0, then use page table 0, otherwise use
	 * page table 1. */
	N = ctx.ttbcr & 0x7;
	if (virt & (N<<29)) {
		fprintf(stderr, "warning: TTBR1 support not tested at all!\n");
		pt_base_addr = ctx.ttbr1 & ~((1UL<<(32-arm_pt_base_length))-1);
	}
	else {
		/* Update translate base and table index width from their
		 * default values according to N. */
		arm_pt_base_length += N;
		arm_pt_index_length -= N;
		pt_base_addr = ctx.ttbr0 & ~((1UL<<(32-arm_pt_base_length))-1);
	}
	addr = pt_base_addr>>PAGE_SHIFT;
	map = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, &addr, &err);
	if (err)
		goto out_unmap;
	DBG("mapped page table base 0x%"PRI_xen_pfn" to %p, err = %d\n", pt_base_addr, map, err);

	/* See ARMv7 Reference Manual, Figure B3-9, or B3-10 on how to get a first-level
	 * descriptor address:
	 * We take bits 31 to (14-N) from TTBR0 (i.e., pt_base_addr) and map them to 31..(14-N).
	 * We then take bits (31-N) to 20 from the virtual address and map them to (13-N)..2.
	 * Bits 1..0 are 0x0. */
	addr = (virt & (~((1UL<<arm_pt_index_length)-1))) >> 20;
	DBG("PT virt part is 0x%"PRI_xen_pfn"\n", addr);
	addr = pt_base_addr + (addr<<2);
	offset = addr - pt_base_addr;
	DBG("address-to-lookup is 0x%"PRI_xen_pfn", offset to base is 0x%"PRI_xen_pfn"\n", addr, addr-pt_base_addr);

	memcpy(&addr, map + offset, 4);
	DBG("content of %p is 0x%"PRI_xen_pfn"\n", map + offset, addr);

	/* we now have to check which type of table entry this is */
	entry_type = addr & 0x3;
	DBG("entry type is 0x%x\n", entry_type);
	switch (entry_type) {
		case 0x0:
			/* page fault. Should never happen, since we want to look at used memory. */
			fprintf(stderr, "Page fault while trying to resolve guest address!\n");
			goto out_unmap;
		case 0x1:
			/* Large page. We need to do a second-level lookup. (cf. Fig. B3-10)
			 * The page table address base (bits 31..10) is in addr[31..10], the
			 * L2 table index (bits 9..2) is in virt[19..12]. Bits 1..0 are 0x0. */
			fprintf(stderr, "Warning: multi-level page walking code not tested at all!\n");
			addr &= (addr & 0xFFFFFC00);
			addr |= ((virt & 0xFF000)>>10);
			if ((addr>>PAGE_SHIFT) != (pt_base_addr>>PAGE_SHIFT)) {
				/* New address is in a different page, get that one. */
				xenforeignmemory_unmap(fmemh, map, 1);
				map = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, (xen_pfn_t *)&addr, &err);
				if (err)
					goto out_unmap;
			}
			offset = addr - pt_base_addr;
			memcpy(&addr, map + offset, 4);
			/* For the output address, the base (bits 31..16) is in addr[31..16], the
			 * page index (bits 15..0) is in virt[15..0]. So splice them together.
			 * The astute reader will notice there is an overlap, and bits 12..15
			 * are used both in the second-level lookup and as part of the address. */
			addr = (addr & ~((1UL<<16)-1)) | (virt & ((1UL<<16)-1));
			break;
		case 0x2:
			/* Section entry. We're done with lookups. (cf. Fig. B3-9)
			 * The base (bits 31..20) is in addr[31..20], the
			 * index (bits 19..0) is in virt[19..0]. So splice them together. */
			addr = (addr & ~((1UL<<20)-1)) | (virt & ((1UL<<20)-1));
			break;
		case 0x3:
			/* Small page. We need to do a second-level lookup (cf. Fig. B3-11)
			 * This first step is exactly the same as for large pages above. */
			fprintf(stderr, "Warning: multi-level page walking code not tested at all!\n");
			addr &= (addr & 0xFFFFFC00);
			addr |= ((virt & 0xFF000)>>10);
			if ((addr>>PAGE_SHIFT) != (pt_base_addr>>PAGE_SHIFT)) {
				/* New address is in a different page, get that one. */
				xenforeignmemory_unmap(fmemh, map, 1);
				map = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, (xen_pfn_t *)&addr, &err);
				if (err)
					goto out_unmap;
			}
			offset = addr - pt_base_addr;
			memcpy(&addr, map + offset, 4);
			/* For the output address, the base (bits 31..12) is in addr[31..12], the
			 * page index (bits 11..0) is in virt[11..0]. So splice them together. */
			addr = (addr & ~((1UL<<12)-1)) | (virt & ((1UL<<12)-1));
			break;
	}
	/* We now have the machine addres. But actually, we want an
	 * MFN, so shift the address accordingly. */
	addr >>= PAGE_SHIFT;
	DBG("found section entry for %llx to mfn 0x%"PRI_xen_pfn"\n", virt, addr);
	xenforeignmemory_unmap(fmemh, map, 1);
	return addr;

out_unmap:
	xenforeignmemory_unmap(fmemh, map, 1);
	fprintf(stderr, "error trying to map domU memory (bogus frame pointers?)\n");
	return 0;
}
#endif /* HYPERCALL_XENCALL */

void xen_map_domu_page(domid_t domid, unsigned int vcpu, uint64_t addr, xen_pfn_t *mfn, void **buf) {
	int err _maybe_unused = 0;
	DBG("mapping page for virt addr %"PRIx64"\n", addr);
#if defined(HYPERCALL_XENCALL)
	*mfn = xen_translate_foreign_address(domid, vcpu, addr);
	if (*mfn) {
		// This works since size is 1, so the array has size 1, so it's just a pointer to an int
		*buf = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, mfn, &err);
		if (err) {
			xenforeignmemory_unmap(fmemh, *buf, 1);
			*buf = 0;
		}
	}
	else {
		*buf = 0;
	}
#elif defined(HYPERCALL_LIBXC)
	*mfn = xc_translate_foreign_address(xc_handle, domid, vcpu, addr);
	DBG("addr = %"PRIx64", mfn = %lx\n", addr, *mfn);
	*buf = xc_map_foreign_range(xc_handle, domid, XC_PAGE_SIZE, PROT_READ, *mfn);
#endif
	DBG("virt addr %"PRIx64" has mfn %"PRI_xen_pfn" and was mapped to %p\n", addr, *mfn, *buf);
}

guest_word_t frame_pointer(const vcpu_guest_context_transparent_t *vc) {
	// this only works for ARM mode so far!
	// also, it might not work at all on AACPI ABI!
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.r11_usr;
#elif defined(HYPERCALL_LIBXC)
	return vc->c.user_regs.r11_usr;
#endif
}

guest_word_t instruction_pointer(const vcpu_guest_context_transparent_t *vc) {
	// this only works for ARM mode so far!
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.pc32;
#elif defined(HYPERCALL_LIBXC)
	return vc->c.user_regs.pc32;
#endif
}
