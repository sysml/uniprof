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

/* On x86, we might have 32-bit domains running on 64-bit machines,
 * so we ask the hypervisor. On ARM, we simply return arch size. */
int get_word_size(int __maybe_unused domid) {
#if defined(__arm__)
	return 4;
#elif defined(__aarch64__)
	return 8;
#else
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
#endif /* call type */
#endif /* architecture */
}


#if defined(HYPERCALL_XENCALL)
/* libxenforeignmemory doesn't provide an address translation method like libxc does,
 * so it needs a replacement function to walk the page tables.
 */
#if (defined(__i386__) || defined(__x86_64__))
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
#elif defined(__arm__)
unsigned long xen_translate_foreign_address(int domid, int vcpu, unsigned long long virt)
{
#define ARM_PT_BASE_LENGTH 18
#define ARM_PT_SECTION_LENGTH 12
	vcpu_guest_context_t ctx;
	uint32_t pt_base_addr;
	uint32_t addr, offset;
	unsigned int N; /* N as defined in the the ARM TCBR specification */
	int err, entry_type;
	void *map;

	get_vcpu_context(domid, vcpu, &ctx);
	N = ctx.ttbcr & 0x7;
	pt_base_addr = ctx.ttbr0 & ~((1<<(32-ARM_PT_BASE_LENGTH+N))-1);
	addr = pt_base_addr>>PAGE_SHIFT;
	DBG("TTCBR N = 0x%x, page table base address = 0x%x (frame number 0x%x)\n", N, pt_base_addr, addr);

	map = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, (xen_pfn_t *)&addr, &err);
	DBG("mapped page table base 0x%x to %p, err = %d\n", pt_base_addr, map, err);

	/* We take bits 31 to (14-N) from TTBR0 (i.e., pt_base_addr) and map them to 31..(14-N).
	 * We then take bits (31-N) to 20 from the virtual address and map them to (13-N)..2.
	 * Bits 1..0 are 0x0. */
	addr = (virt & (~((1<<(12-N))-1))) >> 20;
	DBG("PT virt part is 0x%x\n", addr);
	addr = pt_base_addr + (addr<<2);
	offset = addr - pt_base_addr;
	DBG("address-to-lookup is 0x%x, offset to base is 0x%x\n", addr, addr-pt_base_addr);

	memcpy(&addr, map + offset, 4);
	DBG("content of %p is 0x%x\n", map + offset, addr);
	/* we now have to check which type of table entry this is */
	entry_type = addr & 0x3;
	if (entry_type == 0x2) {
		/* section entry, directly tells us virt -> mfn translation in bits 31..20 */
		//memcpy(&addr, map + offset, 4);
		addr >>= 20;
		return addr;
	}
	else if (entry_type == 0x0) {
		/* page fault. Should never happen, since we want to look at used memory. */
		printf("Page fault while trying to resolve guest address!\n");
		return 0;
	}
	else {
		/* If you thought this was a complete implementation,
		 * boy, do I have bad news for you! */
		return 0;
	}
}
#endif /* architecture */
#endif /* HYPERCALL_XENCALL */

void xen_map_domu_page(int domid, int vcpu, uint64_t addr, unsigned long *mfn, void **buf) {
	int err __maybe_unused = 0;
	DBG("mapping page for virt addr %"PRIx64"\n", addr);
#if defined(HYPERCALL_XENCALL)
#if defined(__i386__) || defined (__x86_64__)
	*mfn = xen_translate_foreign_address(domid, vcpu, addr);
#elif defined(__arm__)
	*mfn = xen_translate_foreign_address(domid, vcpu, addr);
#else
#error "Unsupported architecture"
#endif
	// This works since size is 1, so the array has size 1, so it's just a pointer to an int
	*buf = xenforeignmemory_map(fmemh, domid, PROT_READ, 1, (xen_pfn_t *)mfn, &err);
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
#elif defined(__arm__)
	// this only works for ARM mode so far!
	// also, it might not work at all on AACPI ABI!
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.r11_usr;
#elif defined(HYPERCALL_LIBXC)
	return vc->c.user_regs.r11_usr;
#endif
#else
#error "Unsupported architecture"
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
#elif defined(__arm__)
	// this only works for ARM mode so far!
#if defined(HYPERCALL_XENCALL)
	return vc->user_regs.pc32;
#elif defined(HYPERCALL_LIBXC)
	return vc->c.user_regs.pc32;
#endif
#else
#error "Unsupported architecture"
#endif /* architecture */
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
