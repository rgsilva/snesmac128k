# snesmac SNES-side build. Calypsi C -> 65816, same toolchain as snespico8.
# M0: hello (prove the build plumbing). Later: the 68000 core (Musashi via
# Calypsi) + the Mac machine glue.
#
# Reuses snespico8's vendored Calypsi install for now (override CALDIR to point
# elsewhere once snesmac vendors its own).
CALDIR ?= ../../snespico8/tools/calypsi-65816-5.17
CC := $(CALDIR)/bin/cc65816
AS := $(CALDIR)/bin/as65816
LN := $(CALDIR)/bin/ln65816
MESEN := /Applications/Mesen.app/Contents/MacOS/Mesen
BUILD := build

MODEL := --code-model=large --data-model=large
OPT ?= -O2
CFLAGS = --core=65816 --target=snes $(MODEL) --no-ppu-mul $(OPT) -I. --list-file=$(@:.o=.lst)
AFLAGS := --core=65816 --target=SNES $(MODEL)
LNFLAGS := calypsi/linker.scm --output-format=raw --rtattr exit=simplified \
           $(CALDIR)/lib/clib-lc-ld-snes-noppu.a

.PHONY: hello test-hello clean
hello: $(BUILD)/hello.sfc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/header.o: calypsi/header.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/mailbox.o: calypsi/mailbox.c calypsi/mailbox.h | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/hello.o: hello.c calypsi/mailbox.h | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/hello.raw: $(BUILD)/header.o $(BUILD)/mailbox.o $(BUILD)/hello.o
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/hello.map $^

$(BUILD)/hello.sfc: $(BUILD)/hello.raw tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@

test-hello: $(BUILD)/hello.sfc
	$(MESEN) --testrunner $(BUILD)/hello.sfc test/hello.lua

clean:
	rm -rf $(BUILD)
