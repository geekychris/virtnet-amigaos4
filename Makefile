# virtnet.device — Makefile.
#
# Invoked inside the walkero AmigaOS4 GCC 11 Docker container by
# scripts/build.sh. Do not run this Makefile directly on the host — the
# CC name (ppc-amigaos-gcc) only exists inside the container.
#
#   ./scripts/build.sh          — build device + test
#   ./scripts/build.sh clean    — nuke build/
#   ./scripts/build.sh test     — build only the smoke test
#   ./scripts/build.sh shell    — interactive shell in the container

CC     = ppc-amigaos-gcc
STRIP  = ppc-amigaos-strip

# Cross-compile flags. Adapted from ../python-amigaos4/build.sh but
# WITHOUT -D__USE_INLINE__: that flag turns bare `DebugPrintF(...)` calls
# into `IExec->DebugPrintF(...)` via inline4 macros, which breaks the
# explicit `iexec->` arrow style a device driver has to use (no global
# IExec exists inside a resident-tag .device — the interface pointer is
# a local passed in via Init). Keeping __USE_OLD_TIMEVAL__ for the
# newlib/OS4 timeval-layout compat used everywhere else in the stack.
CFLAGS = -mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall -Wextra \
         -D__PPC__ -D__USE_OLD_TIMEVAL__ \
         -I./include

# Device link line — modeled on VirtualSCSIDevice's Makefile. -nostartfiles
# is REQUIRED for a resident-tag .device (no C runtime prologue). The
# common-page-size / max-page-size flags shave ~28KB of zero-padding from
# the final binary.
DEVLDFLAGS = -nostartfiles \
             -Wl,-z,common-page-size=4096 \
             -Wl,-z,max-page-size=4096

# Test-program link line — normal executable, uses -lauto so lib bases
# open on first call automatically.
TESTLDFLAGS = -lauto

BUILD    = build
DEV      = $(BUILD)/virtnet.device
DEV_DBG  = $(BUILD)/virtnet.device.debug

# Add every test binary here as it lands. Each corresponds to
# tests/<name>.c, compiled the same way.
TEST_NAMES = testopen testdeviceq testmac testirq testonline testtx testrx testtx_cooked testrx_cooked testrxtask testroadshow testdiag testsizeof testdqbuf testcfgbuf testbsdadd testquery teststat
TESTS      = $(patsubst %,$(BUILD)/%,$(TEST_NAMES))

DEV_SRC  = src/device.c
DEV_OBJ  = $(BUILD)/device.o
DEV_OBJ_DBG = $(BUILD)/device.dbg.o

.PHONY: all clean test help

all: $(DEV) $(DEV_DBG) $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

$(DEV_OBJ): $(DEV_SRC) include/version.h include/virtnet.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DEV_OBJ_DBG): $(DEV_SRC) include/version.h include/virtnet.h | $(BUILD)
	$(CC) $(CFLAGS) -DDEBUG -g -c $< -o $@

# Release device: stripped, ready for SYS:Kickstart/ (once stable).
$(DEV): $(DEV_OBJ)
	$(CC) $(DEV_OBJ) -o $@ $(DEVLDFLAGS)
	$(STRIP) --strip-all $@

# Debug device: symbols kept so GrimReaper decodes stack frames.
$(DEV_DBG): $(DEV_OBJ_DBG)
	$(CC) $(DEV_OBJ_DBG) -o $@ $(DEVLDFLAGS)

# Generic test-binary rule — any tests/<name>.c compiles to build/<name>.
$(BUILD)/%: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(TESTLDFLAGS)

test: $(TESTS)

clean:
	rm -rf $(BUILD)

help:
	@echo "virtnet.device build system"
	@echo ""
	@echo "  ./scripts/build.sh          — build everything (device + test)"
	@echo "  ./scripts/build.sh test     — build testopen only"
	@echo "  ./scripts/build.sh clean    — remove build/"
	@echo "  ./scripts/build.sh shell    — drop into the toolchain container"
