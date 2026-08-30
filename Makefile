CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -Isrc
BUILD := build
TARGET := $(BUILD)/cemu.exe

SRCS := $(foreach d,$(sort $(dir $(wildcard src/*/*/))),$(wildcard $(d)*.c))
# Code not yet wired into any machine stays out of the build (开发准则 四):
# sifive_test joins when machine/virt.c lands, time_win when a time consumer
# (CLINT mtime) exists.
SRCS := $(filter-out src/device/sifive_test.c src/host/time_win.c,$(SRCS))
OBJS := $(patsubst src/%.c,$(BUILD)/%.obj,$(SRCS))

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
