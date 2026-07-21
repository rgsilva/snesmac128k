# How snesmac works

snesmac runs a Macintosh 128K on a Super Nintendo. This document explains the 68000
core, the machine model around it, the HLE `.Sony` driver that lets it read a disk, how
the whole thing is mapped into a SNES cartridge, the `runmac` host emulator, and the
parity testing that keeps host and SNES honest.

Unlike its sibling snes86 (which has a C core *and* a separate hand-written assembly
core), snesmac has **one** 68000 core — `core-c/src/m68k.c`, portable C — compiled two
ways:

- with your host C compiler, where it's validated exhaustively against hardware-style
  test vectors and cross-checked against Musashi;
- with [Calypsi](https://www.calypsi.cc/) to 65816, where it actually runs on the SNES.

A per-step **parity** check proves the two builds behave identically as a real ROM
boots. So "what boots in `runmac` boots on the SNES."

---

## The 68000 core on a 65816

### Why C, and why a purpose-built core

A 68000 is a big CPU to emulate: eight 32-bit data registers, eight 32-bit address
registers, a dozen addressing modes, and a 32-bit ALU. Hand-writing that in 65816
assembly (the snes86 approach for the much smaller 8086) would be a slog. The bet
here is that **a 68000 is very tractable in portable C**, and a good cross-compiler can
turn it into acceptable 65816.

The obvious core to reuse — [Musashi](https://github.com/kstenerud/Musashi) — does *not*
survive Calypsi:

- its dispatch is a flat `m68ki_instruction_jump_table[0x10000]` (65 536 function
  pointers). Calypsi rejects arrays that large (a 16-bit object-size ceiling);
- it stores the 68000's 32-bit registers as `unsigned int`, and on Calypsi `int` is
  **16-bit** — the register file and the whole `unsigned int m68k_read_memory_32()`
  interface would silently truncate.

So `m68k.c` is a lean, purpose-built core written to Calypsi's constraints from the
start: `uint32_t` (never `unsigned int`) for every register and effective address, and
**bitfield dispatch** — `switch (op >> 12)` into sub-decoders — instead of a 64K table.
It implements the full instruction set the Mac ROM uses: all addressing modes, the
ALU / shift / rotate / bit / BCD / MOVEM families, and the complete exception path
(traps, line-A/line-F, address error, interrupts with 24 + level autovectors).

Musashi still earns its keep — on the host it's a second, independent oracle the lean
core is diffed against.

### The memory interface

The core reaches memory through six functions it does *not* define itself:

```c
uint8_t  m68k_read8 (uint32_t a);   void m68k_write8 (uint32_t a, uint8_t  v);
uint16_t m68k_read16(uint32_t a);   void m68k_write16(uint32_t a, uint16_t v);
uint32_t m68k_read32(uint32_t a);   void m68k_write32(uint32_t a, uint32_t v);
```

Whoever hosts the core provides them. `mac.c` implements them as the real Mac address
decode; the parity harness points them at a flat buffer. This seam is also where the
host↔SNES split lives (below).

### The Calypsi minefield

Two classes of bug only appear once C meets a 16-bit compiler, and both were caught by
the SNES parity check, not by 317,500 passing host vectors:

- **16-bit `int` overflow.** `for (int i = 0; i < 0x10000; i++)` never terminates when
  `int` is 16-bit. Every counter and address is `uint32_t`.
- **`(int8_t)` doesn't sign-extend a bit-7-set byte** on Calypsi, silently breaking
  every short *backward* branch and negative `MOVEQ` (which passed all host vectors and
  then failed on the SNES at the first `BRA.s -2`). Sign extension is done with a
  branchless `sxt8(x) = (x ^ 0x80) - 0x80` instead.

---

## The machine model (`mac.c`)

`mac.c` is the virtual Macintosh around the CPU: the address map, the chips the boot
ROM pokes, and the interrupt that keeps it alive.

**Address decode.** The 68000's 24-bit space, as the Mac 128K wires it:

| range | contents |
|---|---|
| `$000000` | ROM at reset (the *overlay*), then RAM once the boot ROM clears it |
| `$400000` | 64 KB ROM |
| `$600000` | RAM (while the overlay is on) |
| `$800000` / `$A00000` | SCC (serial) |
| `$C00000` | IWM (floppy controller) |
| `$E80000` | VIA (6522) — register *N* at `$EFE1FE + N*$200` |

**The overlay.** At reset the ROM is mirrored at `$000000` so the reset vector is
found; the boot code then clears VIA port A bit 4, which flips `$000000` to RAM. `mac.c`
models exactly that latch.

**The VIA.** Enough of a 6522 to pass the ROM's power-on probe: port A/B and their data
directions, the interrupt flag/enable registers, and — critically — **CA1 wired to the
vertical blank**. Every `VBL_PERIOD` instructions `mac.c` raises the VBL, which the core
takes as a level-1 autovector interrupt. Without it the ROM's event loop never advances.

**The IWM.** Modelled only as far as the ROM's hardware self-test needs (the drive-sense
state-machine lines and a mode register it writes and reads back). Actual disk I/O does
*not* go through here — see the HLE below.

The core's `m68k_read*/write*` are `mac.c`'s bus decode, so the whole machine is just:
`if (!sony_trap()) m68k_step();` per instruction, plus the VBL timer.

---

## The HLE `.Sony` driver — how it reads a disk

The single hardest part of Macintosh emulation is the floppy: an IWM controller driving
a variable-speed drive, with 6-and-2 GCR track encoding and per-zone sector counts.
snesmac **skips all of it** by emulating the disk at the *driver* level instead of the
*hardware* level.

`sony_trap()` runs before every instruction. It intercepts the Mac's Device Manager OS
traps (opcode `$Axxx`, trap numbers `$02`–`$06` = Read/Write/Control/Status/KillIO) whose
parameter block targets the floppy driver (`ioRefNum` −5, or −4 for the drive), and
services them directly: a Read copies 512-byte sectors from the disk image at the
requested `ioPosOffset`; a Status/Control is acknowledged. The ROM's own File Manager,
Resource Manager, and boot code run for real on top of this — mount the volume, open the
System file, load it, jump to it.

Getting that to actually boot took three fixes, each found by tracing the ROM with a
disassembler:

1. **Set the condition codes from the result.** The real trap glue does a `TST D0`
   before returning, so the caller sees `Z` set for "no error". The ROM's boot-block
   accept *and* a File Manager mount completion (`beq …; moveq #0,D3`) both depend on
   it — a missing `Z` zeroed `D3` (the block number of the volume allocation map), the
   map was read from the wrong sector, its free-block count disagreed with the volume
   header, and the mount failed with `badMDBErr`.
2. **Fill in the drive geometry.** The real driver records the disk size in the
   drive-queue element when a disk is inserted; the mount validates the volume against
   it. The HLE writes `dQDrvSz = 800` (a 400 KB floppy) itself, or the mount rejects the
   volume on a size check.
3. **Run asynchronous completions.** An async request carries a completion routine
   (`ioCompletion`). The real driver's `IODone` invokes it, and *that's* what advances
   the File Manager's mount/open state machine and clears the outer request the caller
   busy-waits on. The HLE jumps to the completion itself; without it, `_MountVol` spins
   forever.

The boot chain, end to end: read boot blocks (block 0, signature `'LK'`) → run the boot
code in RAM → `_MountVol` reads the MDB (MFS signature `$D2D7`) and validates the
allocation map → open and read the System file → `SysError(40)` = the *dsGreeting*
"Welcome to Macintosh" box (normal boot, not a crash) → the Finder draws the desktop.

---

## Mapping a Macintosh into a cartridge

On the host, the Mac's RAM is a `uint8_t[131072]` array and the ROM is a pointer. On the
SNES neither works — a 128 KB array busts Calypsi's object-size limit, and 128 KB won't
fit in WRAM beside the runtime. So `mac.c` reaches memory through four macros
(`RAM_RD/RAM_WR/ROM_RD/DISK_RD`) that are plain array access on the host and, when built
`-DMAC_SNES`, call into a small backend (`snes/macmem.c`):

- **RAM → cartridge SRAM.** The Mac's 128 KB lives in HiROM cart SRAM, which the SNES
  exposes as 8 KB windows at banks `$20`–`$2F`, offset `$6000`–`$7FFF`. Mac address *a*
  maps to bank `$20 + (a >> 13)`, offset `$6000 + (a & $1FFF)`, reached by a 24-bit far
  pointer.
- **ROM + disk → cartridge ROM.** The 64 KB Mac ROM and the 400 KB floppy image are
  appended to the cart ROM at fixed byte offsets by `raw2sfc.py`. HiROM maps SNES
  `$C00000 + N` linearly to ROM byte *N*, so the ROM sits at `$C80000` and the disk at
  `$D00000`, read through far pointers. The cart is a 2 MB HiROM (its own header,
  `macheader.s`, so the embedded blobs aren't mirrored over the code).

Two 65816-specific gotchas, both expensive to find:

- **Declare every accessor.** An implicit (C89) call makes Calypsi pass arguments with
  the 16-bit-int ABI, silently corrupting a 32-bit address or an 8-bit value — it looked
  like a broken SRAM write for an hour.
- **Cart SRAM is battery-backed**, so Mesen persists it between runs; `mac_init` wipes
  RAM on start, so the machine boots fresh every time regardless.

### The display

The Mac framebuffer is 512×342, 1 bit per pixel, at `$1A700` in RAM. A set bit is black.
`macboot.c` shows it the same way snes86 shows CGA text: as SNES tiles. Every so many
instructions it rebuilds a 256×224 centre crop into a WRAM tile buffer — each Mac
framebuffer byte becomes the plane-0 byte of a 2bpp tile (Mac 1 = colour 1 = black,
plane 1 = 0) — and DMAs it to VRAM. A two-colour palette (white / black) and an identity
tilemap complete a Mode-0 background. Only a 256×224 window is shown — the SNES can
address just 1024 tiles per background, and the full 512×342 screen is 2752 unique tiles
— so the window **pans** (the L/R shoulder buttons and the d-pad, read from the auto-read
joypad). To stay responsive on the slow CPU, the per-refresh framebuffer read computes
the SRAM window pointer once per row (rows align to the 8 KB windows) and reads across it
with plain indexing, rather than the full window math per byte. A fit-to-screen
(downscaled) mode is still on the roadmap ([FUTURE.md](FUTURE.md)).

---

## `runmac` — the host emulator

`core-c/src/runmac.c` is the same machine model (`mac.c` + `m68k.c`) on your desktop,
driven from `main()`: load the ROM, attach a disk image, step, and dump the framebuffer
as a PBM. It's the development mirror — reproduce a boot problem with a disassembler and
traces in milliseconds, no SNES layer in the way — and it's where every HLE fix above was
found before it ever ran on Calypsi.

---

## Parity testing

Two independent checks, and (as in snes86) don't conflate them.

### 1. The core vs the vectors

`core-c/tests/run_vectors.py` drives the lean core through the
[SingleStepTests/m68000](https://github.com/SingleStepTests/m68000) vectors — set the
initial registers + RAM, single-step once, compare the final state and every touched
byte. **127 files / 317,500 vectors: 100 % accounted for, zero unexplained failures.**
The non-exact remainder is entirely benign, documented classes (odd-address fault
frames, undefined-result BCD/DIV/CHK/`LINK A7`/`TAS`/`TRAPV`) that well-behaved Mac code
never hits. The same harness runs Musashi (`libem68k.dylib`) and the lean core
(`libem68k_lean.dylib`) behind one ABI, so they can be diffed against each other.

### 2. The host core vs the SNES core, in lockstep

This is the one that guards the actual port. `snes/macparity.c` boots the real ROM on
the SNES for N instructions and folds a cumulative hash of the whole 68000 register file
after each step, plus a final hash of all 128 KB of RAM. `core-c/tests/macgenref.c` does
the identical run on the host (shared fold in `macparity_hash.h`) and emits the expected
hashes; the ROM compares and reports over a WRAM mailbox that a Mesen Lua script drains.
**PARITY OK** — the SNES matches the host exactly. This is the check that caught the two
Calypsi codegen bugs above.

```sh
cd snes && make test-macparity      # boot N steps on the SNES, diff vs the host
```

---

## Where to look

| you want… | start at |
|---|---|
| the 68000 core: dispatch, ALU, exceptions | `core-c/src/m68k.c` (+ `m68k.h`) |
| the Mac machine, VIA, overlay, the HLE `.Sony` driver | `core-c/src/mac.c` |
| the host emulator / development mirror | `core-c/src/runmac.c` |
| the SNES memory backend (SRAM/ROM) | `snes/macmem.c` |
| the live-boot display | `snes/macboot.c` |
| host↔SNES parity | `snes/macparity.c`, `core-c/tests/macgenref.c` |
