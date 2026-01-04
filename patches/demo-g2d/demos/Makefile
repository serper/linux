# Makefile for G2D + DRM Visual Demos
# Copyright (C) 2025 Sergio Perez
# SPDX-License-Identifier: GPL-2.0-or-later

# Toolchain for Alpine Linux with musl
CC = arm-linux-musleabihf-gcc
KERNEL_DIR = /home/sergio/source/t113/kernel-t113
KERNEL_HEADERS = $(KERNEL_DIR)/include/uapi
# Use Alpine rootfs sysroot
ALPINE_ROOTFS = /home/sergio/source/t113/rootfs-alpine
# Use --sysroot to avoid mixing headers from different sources
CFLAGS = -Wall -O0 -g --sysroot=$(ALPINE_ROOTFS) -I. -I$(KERNEL_HEADERS) -I$(ALPINE_ROOTFS)/usr/include/libdrm
LDFLAGS = --sysroot=$(ALPINE_ROOTFS) -ldrm -lm

# Source files
DRM_BASE = demo-drm-base.c

# Demo programs
DEMOS = demo-bouncing-ball demo-fillrect-rcq-simple test_timing test-mask-rop4 test-rop4-pattern test-rop4-blackwhite

# Build directory (optional, for cross-compilation output)
BUILD_DIR = .

.PHONY: all clean

all: $(DEMOS)

# Bouncing ball demo
demo-bouncing-ball: demo-bouncing-ball.c $(DRM_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Bouncing ball demo unified
demo-bouncing-ball-unified: demo-bouncing-ball-unified.c $(DRM_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Rotation test demo
demo-fillrect-rcq-simple: demo-fillrect-rcq-simple.c $(DRM_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Scale + Rotate test demo
demo-scale-rotate: demo-scale-rotate.c $(DRM_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Test timing, display adjustment
test_timing: test_timing.c $(DRM_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Test mask operation with ROP4
test-mask-rop4: test-mask-rop4.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

# Test ROP4 with pattern
test-rop4-pattern: test-rop4-pattern.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

# Test ROP4 with black & white
test-rop4-blackwhite: test-rop4-blackwhite.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

clean:
	rm -f $(DEMOS)
	@echo "Cleaned demo binaries"

# Help target
help:
	@echo "G2D + DRM Demo Makefile"
	@echo "======================="
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all demos (default)"
	@echo "  clean            - Remove built binaries"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Demos:"
	@echo "  demo-bouncing-ball    - Bouncing ball with alpha blending"
	@echo "  demo-fillrect-rcq-simple - RCQ Fillrect simple test"
	@echo "  demo-scale-rotate     - Scale + Rotate 2-pass test"
	@echo ""
	@echo "Note: Compiled for Alpine Linux (musl) on T113-S3"
