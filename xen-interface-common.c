#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <string.h>
#include <inttypes.h>
#include <xen-interface.h>

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
