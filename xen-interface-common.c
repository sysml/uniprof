/*
 * uniprof: architecture-independent Xen interface functions
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

int get_domain_state(int domid, unsigned int *state) {
	int retval;
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_getdomaininfo;
	retval = xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
	*state = domctl.u.getdomaininfo.flags;
	return retval;
#elif defined(HYPERCALL_LIBXC)
	xc_dominfo_t info;
	retval = xc_domain_getinfo(xc_handle, domid, 1, &info);
	*state |= (info.shutdown_reason << XEN_DOMINF_shutdownshift);
	if (info.dying)
		*state |= XEN_DOMINF_dying;
	if (info.hvm)
		*state |= XEN_DOMINF_hvm_guest;
	if (info.shutdown || info.crashed)
		*state |= XEN_DOMINF_shutdown;
	if (info.paused)
		*state |= XEN_DOMINF_paused;
	if (info.blocked)
		*state |= XEN_DOMINF_blocked;
	if (info.running)
		*state |= XEN_DOMINF_running;
	if (info.debugged)
		*state |= XEN_DOMINF_debugged;
	if (retval == 1)
		return 0;
	return retval;
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

int get_max_vcpu_id(int domid, unsigned int *max_vcpu_id) {
	int ret;
#if defined(HYPERCALL_XENCALL)
	struct xen_domctl domctl;
	domctl.domain = (domid_t)domid;
	domctl.interface_version = XEN_DOMCTL_INTERFACE_VERSION;
	domctl.cmd = XEN_DOMCTL_getdomaininfo;
	ret = xencall1(callh, __HYPERVISOR_domctl, (unsigned long)(&domctl));
	*max_vcpu_id = domctl.u.getdomaininfo.max_vcpu_id;
	return ret;
#elif defined(HYPERCALL_LIBXC)
	xc_dominfo_t dominfo;
	ret = xc_domain_getinfo(xc_handle, domid, 1, &dominfo);
	*max_vcpu_id = domctl.u.getdomaininfo.max_vcpu_id;
	return ret;
#endif
}
