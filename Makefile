XEN_ROOT ?= $(realpath ../xen)
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS += $(CFLAGS_libxenctrl) -Wall -Wextra
LDLIBS += $(LDLIBS_libxenctrl)

BIN      = uniprof

.PHONY: all
all: $(BIN)

.PHONY: clean
clean:
	$(RM) *.a *.so *.o *.rpm $(BIN) .$(BIN).o.d

uniprof: uniprof.o
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS) $(APPEND_LDFLAGS)
