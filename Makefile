# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# vkfwtest: GNU C23, built with GCC. Any CPU architecture: override CC (and
# PKG_CONFIG / CFLAGS / LDFLAGS) to cross-compile against a sysroot with
# libvulkan, e.g.  make CC=riscv64-linux-gnu-gcc PKG_CONFIG=riscv64-linux-gnu-pkg-config

ifeq ($(origin CC),default)
CC         := gcc
endif
PKG_CONFIG ?= pkg-config
GLSLANG    ?= glslangValidator
BUILD      ?= build

VK_CFLAGS := $(shell $(PKG_CONFIG) --cflags vulkan 2>/dev/null)
VK_LIBS   := $(shell $(PKG_CONFIG) --libs vulkan 2>/dev/null || echo -lvulkan)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu23 -Wall -Wextra -Werror -Wshadow -Wformat=2 -I$(BUILD) -Isrc $(VK_CFLAGS)
LDFLAGS ?=
LDLIBS  += $(VK_LIBS) -lpthread -lm

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)
SHADERS := $(wildcard shaders/*.vert shaders/*.frag shaders/*.comp)
SPV_H   := $(SHADERS:shaders/%=$(BUILD)/%.h)

all: $(BUILD)/vkfwtest

$(BUILD)/vkfwtest: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# shaders are embedded as uint32_t arrays: no shader compiler on the target
$(BUILD)/%.h: shaders/% | $(BUILD)
	$(GLSLANG) -V --quiet --vn spv_$(subst .,_,$*) -o $@.tmp $<
	sed 's/^const uint32_t/static const uint32_t/' $@.tmp > $@ && rm -f $@.tmp

$(BUILD)/%.o: src/%.c src/vkfwtest.h $(SPV_H) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
