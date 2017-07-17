/*
 * uniprof, a stack tracer/profiler for Xen domains
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

#include <config.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <binsearch.h>
#include <xen-interface.h>
#ifdef WITH_UNWIND
#include <libunwind.h>
#include <libunwind-xen.h>
#endif

typedef struct mapped_page {
	guest_word_t base; // page number, i.e. addr>>PAGE_SHIFT
	unsigned long mfn;
	void *buf;
	struct mapped_page *next;
} mapped_page_t;

static bool verbose = false;
#define VERBOSE(args...) if (verbose) printf(args);

/* since some versions of sys/time.h do not include the
 * timespecadd/sub function, here's a macro (adapted from
 * the macros in sys/time.h) to do the job. */
#ifndef timespecadd
#define timespecadd(a, b, result)				\
do {								\
	(result)->tv_sec = (a)->tv_sec + (b)->tv_sec;		\
	(result)->tv_nsec = (a)->tv_nsec + (b)->tv_nsec;	\
	if ((result)->tv_nsec >= 1000000000) {			\
		++(result)->tv_sec;				\
		(result)->tv_nsec -= 1000000000;		\
	}							\
} while (0)
#endif /* timespecadd */
#ifndef timespecsub
#define timespecsub(a, b, result)				\
do {								\
	(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;		\
	(result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;	\
	if ((result)->tv_nsec < 0) {				\
		--(result)->tv_sec;				\
		(result)->tv_nsec += 1000000000;		\
	}							\
} while (0)
#endif /* timespecsub */
#ifndef timespeccmp
#define timespeccmp(a, b, CMP)					\
(((a)->tv_sec == (b)->tv_sec) ?					\
((a)->tv_nsec CMP (b)->tv_nsec) :				\
((a)->tv_sec CMP (b)->tv_sec))
#endif /* timespeccmp */
/* invert a negative value (e.g., -300usecs = -1.000700000)
 * to a positive value (300 usecs = 0.000300000). This is
 * useful to print negative timespec values. */
#define timespecnegtopos(a, b)					\
do {								\
	(b)->tv_sec = -((a)->tv_sec+1);				\
	(b)->tv_nsec = 1000000000 - (a)->tv_nsec;		\
} while (0)

static unsigned long get_time_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static void busywait(unsigned long nsecs)
{
	unsigned long deadline = get_time_nsec() + nsecs;
	do {
	} while (get_time_nsec() < deadline);
}

static void measure_overheads(struct timespec *gettime_overhead, struct timespec *minsleep, int rounds)
{
	int i;
	struct timespec before = { .tv_sec = 0, .tv_nsec = 0 };
	struct timespec after  = { .tv_sec = 0, .tv_nsec = 0 };
	struct timespec sleep  = { .tv_sec = 0, .tv_nsec = 0 };
	unsigned long long sleepsecs = 0, sleepnanosecs = 0, timesecs = 0, timenanosecs = 0;

	for (i=0; i<rounds; i++) {
		clock_gettime(CLOCK_MONOTONIC, &before);
		nanosleep(&sleep, NULL);
		clock_gettime(CLOCK_MONOTONIC, &after);
		timespecsub(&after, &before, &before);
		sleepsecs += before.tv_sec;
		sleepnanosecs += before.tv_nsec;
	}
	for (i=0; i<rounds; i++) {
		clock_gettime(CLOCK_MONOTONIC, &before);
		clock_gettime(CLOCK_MONOTONIC, &after);
		timespecsub(&after, &before, &before);
		timesecs += before.tv_sec;
		timenanosecs += before.tv_nsec;
	}
	gettime_overhead->tv_sec  = timesecs / rounds;
	gettime_overhead->tv_nsec = timenanosecs / rounds;
	minsleep->tv_sec          = (sleepsecs + timesecs) / rounds;
	minsleep->tv_nsec         = (sleepnanosecs + timenanosecs) / rounds;
}

int domain_shut_down(int domid) {
	const unsigned int dying_and_shutdown = XEN_DOMINF_dying | XEN_DOMINF_shutdown;
	unsigned int domstate;

	// error handling: if get_domain_state() signals an error (e.g.,
	// from the hypercall), we signal that the domain is shut down,
	// because we won't be able to do anything sensible with a domain
	// on which hypercalls fail.
	if (get_domain_state(domid, &domstate))
		return 1;
	if ((domstate & dying_and_shutdown) == dying_and_shutdown)
		return 1;

	return 0;
}

void *guest_to_host(int domid, int vcpu, guest_word_t gaddr) {
	static mapped_page_t *map_head = NULL;
	mapped_page_t *map_iter;
	mapped_page_t *new_item;
	guest_word_t base = gaddr & PAGE_MASK;
	guest_word_t offset = gaddr & ~PAGE_MASK;

	map_iter = map_head;
	while (map_iter != NULL) {
		if (base == map_iter->base)
			return map_iter->buf + offset;
		// preserve last item in map_iter by jumping out
		if (map_iter->next == NULL)
			break;
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
	xen_map_domu_page(domid, vcpu, base, &new_item->mfn, &new_item->buf);
	VERBOSE("mapping new page %#"PRIx64"->%p\n", new_item->base, new_item->buf);
	if (new_item->buf == NULL) {
		fprintf(stderr, "failed to allocate memory mapping page.\n");
		goto out_free;
	}
	if (new_item->mfn == 0) {
		fprintf(stderr, "failed to resolve virtual address.\n");
		goto out_free;
	}
	new_item->next = NULL;
	if (map_head == NULL)
		map_head = new_item;
	else
		map_iter->next = new_item;
	return new_item->buf + offset;

out_free:
	free(new_item);
	return NULL;
}

void resolve_and_print_symbol(void *symbol_table, guest_word_t address, FILE *file) {
	element_t *ele;

	if (!symbol_table) {
		fprintf(file, "%#"PRIx64"\n", address);
		return;
	}

	ele = binsearch_find_not_above(symbol_table, address);
	if (!ele)
		fprintf(file, "%#"PRIx64"\n", address);
	else {
		if (address == ele->key)
			fprintf(file, "%#"PRIx64"\n", address);
		else
			fprintf(file, "%s+%#"PRIx64"\n", ele->val.c, address - ele->key);
	}
}

void walk_stack_fp(int domid, int vcpu, int wordsize, FILE *file, void *symbol_table) {
	int ret;
	guest_word_t fp, retaddr;
	void *hfp, *hrp;
	vcpu_guest_context_transparent_t vc;

	DBG("tracing vcpu %d\n", vcpu);
	if ((ret = get_vcpu_context(domid, vcpu, &vc)) < 0) {
		printf("Failed to get context for VCPU %d, skipping trace. (ret=%d)\n", vcpu, ret);
		return;
	}

	// our first "return" address is the instruction pointer
	retaddr = instruction_pointer(&vc);
	fp = frame_pointer(&vc);
	DBG("vcpu %d, initial (register-based) fp = %#"PRIx64", retaddr = %#"PRIx64"\n", vcpu, fp, retaddr);
	while (fp != 0) {
		if (symbol_table)
			resolve_and_print_symbol(symbol_table, retaddr, file);
		else
			fprintf(file, "%#"PRIx64"\n", retaddr);
		/* walk the stack: on x86, the fp points to the address of the previous
		 * frame pointers, so new_fp = *old_fp. On ARM, the fp points to the
		 * first address of the next frame, with the frame pointer the first
		 * (=highest) value in the new frame, so new_fp = *old_fp-wordsize.
		 * The return address is the following word-sized values on the stack,
		 * so new_ret = new_fp + wordsize.
		 * We just have to be careful if the new values reside in different
		 * 4k pages. In that case, we have to map both separately, because
		 * they might not be in contiguous memory.
		 * Otherwise, we can just add wordsize to the fp and get retaddr. */
#if defined(__i386__) || defined(__x86_64__)
		hfp = guest_to_host(domid, vcpu, fp);
#elif defined(__arm__)
		hfp = guest_to_host(domid, vcpu, fp-wordsize);
#endif
		if (!hfp) {
			fprintf(file, "0\n\n");
			return;
		}
		if ((fp & PAGE_MASK) != ((fp+wordsize) & PAGE_MASK))
			hrp = guest_to_host(domid, vcpu, fp+wordsize);
		else
			hrp = hfp+wordsize;
		memcpy(&fp, hfp, wordsize);
		memcpy(&retaddr, hrp, wordsize);
		DBG("vcpu %d, fp = %#"PRIx64"->%p->%#"PRIx64", return addr = %#"PRIx64"->%p->%#"PRIx64"\n",
				vcpu, fp, hfp, *((uint64_t*)hfp), fp+wordsize, hrp, retaddr);
	}
	fprintf(file, "1\n\n");
}

/**
 * Walk the stack via the frame pointer. Returns 0 on success.
 */
int do_stack_trace_fp(int domid, unsigned int max_vcpu_id, int wordsize, FILE *file, void *symbol_table) {
	unsigned int vcpu;

	if (pause_domain(domid) < 0) {
		fprintf(stderr, "Could not pause domid %d\n", domid);
		return -7;
	}
	for (vcpu = 0; vcpu <= max_vcpu_id; vcpu++) {
		walk_stack_fp(domid, vcpu, wordsize, file, symbol_table);
	}
	if (unpause_domain(domid) < 0) {
		fprintf(stderr, "Could not unpause domid %d\n", domid);
		return -7;
	}
	return 0;
}


#ifdef WITH_UNWIND
void walk_stack_libunwind(struct UXEN_info *ui, unw_addr_space_t as, FILE *file, bool resolve_symbols) {
	unw_cursor_t cursor;
	unw_word_t addr;
	const unsigned int BUFLEN = 64;
	char buf[BUFLEN];

	// This needs to be reinitalized for every stack walk round,
	// so no reason to save it anywhere outside for re-use.
	unw_init_remote(&cursor, as, ui);

	// our first "return" address is the instruction pointer
	unw_get_reg(&cursor, UNW_REG_IP, &addr);

	if (resolve_symbols && !unw_get_proc_name(&cursor, buf, BUFLEN, &addr))
		fprintf(file, "%s+%#"PRIxPTR"\n", buf, addr);
	else
		fprintf(file, "%#"PRIxPTR"\n", addr);

	while (unw_step(&cursor) > 0) {
		unw_get_reg(&cursor, UNW_REG_IP, &addr);
		if (!addr)
			break;
		if (resolve_symbols && !unw_get_proc_name(&cursor, buf, BUFLEN, &addr))
			fprintf(file, "%s+%#"PRIxPTR"\n", buf, addr);
		else
			fprintf(file, "%#"PRIxPTR"\n", addr);
	}
	fprintf(file, "1\n\n");
}

/**
 * Walk the stack via eh_frame information parsed by libunwind. Returns 0 on success.
 */
int do_stack_trace_libunwind(int domid, unsigned int max_vcpu_id, FILE *file,
		struct UXEN_info *ui, unw_addr_space_t as, bool resolve_symbols) {
	unsigned int vcpu;

	if (pause_domain(domid) < 0) {
		fprintf(stderr, "Could not pause domid %d\n", domid);
		return -7;
	}
	for (vcpu = 0; vcpu <= max_vcpu_id; vcpu++) {
		_UXEN_change_vcpu(ui, vcpu);
		walk_stack_libunwind(ui, as, file, resolve_symbols);
	}
	if (unpause_domain(domid) < 0) {
		fprintf(stderr, "Could not unpause domid %d\n", domid);
		return -7;
	}
	return 0;
}
#endif

void *read_symbol_table(char *symbol_table_file_name)
{
	char line[256];
	char *p, *symbol;
	size_t len;
	int count = 0;
	FILE *f;
	int ch, i;
	void *head;
	element_t element;

	f = fopen(symbol_table_file_name, "r");
	if (f == NULL) {
		fprintf(stderr, "failed to open symbol table file %s, will not resolve symbols!\n",
				symbol_table_file_name);
		return NULL;
	}

	// count number of lines, i.e., elements in the file:
	while((ch = fgetc(f)) != EOF)
		if (ch == '\n')
			count++;

	if (count == 0) {
		fprintf(stderr, "Symbol table file %s contained no valid entries!\n", symbol_table_file_name);
		fprintf(stderr, "Disabling symbol resolution.\n");
		return NULL;
	}

	rewind(f);
	head = binsearch_alloc(count);
	if (!head)
		return NULL;
	for (i=0; i<count; i++) {
		if (fgets(line, 256, f) == NULL) {
			fprintf(stderr, "Error reading entry %d from symbol table file\n", i);
			goto out_err;
		}
		element.key = strtoull(line, &p, 16);
		// p should now point to the space between address and type
		// so jump ahead 3 characters to symbol
		p += 3;
		len = strlen(p);
		if (len == 0) {
			fprintf(stderr, "Error reading entry %d from symbol table file\n", i);
			goto out_err;
		}
		symbol = malloc(len+1);
		if (!symbol) {
			fprintf(stderr, "Error allocating %zu bytes of memory for symbol table entry %d!\n", len, i);
			goto out_err;
		}
		else {
			// don't copy newline
			strncpy(symbol, p, len-1);
			element.val.c = symbol;
		}
		binsearch_fill(head, &element);
	}
	if (i != count) {
		fprintf(stderr, "Error reading symbol table from file, expected %d entries, got %d\n", count, i);
		goto out_err;
	}
	return head;

out_err:
	fprintf(stderr, "Disabling symbol resolution.\n");
	free(head);
	return NULL;
}

void write_file_header(FILE *f, int domid)
{
	char timestring[64];
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME_COARSE, &ts);
	strftime(timestring, 63, "%Y-%m-%d %H:%M:%S %Z (%z)", localtime(&ts.tv_sec));
	fprintf(f, "#unikernel stack tracer using %s hypercall interface\n", HYPERCALL_NAME);
	fprintf(f, "#tracing domid %d on %s\n\n", domid, timestring);
}

static void print_usage(char *name) {
	printf("usage:\n");
	printf("  %s [options] <outfile> <domid>\n\n", name);
	printf("options:\n");
	printf("  -F n --frequency=n         Frequency of traces (in per second, default 1)\n");
	printf("  -T n --time=n              How long to run the tracer (in seconds, default 1)\n");
	printf("  -M --missed-deadlines      Print a warning to STDERR whenever a deadline is\n");
	printf("                             missed. Note that this may exacerbate the problem,\n");
	printf("                             or it may treacherously appear to improve it,\n");
	printf("                             while it actually doesn't (due to timing quirks)\n");
	printf("  -s TAB --symbol-table=TAB  Resolve stack addresses with symbols from TAB.\n");
	printf("                             The file is expected to contain information\n");
	printf("                             formatted like the output of 'nm -n'. Please\n");
	printf("                             note that this slows down tracing.\n");
#ifdef WITH_UNWIND
	printf("                             -s, -e, and -E are mutually exclusive.\n");
	printf("  -e ELF --elf-file=ELF      Use libunwind to unwind the stack, using the\n");
	printf("                             .eh_frame section of the provided ELF file instead\n");
	printf("                             of the frame pointer. This allows unwinding code\n");
	printf("                             compiled with -fomit-frame-pointer, but is slower.\n");
	printf("                             -s, -e, and -E are mutually exclusive.\n");
	printf("  -E ELF --elf-resolve=ELF   In addition to using the provided ELF file to\n");
	printf("                             unwind the stack (as the -e option does), use the\n");
	printf("                             information in the file's .debug sections to also\n");
	printf("                             resolve symbols. This requires an unstripped\n");
	printf("                             binary and is naturally slower than the -e option.\n");
	printf("                             -s, -e, and -E are mutually exclusive.\n");
#endif
	printf("  -v --verbose               Show some more informational output.\n");
	printf("  -V --version               Show version information.\n");
	printf("  -h --help                  Print this help message.\n");
}

int main(int argc, char **argv) {
	int domid, ret;
	FILE *outfile;
	int max_vcpu_id;
	int wordsize;
	const int measure_rounds = 100;
	struct timespec gettime_overhead, minsleep, sleep;
	struct timespec begin, end, ts;
#ifdef WITH_UNWIND
	static const char *sopts = "hF:T:Ms:e:E:vV";
#else
	static const char *sopts = "hF:T:Ms:vV";
#endif
	static const struct option lopts[] = {
		{"help",             no_argument,       NULL, 'h'},
		{"frequency",        required_argument, NULL, 'F'},
		{"time",             required_argument, NULL, 'T'},
		{"missed-deadlines", no_argument,       NULL, 'M'},
		{"symbol-table",     required_argument, NULL, 's'},
#ifdef WITH_UNWIND
		{"elf-file",         required_argument, NULL, 'e'},
		{"elf-resolve",      required_argument, NULL, 'E'},
#endif
		{"verbose",          no_argument,       NULL, 'v'},
		{"version",          no_argument,       NULL, 'V'},
		{0, 0, 0, 0}
	};
	char *resolver_file_name = NULL;
	void *symbol_table = NULL;
#ifdef WITH_UNWIND
	struct UXEN_info *ui = NULL;
	unw_addr_space_t as = NULL;
	bool have_seE = false;
	bool resolver_is_elf = false;
	bool resolve_symbols_from_elf = false;
#endif
	char *exename, *outname;
	int opt;
	unsigned int freq = 1;
	unsigned int time = 1;
	bool warn_missed_deadlines = false;
	unsigned int i,j;
	unsigned long long missed_deadlines = 0;

	while ((opt = getopt_long(argc, argv, sopts, lopts, NULL)) != -1) {
		switch(opt) {
			case 'h':
				print_usage(argv[0]);
				return 0;
			case 'F':
				freq = strtoul(optarg, NULL, 10);
				break;
			case 'T':
				time = strtoul(optarg, NULL, 10);
				break;
			case 'M':
				warn_missed_deadlines = true;
				break;
			case 's':
				resolver_file_name = optarg;
#ifdef WITH_UNWIND
				if (have_seE) {
					printf("-s, -e, and -E are mutually exclusive.\n");
					return -1;
				}
				have_seE = true;
				break;
			case 'E':
				resolve_symbols_from_elf = true;
				// fallthrough
			case 'e':
				if (have_seE) {
					printf("-s, -e, and -E are mutually exclusive.\n");
					return -1;
				}
				have_seE = true;
				resolver_file_name = optarg;
				resolver_is_elf = true;
#endif
				break;
			case 'v':
				verbose = true;
				break;
			case 'V':
				printf("uniprof version %s\n", PACKAGE_VERSION);
				printf("source code available at %s\n", PACKAGE_URL);
				return 0;
			case '?':
				fprintf(stderr, "%s --help for usage\n", argv[0]);
				return -1;
		}
	}
	sleep.tv_sec = 0; sleep.tv_nsec = (1000000000/freq);
	exename = argv[0];
	argv += optind; argc -= optind;
	outname = argv[0];

	if (argc < 2 || argc > 3) {
		print_usage(exename);
		return -1;
	}

	domid = strtol(argv[1], NULL, 10);
	if (domid == 0) {
		fprintf(stderr, "invalid domid (unparseable domid string %s, or cannot trace dom0)\n", argv[1]);
		return -2;
	}

	if ((strlen(outname) == 1) && (!(strncmp(outname, "-", 1)))) {
		outfile = stdout;
	}
	else {
		outfile = fopen(outname, "w");
		if (!outfile) {
			fprintf(stderr, "cannot open file %s: %s\n", outname, strerror(errno));
			return -3;
		}
	}

	if (xen_interface_open()) {
		fprintf(stderr, "Cannot connect to the hypervisor. (Is this Xen?)\n");
		return -4;
	}

	max_vcpu_id = get_max_vcpu_id(domid);
	if (max_vcpu_id < 0) {
		fprintf(stderr, "Could not access information for domid %d. (Does domid %d exist?)\n", domid, domid);
		return -5;
	}

	wordsize = get_word_size(domid);
	if (wordsize < 0) {
		fprintf(stderr, "Failed to retrieve word size for domid %d (returned %d)\n", domid, wordsize);
		return -6;
	}
	else if ((wordsize != 8) && (wordsize != 4)) {
		fprintf(stderr, "Unexpected wordsize (%d) for domid %d, cannot trace.\n", wordsize, domid);
		return -6;
	}
	DBG("wordsize is %d\n", wordsize);

#ifdef WITH_UNWIND
	if (resolver_is_elf) {
		// this implies the ELF file name is set
		ui = _UXEN_create(domid, 0, resolver_file_name);
		if (!ui) {
			fprintf(stderr, "Cannot read elf file %s. File unreadable or invalid!\n", resolver_file_name);
			return -7;
		}
		as = unw_create_addr_space(&_UXEN_accessors, 0);
	}
	else
#endif
		if (resolver_file_name) {
			symbol_table = read_symbol_table(resolver_file_name);
		}

	// Initialization stuff: write file header, measure overhead of clock_gettime/minimal sleeptime, etc.
	write_file_header(outfile, domid);
	measure_overheads(&gettime_overhead, &minsleep, measure_rounds);
	DBG("gettime overhead is %ld.%09ld, minimal nanosleep() sleep time is %ld.%09ld\n",
		gettime_overhead.tv_sec, gettime_overhead.tv_nsec, minsleep.tv_sec, minsleep.tv_nsec);

	// The actual stack tracing loop
	for (i = 0; i < time; i++) {
		// is the domain done and just hanging around for our sake?
		if (domain_shut_down(domid)) {
			return -8;
		}
		for (j = 0; j < freq; j++) {
			clock_gettime(CLOCK_MONOTONIC, &begin);
#ifdef WITH_UNWIND
			if (resolver_is_elf)
				ret = do_stack_trace_libunwind(domid, max_vcpu_id, outfile, ui, as, resolve_symbols_from_elf);
			else
#endif
				ret = do_stack_trace_fp(domid, max_vcpu_id, wordsize, outfile, symbol_table);
			if (ret) {
				return ret;
			}
			clock_gettime(CLOCK_MONOTONIC, &end);
			timespecadd(&begin, &sleep, &ts);
			if (timespeccmp(&ts, &end, <)) {
				missed_deadlines++;
				// don't sleep, but warn if --missed-deadlines is set
				if (warn_missed_deadlines) {
					timespecsub(&ts, &end, &ts);
					timespecnegtopos(&ts, &ts);
					fprintf(stderr, "we're falling behind by %ld.%09ld!\n", ts.tv_sec, ts.tv_nsec);
				}
			}
			else if ( (i < (time-1)) || (j < (freq-1)) )  {
				// only sleep if it's not the very last round
				timespecsub(&ts, &end, &ts);
				if (timespeccmp(&ts, &minsleep, <)) {
					// we finished so close to the next deadline that nanosleep() cannot
					// reliably wake us up in time, so do busywaiting
					busywait(ts.tv_nsec);
				}
				else {
					nanosleep(&ts, NULL);
				}
			}
		}
	}

	if (xen_interface_close())
		printf("error closing interface to hypervisor. (?!)\n");

	if (missed_deadlines)
		printf("Missed %lld deadlines\n", missed_deadlines);

	return 0;
}
