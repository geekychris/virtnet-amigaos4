# sanashunt.device — Roadshow trace proxy over rtl8139.device.
# Built inside the walkero AmigaOS4 GCC 11 Docker container.
#
#   ./scripts/build.sh  — from parent virte1000 dir wraps docker (reused)
#   docker exec ... make -C sanashunt/

CC     = ppc-amigaos-gcc
STRIP  = ppc-amigaos-strip

CFLAGS = -mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall -Wextra \
         -D__PPC__ -D__USE_OLD_TIMEVAL__ \
         -I./include

# Same rationale as virte1000's DEVLDFLAGS.
DEVLDFLAGS = -nostartfiles \
             -Wl,-z,common-page-size=4096 \
             -Wl,-z,max-page-size=4096

BUILD    = build
DEV      = $(BUILD)/sanashunt.device
DEV_DBG  = $(BUILD)/sanashunt.device.debug

DEV_SRC      = src/shunt.c
DEV_OBJ      = $(BUILD)/shunt.o
DEV_OBJ_DBG  = $(BUILD)/shunt.dbg.o

# Test binaries in tests/*.c compile to build/<name>.
TEST_NAMES = testshuntdiag
TESTS      = $(patsubst %,$(BUILD)/%,$(TEST_NAMES))

TESTLDFLAGS = -lauto

.PHONY: all clean help

all: $(DEV) $(DEV_DBG) $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

$(DEV_OBJ): $(DEV_SRC) include/shunt.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DEV_OBJ_DBG): $(DEV_SRC) include/shunt.h | $(BUILD)
	$(CC) $(CFLAGS) -DDEBUG -g -c $< -o $@

$(DEV): $(DEV_OBJ)
	$(CC) $(DEV_OBJ) -o $@ $(DEVLDFLAGS)
	$(STRIP) --strip-all $@

$(DEV_DBG): $(DEV_OBJ_DBG)
	$(CC) $(DEV_OBJ_DBG) -o $@ $(DEVLDFLAGS)

$(BUILD)/%: tests/%.c include/shunt.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(TESTLDFLAGS)

clean:
	rm -rf $(BUILD)

help:
	@echo "sanashunt.device build"
	@echo ""
	@echo "  make all              — device + debug + tests"
	@echo "  make clean            — nuke build/"
