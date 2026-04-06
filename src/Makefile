CLANG ?= clang
CC ?= gcc

ARCH := x86

# paths
LIBBPF_DIR := /usr
INCLUDES := -I$(LIBBPF_DIR)/include -I$(LIBBPF_DIR)/include/bpf

# flags
CFLAGS := -O2 -g -Wall $(INCLUDES)
BPF_CFLAGS := -O2 -g -target bpf

LIBS := -L$(LIBBPF_DIR)/lib/x86_64-linux-gnu -lbpf -lelf -lz

TARGET = bootstrap

# ===================== BUILD =====================

all: $(TARGET)

$(TARGET): bootstrap.o stats.o dashboard.o bootstrap.skel.h
	$(CC) $(CFLAGS) -o $@ bootstrap.o stats.o dashboard.o $(LIBS)

bootstrap.o: bootstrap.c bootstrap.h stats.h dashboard.h bootstrap.skel.h
	$(CC) $(CFLAGS) -c bootstrap.c

stats.o: stats.c stats.h bootstrap.h
	$(CC) $(CFLAGS) -c stats.c

dashboard.o: dashboard.c dashboard.h stats.h
	$(CC) $(CFLAGS) -c dashboard.c

# ===================== BPF =====================

bootstrap.bpf.o: bootstrap.bpf.c bootstrap.h
	$(CLANG) $(BPF_CFLAGS) -D__TARGET_ARCH_$(ARCH) \
		-c bootstrap.bpf.c -o bootstrap.bpf.o

bootstrap.skel.h: bootstrap.bpf.o
	bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h

# ===================== CLEAN =====================

clean:
	rm -f *.o *.skel.h $(TARGET)
