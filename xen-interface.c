#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <string.h>
#include <inttypes.h>
#include "xen-interface.h"

#undef DBG
#ifdef DEBUG
#define DBG(args...) printf(args)
#else
#define DBG(args...)
#endif /* DEBUG */

#if defined(HYPERCALL_XENCALL)
xencall_handle *callh;
xenforeignmemory_handle *fmemh;
#endif
#if defined(HYPERCALL_LIBXC)
xc_interface *xc_handle;
#endif

int xen_interface_open(void) {
#if defined(HYPERCALL_XENCALL)
	callh = xencall_open(NULL, XENCALL_OPENFLAG_NON_REENTRANT);
	if (callh == NULL)
		return -1;
	fmemh = xenforeignmemory_open(NULL, 0);
	if (fmemh == NULL)
		return -2;
#endif
#if defined(HYPERCALL_LIBXC)
	xc_handle = xc_interface_open(0,0,0);
	if (xc_handle == NULL)
		return -1;
#endif
	return 0;
}

int xen_interface_close(void) {
#if defined(HYPERCALL_XENCALL)
	if (xenforeignmemory_close(fmemh))
		return -2;
	if (xencall_close(callh))
		return -1;
#elif defined(HYPERCALL_LIBXC)
	if (xc_interface_close(xc_handle))
		return -1;
#endif
	return 0;
}

int get_word_size(int domid) {
	//TODO: support for HVM
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_get_address_size;
	if (xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl)))
		return -1;
	return (domctl.u.address_size.size / 8);
#elif defined(HYPERCALL_LIBXC)
	unsigned int guest_word_size;

	if (xc_domain_get_guest_width(xc_handle, domid, &guest_word_size))
		return -1;
	return guest_word_size;
#endif
}


#if defined(HYPERCALL_XENCALL)
/* libxenforeignmemory doesn't provide an address translation method like libxc does,
 * so it needs a replacement function to walk the page tables.
 */
unsigned long xen_translate_foreign_address(int domid, int vcpu, unsigned long long virt)
{
	vcpu_guest_context_t ctx;
	int wordsize, levels;
	int i, err;
	uint64_t addr, mask, clamp, offset;
	xen_pfn_t pfn;
	void *map;

	get_vcpu_context(domid, vcpu, &ctx);
	wordsize = get_word_size(domid);

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
	return addr;
}
#endif /* HYPERCALL_XENCALL */

void xen_map_domu_page(int domid, int vcpu, uint64_t addr, unsigned long *mfn, void **buf) {
	DBG("mapping page for virt addr %"PRIx64"\n", addr);
#if defined(HYPERCALL_XENCALL)
	*mfn = xen_translate_foreign_address(domid, vcpu, addr);
	// This works since size is 1, so the array has size 1, so it's just a pointer to an int
	*buf = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, mfn, NULL);
#elif defined(HYPERCALL_LIBXC)
	*mfn = xc_translate_foreign_address(xc_handle, domid, vcpu, addr);
	*buf = xc_map_foreign_range(xc_handle, domid, XC_PAGE_SIZE, PROT_READ, *mfn);
#endif
}

guest_word_t frame_pointer(vcpu_guest_context_transparent_t *vc, int wordsize) {
	// only possible word sizes are 4 and 8, everything else leads to an
	// early exit during initialization, since we can't handle it
#if defined(HYPERCALL_XENCALL)
	if (wordsize == 4)
		return vc->user_regs.ebp;
	else
		return vc->user_regs.rbp;
#elif defined(HYPERCALL_LIBXC)
	if (wordsize == 4)
		return vc->x32.user_regs.ebp;
	else
		return vc->x64.user_regs.rbp;
#endif
}

guest_word_t instruction_pointer(vcpu_guest_context_transparent_t *vc, int wordsize) {
	//TODO: currently no support for real-mode 32 bit
#if defined(HYPERCALL_XENCALL)
	if (wordsize == 4)
		return vc->user_regs.eip;
	else
		return vc->user_regs.rip;
#elif defined(HYPERCALL_LIBXC)
	if (wordsize == 4)
		return vc->x32.user_regs.eip;
	else
		return vc->x64.user_regs.rip;
#endif
}

int get_vcpu_context(int domid, int vcpu, vcpu_guest_context_transparent_t *vc) {
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_getvcpucontext;
	domctl.u.vcpucontext.vcpu = (uint16_t)vcpu;
	domctl.u.vcpucontext.ctxt.p = (vcpu_guest_context_t *)vc;
	return xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
#elif defined(HYPERCALL_LIBXC)
	return xc_vcpu_getcontext(xc_handle, domid, vcpu, vc);
#endif
}

int pause_domain(int domid) {
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_pausedomain;
	return xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
#elif defined(HYPERCALL_LIBXC)
	return xc_domain_pause(xc_handle, domid);
#endif
}

int unpause_domain(int domid) {
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_unpausedomain;
	return xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
#elif defined(HYPERCALL_LIBXC)
	return xc_domain_unpause(xc_handle, domid);
#endif
}

int get_max_vcpu_id(int domid) {
#if defined(HYPERCALL_XENCALL)
	int ret;
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_getdomaininfo;
	ret = xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
	if (ret < 0)
		return -5;
	else
		return domctl.u.getdomaininfo.max_vcpu_id;
#elif defined(HYPERCALL_LIBXC)
	int ret;
	xc_dominfo_t dominfo;
	ret = xc_domain_getinfo(xc_handle, domid, 1, &dominfo);
	if (ret < 0)
		return -5;
	else
		return dominfo.max_vcpu_id;
#endif
}
