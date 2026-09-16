# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# chaingraph build: libbpf (vendored, static) + CO-RE BPF object + skeleton.
#
#   make            build build/chaingraph
#   make test       run the end-to-end chain test (needs root)
#   make clean

OUTPUT      := build
LIBBPF_SRC  := $(abspath libbpf/src)
LIBBPF_OBJ  := $(abspath $(OUTPUT)/libbpf.a)
BPFTOOL     ?= bpftool
CLANG       ?= clang
CC          ?= gcc

ARCH        := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
VMLINUX_BTF ?= /sys/kernel/btf/vmlinux

INCLUDES    := -I$(OUTPUT) -Isrc
CFLAGS      := -g -O2 -Wall -Wextra -Wno-unused-parameter
BPF_CFLAGS  := -g -O2 -Wall -Wno-unused-function -Wno-missing-declarations -mcpu=v3 -D__TARGET_ARCH_$(ARCH)
LDLIBS      := -lelf -lz

APP         := chaingraph
USER_SRCS   := src/chaingraph.c src/syms.c
USER_OBJS   := $(patsubst src/%.c,$(OUTPUT)/%.o,$(USER_SRCS))

ifeq ($(V),1)
Q =
msg =
else
Q = @
msg = @printf '  %-8s %s\n' "$(1)" "$(2)";
endif

.PHONY: all clean test
all: $(OUTPUT)/$(APP) $(OUTPUT)/chainload

$(OUTPUT) $(OUTPUT)/libbpf:
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

# libbpf, built static and installed privately into build/
$(LIBBPF_OBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(OUTPUT)/libbpf
	$(call msg,LIB,$@)
	$(Q)$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1		      \
		    OBJDIR=$(abspath $(OUTPUT)/libbpf) DESTDIR=$(abspath $(OUTPUT)) \
		    INCLUDEDIR= LIBDIR= UAPIDIR= prefix= libdir=	      \
		    install >/dev/null

# BTF of the build kernel; CO-RE relocates field offsets at load time, so
# the resulting binary also runs on other BTF-enabled kernels.
$(OUTPUT)/vmlinux.h: | $(OUTPUT)
	$(call msg,BTF,$@)
	$(Q)$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@

$(OUTPUT)/%.bpf.o: src/%.bpf.c src/chaingraph.h $(OUTPUT)/vmlinux.h $(LIBBPF_OBJ) | $(OUTPUT)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) $(BPF_CFLAGS) -target bpf $(INCLUDES) -c $< -o $@.tmp
	$(Q)$(BPFTOOL) gen object $@ $@.tmp
	$(Q)rm -f $@.tmp

$(OUTPUT)/%.skel.h: $(OUTPUT)/%.bpf.o | $(OUTPUT)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/chaingraph.o: src/chaingraph.c src/chaingraph.h src/syms.h $(OUTPUT)/chaingraph.skel.h $(LIBBPF_OBJ)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OUTPUT)/syms.o: src/syms.c src/syms.h | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OUTPUT)/$(APP): $(USER_OBJS) $(LIBBPF_OBJ)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

# synthetic wakeup-chain workload used by tests/run_tests.sh
$(OUTPUT)/chainload: tests/chainload.c | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) -g -O1 -fno-omit-frame-pointer -Wall -Wextra $< -o $@

test: all
	tests/run_tests.sh

clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT)

# delete failed targets
.DELETE_ON_ERROR:
# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:
