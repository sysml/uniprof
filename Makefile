XEN_ROOT ?= $(realpath ../xen)
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS   += $(CFLAGS_libxenctrl) -Wall -Wextra
CXXFLAGS ?= -O3 -Wall -Wextra -MMD -MF .$(if $(filter-out .,$(@D)),$(subst /,@,$(@D))@)$(@F).d
LDLIBS   += $(LDLIBS_libxenctrl)

BIN      = uniprof symbolize
OBJ      = $(addsuffix .o,$(BIN))
SRC      = uniprof.c symbolize.cc
DEP      = $(addprefix .,$(addsuffix .d,$(OBJ)))

.PHONY: all
all: $(BIN)

.PHONY: clean
clean:
	$(RM) *.a *.so *.o *.rpm $(BIN) $(DEP)

uniprof: uniprof.o
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS) $(APPEND_LDFLAGS)

symbolize: symbolize.o
	$(CXX) $(LDFLAGS) -o $@ $< $(LDLIBS) $(APPEND_LDFLAGS)

-include $(DEP)
