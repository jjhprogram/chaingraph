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

APPS        := chaingraph hitchtrace
BENCHES     := chainload hitchbench

ifeq ($(V),1)
Q =
msg =
else
Q = @
msg = @printf '  %-8s %s\n' "$(1)" "$(2)";
endif

.PHONY: all clean test test-hitch layer layer-install
all: $(addprefix $(OUTPUT)/,$(APPS) $(BENCHES))

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

$(OUTPUT)/%.bpf.o: src/%.bpf.c src/%.h $(OUTPUT)/vmlinux.h $(LIBBPF_OBJ) | $(OUTPUT)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) $(BPF_CFLAGS) -target bpf $(INCLUDES) -c $< -o $@.tmp
	$(Q)$(BPFTOOL) gen object $@ $@.tmp
	$(Q)rm -f $@.tmp

$(OUTPUT)/%.skel.h: $(OUTPUT)/%.bpf.o | $(OUTPUT)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

# each tool: src/<app>.c + its skeleton, linked with the symbolizer
$(OUTPUT)/%.o: src/%.c src/%.h src/syms.h $(OUTPUT)/%.skel.h $(LIBBPF_OBJ)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OUTPUT)/syms.o: src/syms.c src/syms.h | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(addprefix $(OUTPUT)/,$(APPS)): $(OUTPUT)/%: $(OUTPUT)/%.o $(OUTPUT)/syms.o $(LIBBPF_OBJ)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

# the implicit Vulkan layer: gives any Vulkan app the frame markers.
# The two markers carry visibility("default") so they stay in .dynsym and
# .symtab for the uprobe; never strip this .so.
LAYER_SO  := $(OUTPUT)/libVkLayer_hitchtrace.so
LAYER_DIR ?= $(HOME)/.local/share/vulkan/implicit_layer.d

$(LAYER_SO): layer/hitchtrace_layer.c | $(OUTPUT)
	$(call msg,LAYER,$@)
	$(Q)$(CC) -g -O2 -Wall -Wextra -fPIC -fvisibility=hidden -shared \
		-pthread $< -o $@ -Wl,-z,defs

layer: $(LAYER_SO)

# install the manifest for this user, pointing at the built .so
layer-install: $(LAYER_SO) layer/VkLayer_hitchtrace.json
	$(call msg,INSTALL,$(LAYER_DIR)/VkLayer_hitchtrace.json)
	$(Q)mkdir -p $(LAYER_DIR)
	$(Q)sed 's#"library_path": "[^"]*"#"library_path": "$(abspath $(LAYER_SO))"#' \
		layer/VkLayer_hitchtrace.json > $(LAYER_DIR)/VkLayer_hitchtrace.json

# synthetic workloads used by the test scripts
$(OUTPUT)/chainload: tests/chainload.c | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) -g -O1 -fno-omit-frame-pointer -Wall -Wextra $< -o $@

# the frame marker must stay in the symbol table for the uprobe
$(OUTPUT)/hitchbench: tests/hitchbench.c | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) -g -O1 -fno-omit-frame-pointer -Wall -Wextra -pthread $< -o $@

test: all
	tests/run_tests.sh

test-hitch: all
	tests/run_hitch_tests.sh

clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT)

# delete failed targets
.DELETE_ON_ERROR:
# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:
