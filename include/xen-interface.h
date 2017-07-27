/*
 * uniprof: Xen interface
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

#ifndef XEN_INTERFACE_H
#define XEN_INTERFACE_H

#include <config.h>

#if defined(HYPERCALL_XENCALL) + defined(HYPERCALL_LIBXC) == 0
#error Define exactly one of HYPERCALL_LIBXC, HYPERCALL_XENCALL
#elif defined(HYPERCALL_XENCALL) + defined(HYPERCALL_LIBXC) > 1
#warning You defined more than one of HYPERCALL_LIBXC, HYPERCALL_XENCALL. This might lead to unexpected results.
#endif

#undef DBG
#ifdef DEBUG
#define DBG(string, args...) printf("[DBG %s:%s():%d] "string, __FILE__, __func__, __LINE__, ##args)
#else
#define DBG(args...)
#endif /* DEBUG */

#if defined(HYPERCALL_XENCALL)
#include <xenctrl.h>
#include <xencall.h>
#include <xenforeignmemory.h>
#define HYPERCALL_NAME "libxencall"
typedef vcpu_guest_context_t vcpu_guest_context_transparent_t;
extern xencall_handle *callh;
extern xenforeignmemory_handle *fmemh;
#elif defined(HYPERCALL_LIBXC)
#define XC_WANT_COMPAT_MAP_FOREIGN_API
#include <xenctrl.h>
#define HYPERCALL_NAME "libxc"
typedef vcpu_guest_context_any_t vcpu_guest_context_transparent_t;
extern xc_interface *xc_handle;
#endif

#ifndef _maybe_unused
#define _maybe_unused __attribute__((unused))
#endif

#define PAGE_SHIFT XC_PAGE_SHIFT
#define PAGE_SIZE  XC_PAGE_SIZE
#define PAGE_MASK  XC_PAGE_MASK

// big enough for 32 bit and 64 bit
typedef uint64_t guest_word_t;

int xen_interface_open(void);
int xen_interface_close(void);
int get_word_size(int domid);
guest_word_t instruction_pointer(vcpu_guest_context_transparent_t *vc);
guest_word_t frame_pointer(vcpu_guest_context_transparent_t *vc);
int get_vcpu_context(int domid, int vcpu, vcpu_guest_context_transparent_t *vc);
void xen_map_domu_page(int domid, int vcpu, uint64_t addr, unsigned long *mfn, void **buf);
int get_domain_state(int domid, unsigned int *state);
int pause_domain(int domid);
int unpause_domain(int domid);
int get_max_vcpu_id(int domid);

#endif /* XEN_INTERFACE_H */
