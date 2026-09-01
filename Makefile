CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -Isrc
# -MMD -MP: emit header dependencies so editing a header rebuilds its users
CFLAGS += -MMD -MP
BUILD := build
TARGET := $(BUILD)/cemu.exe

SRCS := $(foreach d,$(sort $(dir $(wildcard src/*/*/))),$(wildcard $(d)*.c))
# Every module in the tree is wired: sifive_test/time_win landed with the
# virt machine (CLINT mtime consumes the host clock).
OBJS := $(patsubst src/%.c,$(BUILD)/%.obj,$(SRCS))
DEPS := $(OBJS:.obj=.d)

.PHONY: all clean

all: dirs $(TARGET)

dirs:
	mkdir -p $(BUILD) $(sort $(dir $(OBJS)))

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.obj: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.obj: | $(BUILD)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
