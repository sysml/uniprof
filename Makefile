XEN_ROOT ?= $(realpath ../xen)
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS += $(CFLAGS_libxenctrl) -Wall -Wextra
LDLIBS += $(LDLIBS_libxenctrl)

BIN      = uniprof symbolize

.PHONY: all
all: $(BIN)

.PHONY: clean
clean:
	$(RM) *.a *.so *.o *.rpm $(BIN) .$(BIN).o.d

uniprof: uniprof.o
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS) $(APPEND_LDFLAGS)

symbolize.o: symbolize.cc
	g++ -c -O3 -Wall -Wextra -o $@ $<
symbolize: symbolize.o
	g++ $(LDFLAGS) -o $@ $< $(LDLIBS) $(APPEND_LDFLAGS)
