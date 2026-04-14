CLANG ?= clang
CC    ?= gcc

# Auto-detect architecture; override with: make ARCH=arm64
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
    ARCH := x86
else ifeq ($(UNAME_M),aarch64)
    ARCH := arm64
else ifeq ($(UNAME_M),arm)
    ARCH := arm
else
    ARCH := $(UNAME_M)
endif

LIBBPF_DIR := /usr
INCLUDES   := -I$(LIBBPF_DIR)/include -I$(LIBBPF_DIR)/include/bpf

CFLAGS     := -O2 -g -Wall -Wextra -std=c11 $(INCLUDES)
BPF_CFLAGS := -O2 -g -target bpf -D__TARGET_ARCH_$(ARCH)

LIBS := -L$(LIBBPF_DIR)/lib/x86_64-linux-gnu -lbpf -lelf -lz -lm

TARGET = bootstrap

# ===================== SOURCES =====================

SRCS = bootstrap.c stats.c dashboard.c export.c vscreen.c layout.c theme.c
OBJS = $(SRCS:.c=.o)

# ===================== BUILD =====================

all: $(TARGET)

$(TARGET): $(OBJS) bootstrap.skel.h
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

bootstrap.o: bootstrap.c bootstrap.h stats.h dashboard.h export.h \
             bootstrap.skel.h term.h vscreen.h theme.h
	$(CC) $(CFLAGS) -c bootstrap.c

stats.o: stats.c stats.h bootstrap.h
	$(CC) $(CFLAGS) -c stats.c

dashboard.o: dashboard.c dashboard.h stats.h vscreen.h layout.h theme.h term.h
	$(CC) $(CFLAGS) -c dashboard.c

export.o: export.c export.h stats.h
	$(CC) $(CFLAGS) -c export.c

vscreen.o: vscreen.c vscreen.h term.h
	$(CC) $(CFLAGS) -c vscreen.c

layout.o: layout.c layout.h vscreen.h
	$(CC) $(CFLAGS) -c layout.c

theme.o: theme.c theme.h
	$(CC) $(CFLAGS) -c theme.c

# ===================== BPF =====================

bootstrap.bpf.o: bootstrap.bpf.c bootstrap.h vmlinux.h
	$(CLANG) $(BPF_CFLAGS) \
		-c bootstrap.bpf.c -o bootstrap.bpf.o

bootstrap.skel.h: bootstrap.bpf.o
	bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h

# ===================== CLEAN =====================

clean:
	rm -f *.o *.skel.h $(TARGET)

# ===================== HELP =====================

help:
	@echo "Targets:"
	@echo "  make              - build everything (auto-detects arch: $(ARCH))"
	@echo "  make ARCH=arm64   - build for a specific arch"
	@echo "  make clean        - remove build artifacts"
	@echo ""
	@echo "Runtime options:"
	@echo "  --theme 0         - dark theme (default)"
	@echo "  --theme 1         - Catppuccin Mocha theme"
	@echo ""
	@echo "Manual BPF steps (if needed):"
	@echo "  clang -O2 -g -target bpf -D__TARGET_ARCH_$(ARCH) -c bootstrap.bpf.c -o bootstrap.bpf.o"
	@echo "  bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h"

.PHONY: all clean help
