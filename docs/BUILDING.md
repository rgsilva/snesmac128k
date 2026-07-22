# Building snesmac

There are two halves: the **host** side (the C core, the `runmac` emulator, and the
vector/parity validation) and the **SNES** side (the Calypsi build that produces the
`.sfc` ROMs). You can do everything on the host; you need a Mac ROM and a System disk to
build the boot ROMs.

## Prerequisites

- A host C compiler (`cc`/`clang`) and `python3` — for the core, `runmac`, and the
  validation harness.
- The [Calypsi](https://www.calypsi.cc/) 65816 C compiler — for the SNES ROMs. The build
  points at the copy vendored with the sibling snespico8 project
  (`../../snespico8/tools/calypsi-65816-5.17`); override `CALDIR` in `snes/Makefile` if
  yours lives elsewhere.
- [Mesen](https://www.mesen.ca/) — to run the ROMs (`--testrunner` for the headless
  harnesses, or the GUI to watch a boot). The Makefiles expect it at
  `/Applications/Mesen.app`.
- For the CPU vectors: `~113 MB` of test data, fetched on demand (below).

## Bring your own ROM and disk

The Macintosh ROM and Apple's System software are copyright Apple and are **not** in
this repository. `roms/` is gitignored. You provide two files:

| file | what | size |
|---|---|---|
| `roms/mac128k.rom` | a Macintosh 128K ROM dump | 64 KB (checksum `$28BA61CE`) |
| `roms/sys11_data.bin` | a bootable System disk as raw 512-byte logical sectors | 400 KB (800 sectors) |

To make `sys11_data.bin` from a disk image: get a **DiskCopy 4.2** `.image` of a bootable
400 KB System disk (System 1.1 is the tested one), then split it into logical sectors:

```sh
python3 core-c/tests/dc42.py System-1.1.image roms/sys11_data.bin
```

`dc42.py` strips the DiskCopy header and tags and writes the 800 × 512-byte data fork.
Sector 0's `4C4B` (`'LK'`) signature confirms it's bootable.

## A second disk (optional)

Any additional 400 KB raw image dropped at `roms/disk2_data.bin` — same
DiskCopy-4.2 → raw pipeline as the System disk — is embedded in the cart
(which grows to 4 MB) and served as the **external drive**: the Finder mounts
it as a second volume on the desktop. An apps disk (MacPaint, MacWrite) from
the same archives is the natural choice. Remove the file and rebuild to go
back to one drive.

## Host: the core, the emulator, and validation

```sh
cd core-c/tests
make            # build the core as a shared library (regenerates Musashi's tables)
make vectors    # one-time: fetch the ~113 MB SingleStepTests/m68000 vector set
make test       # run the full per-instruction validation
```

Build and run the host emulator (boots your ROM + disk, dumps the framebuffer to
`/tmp/macscreen.pbm`):

```sh
cc -O2 -std=c11 -I core-c/src -o /tmp/runmac \
   core-c/src/runmac.c core-c/src/mac.c core-c/src/m68k.c
/tmp/runmac roms/mac128k.rom 80000000
```

`runmac` is the development mirror — it's where the machine model and every HLE fix are
debugged before anything touches Calypsi.

## SNES: the ROMs

From `snes/`:

```sh
make macboot          # -> build/macboot.sfc   the live boot (ROM + disk embedded)
make macboot-desktop  # teleport straight to the Finder desktop, not the boot
make macboot-purist   # the faithful power-on (no teleport; very slow)
make macparity        # -> build/macparity.sfc the host<->SNES parity ROM
make test-macparity   # build + run the parity check in Mesen (expects "PARITY OK")
make test-macparity-hot  # same boot, but through the hand-asm inner loop
make test-macboot     # build + run the live boot in Mesen
```

`test-macparity` steps the real ROM boot with the portable **C** core (validates
Calypsi's codegen); `test-macparity-hot` steps the identical boot through the
hand-65816 inner loop (`m68k_run_hot`) and checks the same host hash — the gate
for the assembly native handlers on real code. Both raise `MACP_N` for a longer
check (e.g. `make test-macparity-hot MACP_N=100000`); delete `build/macparity_ref.h`
first, since the host reference is regenerated only when its file is missing.

By default (`make macboot`) the boot **teleports** past the ROM's ~1.5M-instruction RAM
test to the first disk read, so the Happy Mac shows up in moments and you watch it boot
to the Finder; **`make macboot-desktop`** teleports all the way to the settled desktop
(power-on → Finder in seconds); **`make macboot-purist`** is the faithful power-on with
no teleport at all. (The targets are thin wrappers over `TELEPORT=desktop` / `FASTBOOT=0`
if you'd rather pass the flags.) The warm state each uses is generated from *your* ROM at
build time and embedded in the cart (never committed). See
[INTERNALS.md](INTERNALS.md#fastboot) and [PERF.md](PERF.md).

`raw2sfc.py` embeds your `roms/mac128k.rom` at cart offset `$080000`, `roms/sys11_data.bin`
at `$100000`, the generated fastboot warm state at `$180000`, and (if present) a second
disk at `$200000`; it pads to a 4 MB HiROM and fixes the header checksum. The resulting
`.sfc` contains Apple ROM/disk data, so **don't redistribute it** — it's build output,
and `build/` is gitignored.

The parity ROM defaults to a small step count; run more of the boot with
`make macparity MACP_N=100000` (bigger N = a longer, slower check).

### A note on speed

`macboot` is compiled `-O2` and still runs the interpreted 68000 at only a few hundred
instructions per second under Mesen, so a full boot to the desktop is tens of minutes to
hours — use the emulator's fast-forward. This is expected; see
[FUTURE.md](FUTURE.md) for the plan to make it watchable (SA-1, assembly hot paths).

## Other targets

The earlier milestone ROMs still build and are useful references:

```sh
make parity           # M2: the 68000 core alone, host<->SNES parity on a directed stream
make macvideo         # M4: a captured Mac framebuffer displayed on the SNES (static)
make hello            # M0: the build/plumbing spike
```
