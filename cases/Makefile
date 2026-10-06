# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Workloads for the hitchtrace case studies.
#
#   make -C cases        build ../build/cases/{minigame,bgload,bgio}
#
# -fno-omit-frame-pointer so `hitchtrace -U` can walk the game's user stacks.

OUTPUT  := ../build/cases
CC      ?= gcc
CFLAGS  := -g -O2 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer

.PHONY: all clean
all: $(OUTPUT)/minigame $(OUTPUT)/bgload $(OUTPUT)/bgio

$(OUTPUT):
	mkdir -p $@

$(OUTPUT)/minigame: minigame/minigame.c | $(OUTPUT)
	$(CC) $(CFLAGS) $< -o $@ -lvulkan -lxcb -lpthread -lm

$(OUTPUT)/bgload: bgload.c | $(OUTPUT)
	$(CC) $(CFLAGS) $< -o $@ -lpthread

$(OUTPUT)/bgio: bgio.c | $(OUTPUT)
	$(CC) $(CFLAGS) $< -o $@ -lpthread

clean:
	rm -rf $(OUTPUT)
