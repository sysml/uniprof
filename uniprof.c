#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include "xenctrl.h"

#ifdef DEBUG
#define DBG(args...) printf(args)
#else
#define DBG(args...)
#endif /* DEBUG */

// big enough for 32 bit and 64 bit
typedef uint64_t guest_word_t;

typedef struct mapped_page {
	guest_word_t base; // page number, i.e. addr>>XC_PAGE_SHIFT
	unsigned long mfn;
	void *buf;
	struct mapped_page *next;
} mapped_page_t;

static int get_word_size(xc_interface *xc_handle, int domid) {
	//TODO: support for HVM
	unsigned int guest_word_size;

	if (xc_domain_get_guest_width(xc_handle, domid, &guest_word_size))
		return -1;
	return guest_word_size;
}

static guest_word_t frame_pointer(vcpu_guest_context_any_t *vc, int wordsize) {
	// only possible word sizes are 4 and 8, everything else leads to an
	// early exit during initialization, since we can't handle it
	if (wordsize == 4)
		return vc->x32.user_regs.ebp;
	else
		return vc->x64.user_regs.rbp;
}

static guest_word_t instruction_pointer(vcpu_guest_context_any_t *vc, int wordsize) {
	//TODO: currently no support for real-mode 32 bit
	if (wordsize == 4)
		return vc->x32.user_regs.eip;
	else
		return vc->x64.user_regs.rip;
}

void *guest_to_host(xc_interface *xc_handle, int domid, int vcpu, guest_word_t gaddr) {
	static mapped_page_t *map_head = NULL;
	mapped_page_t *map_iter;
	mapped_page_t *new_item;
	guest_word_t base = gaddr & XC_PAGE_MASK;
	guest_word_t offset = gaddr & ~XC_PAGE_MASK;

	map_iter = map_head;
	while (map_iter != NULL) {
		if (base == map_iter->base)
			return map_iter->buf + offset;
		map_iter = map_iter->next;
	}

	// no matching page found, we need to map a new one.
	// At this pointer, map_iter conveniently points to the last item.
	new_item = malloc(sizeof(mapped_page_t));
	if (new_item == NULL) {
		fprintf(stderr, "failed to allocate memory for page struct.\n");
		return NULL;
	}
	new_item->base = base;
	new_item->mfn = xc_translate_foreign_address(xc_handle, domid, vcpu, base);
	new_item->buf = xc_map_foreign_range(xc_handle, domid, XC_PAGE_SIZE, PROT_READ, new_item->mfn);
	DBG("mapping new page %#"PRIx64"->%p\n", new_item->base, new_item->buf);
	if (new_item->buf == NULL) {
		fprintf(stderr, "failed to allocate memory mapping page.\n");
		return NULL;
	}
	new_item->next = NULL;
	if (map_head == NULL)
		map_head = new_item;
	else
		map_iter->next = new_item;
	return new_item->buf + offset;
}

void walk_stack(xc_interface *xc_handle, int domid, int vcpu, int wordsize) {
	int ret;
	guest_word_t fp, retaddr;
	void *hfp;
	vcpu_guest_context_any_t vc;

	DBG("tracing vcpu %d\n", vcpu);
	if ((ret = xc_vcpu_getcontext(xc_handle, domid, vcpu, &vc)) < 0) {
		printf("Failed to get context for VCPU %d, skipping trace. (ret=%d)\n", vcpu, ret);
		return;
	}

	// our first "return" address is the instruction pointer
	retaddr = instruction_pointer(&vc, wordsize);
	fp = frame_pointer(&vc, wordsize);
	while (fp != 0) {
		hfp = guest_to_host(xc_handle, domid, vcpu, fp);
		DBG("vcpu %d, fp = %#"PRIx64"->%p->%#"PRIx64", return addr = %#"PRIx64"\n",
				vcpu, fp, hfp, *((uint64_t*)hfp), retaddr);
		printf("%#"PRIx64"\n", retaddr);
		// walk the frame pointers: new fp = content of old fp
		memcpy(&fp, hfp, wordsize);
		// and return address is always the next address on the stack
		memcpy(&retaddr, hfp+wordsize, wordsize);
		DBG("after: return addr = %#"PRIx64", fp = %#"PRIx64"\n", retaddr, fp);
	}
	printf("1\n\n");
}

/**
 * returns 0 on success.
 */
int do_stack_trace(xc_interface *xc_handle, int domid, xc_dominfo_t *dominfo, int wordsize) {
	unsigned int vcpu;

	if (xc_domain_pause(xc_handle, domid) < 0) {
		fprintf(stderr, "Could not pause domid %d\n", domid);
		return 6;
	}
	for (vcpu = 0; vcpu <= dominfo->max_vcpu_id; vcpu++) {
		walk_stack(xc_handle, domid, vcpu, wordsize);
	}
	if (xc_domain_unpause(xc_handle, domid) < 0) {
		fprintf(stderr, "Could not unpause domid %d\n", domid);
		return 6;
	}
	return 0;
}

int main(int argc, char **argv) {
	int domid, ret;
	xc_interface *xc_handle;
	xc_dominfo_t dominfo;
	int wordsize;

	if (argc < 2) {
		fprintf(stderr, "usage: uniprof <domid>\n");
		return 1;
	}

	domid = strtol(argv[1], NULL, 10);
	if (domid == 0) {
		fprintf(stderr, "invalid domid (unparseable domid string or cannot trace dom0)\n");
		return 2;
	}

	xc_handle = xc_interface_open(0,0,0);
	if (xc_handle == NULL) {
		fprintf(stderr, "Cannot connect to the hypervisor. (Is this Xen?)\n");
		return 3;
	}

	ret = xc_domain_getinfo(xc_handle, domid, 1, &dominfo);
	if (ret < 0) {
		fprintf(stderr, "Could not access information for domid %d. (Does domid %d exist?)\n", domid, domid);
		return 4;
	}

	wordsize = get_word_size(xc_handle, domid);
	if (wordsize < 0) {
		fprintf(stderr, "Failed to retrieve word size for domid %d\n", domid);
		return 5;
	}
	else if ((wordsize != 8) && (wordsize != 4)) {
		fprintf(stderr, "Unexpected wordsize (%d) for domid %d, cannot trace.\n", wordsize, domid);
		return 5;
	}
	DBG("wordsize is %d\n", wordsize);

	ret = do_stack_trace(xc_handle, domid, &dominfo, wordsize);
	if (ret) {
		return ret;
	}

	return 0;
}
