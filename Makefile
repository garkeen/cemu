CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -Isrc
# -MMD -MP: emit header dependencies so editing a header rebuilds its users
CFLAGS += -MMD -MP
BUILD := build
TARGET := $(BUILD)/cemu.exe

# Explicit per-depth wildcards. Portable GNU make (no shell, no find), and
# unlike the old `src/*/*/` glob it does not rely on Windows' loose
# trailing-slash matching — on POSIX make the old pattern matched directories
# only and silently dropped every top-level module. Deepest modules are
# src/cpu/isa/<name>/ and src/device/<class>/.
SRCS := $(wildcard src/*.c) \
        $(wildcard src/*/*.c) \
        $(wildcard src/*/*/*.c) \
        $(wildcard src/*/*/*/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.obj,$(SRCS))
DEPS := $(OBJS:.obj=.d)

.PHONY: all clean check dirs

all: dirs $(TARGET)

dirs:
	mkdir -p $(BUILD) $(sort $(dir $(OBJS)))

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.obj: src/%.c | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependency-edge check: see tools/depcheck.sh (AGENTS.md 第七节).
check:
	@bash tools/depcheck.sh

clean:
	rm -rf $(BUILD)

-include $(DEPS)
