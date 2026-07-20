# Host validation harness: the Musashi 68000 core vs the SingleStepTests/m68000
# vectors. Same idea as snes86's core-c/tests + libem86.dylib.
#
#   make          # build the ctypes dylib (regenerates Musashi's opcode tables)
#   make vectors  # fetch the ~113 MB vector set once (needs vectors/_urls.json)
#   make test     # run the full validation
MUS := ../musashi
CFLAGS := -O2 -std=c11 -fPIC -I$(MUS) -I$(MUS)/softfloat -I.
DYLIB := build/libem68k.dylib
# m68kcpu.c #includes m68kfpu.c, so it is NOT listed separately here.
SRC := m68ktest.c m68kglue.c $(MUS)/m68kcpu.c $(MUS)/m68kops.c \
       $(MUS)/m68kdasm.c $(MUS)/softfloat/softfloat.c

.PHONY: all test vectors clean
all: $(DYLIB)

$(MUS)/m68kmake: $(MUS)/m68kmake.c
	cc -O2 -o $@ $<

# Musashi generates its 1900+ opcode handlers from m68k_in.c at build time.
$(MUS)/m68kops.c: $(MUS)/m68kmake $(MUS)/m68k_in.c
	cd $(MUS) && ./m68kmake . m68k_in.c

$(DYLIB): $(SRC) | build
	cc $(CFLAGS) -shared -o $@ $(SRC)

build:
	mkdir -p build

vectors:
	bash fetch.sh

test: $(DYLIB)
	python3 run_vectors.py --fails 8

clean:
	rm -rf build $(MUS)/m68kops.c $(MUS)/m68kops.h $(MUS)/m68kmake
