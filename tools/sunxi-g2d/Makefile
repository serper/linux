# Simple userspace build for sunxi-g2d sample
# Build with: make -C tools/sunxi-g2d

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11
# Include kernel UAPI headers from this repo if available
CFLAGS += -I../../include/uapi

LDFLAGS ?=

BIN := sunxi-g2d-yuv2rgb
SRCS := sunxi-g2d-yuv2rgb.c
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJS)

.PHONY: all clean
