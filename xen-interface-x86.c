/*
 * uniprof: x86-specific Xen interface functions
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

#define _GNU_SOURCE 1
#include <string.h>
#include <sys/mman.h>
#include <inttypes.h>
#include "xen-interface.h"

/* On x86, we might have 32-bit domains running on 64-bit machines,
 * so we ask the hypervisor. On ARM, we simply return arch size. */
int get_word_size(domid_t domid, unsigned int *wordsize) {
	//TODO: support for HVM
#if defined(HYPERCALL_XENCALL)
	int ret;
	struct xen_domctl domctl;
	domctl.domain = domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_get_address_size;
	ret = xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
	*wordsize = domctl.u.address_size.size / 8;
	return ret;
#elif defined(HYPERCALL_LIBXC)
	return xc_domain_get_guest_width(xc_handle, domid, wordsize);
#endif
}

#if defined(HYPERCALL_XENCALL)
/* libxenforeignmemory doesn't provide an address translation method like libxc does,
 * so it needs a replacement function to walk the page tables.
 */
unsigned long xen_translate_foreign_address(domid_t domid, unsigned int vcpu, unsigned long long virt)
{
	vcpu_guest_context_t ctx;
	unsigned int wordsize;
	int levels, i, err;
	uint64_t addr, mask, clamp, offset;
	xen_pfn_t pfn;
	void *map;

	get_vcpu_context(domid, vcpu, &ctx);
	if (get_word_size(domid, &wordsize))
		return 0;

	if (wordsize == 8) {
		/* 64-bit has a 4-level page table */
		levels = 4;
		/* clamp values to 48 bit virtual address range */
		clamp = (1ULL<<48) - 1;
		addr = (uint64_t)xen_cr3_to_pfn_x86_64(ctx.ctrlreg[3]) << PAGE_SHIFT;
		addr &= clamp;
	}
	else {  /* wordsize == 4, any weird other values throw and error much earlier */
		/* 32-bit has a 3-level page table */
		levels = 3;
		/* clamp value to 32 bit address range */
		clamp = (1ULL<<32) - 1;
		addr = (uint32_t)xen_cr3_to_pfn_x86_32(ctx.ctrlreg[3]) << PAGE_SHIFT;
	}
	DBG("page table base address is 0x%lx\n", addr);
	/* See AMD64 Architecture Programmer's Manual, Volume 2: System Programming,
	 * rev 3.22, p. 127, Fig. 5-9 for 32-bit and p, 132, Fig. 5-17 for 64-bit. */
	/* Each page table considers a 9-bit range.
	 * The lowest level considers bits 12-21 (hence the <<12 shift),
	 * each higher level the next-significant 9 bits.
	 * Note that the highest level is truncated and only considers
	 * 2 bits for 32-bit archictures. */
	mask = ((((1ULL<<9)-1) << 12) << ((levels-1)*9));

	for (i = levels; i > 0; i--) {
		/* See AMD64 Architecture Programmer's Manual, Volume 2: System
		 * Programming, rev 3.22, p. 128, Figs. 5-10 to 5-12 for 32-bit
		 * and p, 133, Figs. 5-18 to 5-21 for 64-bit. */
		/* Take the respective bits for the level, shift them to the
		 * very right, and interpret them as a "page table entry number".
		 * Since PTEs are 8 bytes for both 64-bit and 32-bit (Xen doesn't
		 * seem to emulate legacy non-PAE setups with 4-byte PTEs),
		 * multiply the page table entry number by 8. */
		offset = ((virt & mask) >> (ffsll(mask) - 1)) * 8;
		DBG("level %d page walk gives us offset 0x%lx\n", i, offset);
		/* But before we can read from there, we will need to map in that memory */
		pfn = addr >> PAGE_SHIFT;
		map = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, &pfn, &err);
		if (err)
			goto out_unmap;
		memcpy(&addr, map + offset, 8);
		xenforeignmemory_unmap(fmemh, map, 1);
		/* However, addr is not really an address right now, but rather a PTE,
		 * which contains the base address in bits 51..12. No shifting necessary,
		 * because the base address in the PTE is a PFN. */
		addr &= 0x000FFFFFFFFFF000ULL;
		DBG("level %d page table tells us to look at address 0x%lx\n", i, addr);
		/* Move the mask by 9 bits, and go on to the next round */
		mask >>= 9;
	}
	/* We now have the machine addres. But actually, we want an
	 * MFN, so shift the address accordingly. */
	addr >>= PAGE_SHIFT;
	DBG("found section entry for %llx to mfn 0x%lx\n", virt, addr);
	xenforeignmemory_unmap(fmemh, map, 1);
	return addr;

out_unmap:
	xenforeignmemory_unmap(fmemh, map, 1);
	return 0;
}
#endif /* HYPERCALL_XENCALL */

void xen_map_domu_page(domid_t domid, int vcpu, uint64_t addr, unsigned long *mfn, void **buf) {
	int err _maybe_unused = 0;
	DBG("mapping page for virt addr %"PRIx64"\n", addr);
#if defined(HYPERCALL_XENCALL)
	*mfn = xen_translate_foreign_address(domid, vcpu, addr);
	if (*mfn) {
		// This works since size is 1, so the array has size 1, so it's just a pointer to an int
		*buf = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, (xen_pfn_t *)mfn, &err);
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
	DBG("virt addr %"PRIx64" has mfn %lx and was mapped to %p\n", addr, *mfn, *buf);
}

guest_word_t frame_pointer(vcpu_guest_context_transparent_t *vc) {
	// only possible word sizes are 4 and 8, everything else leads to an
	// early exit during initialization, since we can't handle it
#if defined(__i386__)
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.ebp;
#elif defined(HYPERCALL_LIBXC)
	return vc->x32.user_regs.ebp;
#endif /* libxc/hypercall */
#elif defined(__x86_64__)
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.rbp;
#elif defined(HYPERCALL_LIBXC)
	return vc->x64.user_regs.rbp;
#endif /* libxc/hypercall */
#endif /* architecture */
}

guest_word_t instruction_pointer(vcpu_guest_context_transparent_t *vc) {
	//TODO: currently no support for real-mode 32 bit
#if defined(__i386__)
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.eip;
#elif defined(HYPERCALL_LIBXC)
	return vc->x32.user_regs.eip;
#endif /* libxc/hypercall */
#elif defined(__x86_64__)
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.rip;
#elif defined(HYPERCALL_LIBXC)
	return vc->x64.user_regs.rip;
#endif /* libxc/hypercall */
#endif /* architecture */
}
