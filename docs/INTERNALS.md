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

The single C core is still the source of truth for *behaviour*. Where the SNES build
needs speed there's a hand-written 65816 layer over it — an inner-loop dispatcher, the
memory fast paths, the video copy — but each of those either mirrors the C exactly
(and is held to it by the same parity check) or falls back to the C for anything it
doesn't handle. The optimization campaign that produced that layer — and got a cold
boot from *hours* down to minutes — is its own document: **[PERF.md](PERF.md)**, with
a measured cost breakdown, a ranked fix list, and a committable Apple-free benchmark
ROM that scores every change. This file is the architecture; PERF.md is the speed.

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

**The mouse (also HLE).** Rather than emulate the quadrature encoders and their
SCC/VIA interrupts, `mac_set_mouse(x, y, btn)` writes the cursor position straight into
the ROM's low-memory mouse globals — `MTemp`/`RawMouse` (a QuickDraw `Point {v,h}`) and
the `CrsrNew` flag — which the VBL cursor task then follows and redraws. The button is a
real VIA line (port B bit 3, low = pressed), so the Event Manager posts mouse-down/up as
usual and menus and clicks just work. (Validated on the host by clicking the 🍎 menu
open; the SNES feeds it from the d-pad + A.)

**The keyboard (also HLE).** The real Mac keyboard is a serial protocol over a VIA
shift register; snesmac skips it and injects at the *event* level. `mac_post_key(code,
char)` queues one keyDown; the A-line hook (below) watches for the guest running an
event-polling trap (`GetNextEvent`, `EventAvail`, `GetOSEvent`, `OSEventAvail`) with a
key pending, and *piggybacks the ROM's own `_PostEvent`* — it pushes the poll's return
address, sets `A0 = keyDown`, `D0 = (keycode << 8) | char`, and jumps to `_PostEvent`
through the trap-dispatch table (the 64 KB ROM's is 512 compressed 16-bit entries at
`$0400`, address = `ROMBase + entry*2`). `_PostEvent` returns straight back into
re-running the poll, which now finds a genuine event in the queue. Verified on the host
by asserting the keyDown lands in the Event Manager queue before any app reads it; the
SNES drives it from an on-screen keyboard (SELECT, below).

**Fetch once.** The core's `m68k_read*/write*` are `mac.c`'s bus decode. A naïve loop
would fetch each opcode twice — once for the sony/keyboard trap check, once to execute —
so instead the trap check is an **A-line hook** (`m68k_aline_hook`): `m68k_step()` does
the single opcode fetch, and *only* on an `$Axxx` opcode calls the hook, which either
services the trap (disk / keyboard) or declines to the normal line-A exception. The
whole machine step is then `m68k_step()` plus the VBL/timer tick; on the SNES the batch
entry point `mac_run(n)` runs that loop in hand 65816 (see PERF.md).

---

## The HLE `.Sony` driver — how it reads a disk

The single hardest part of Macintosh emulation is the floppy: an IWM controller driving
a variable-speed drive, with 6-and-2 GCR track encoding and per-zone sector counts.
snesmac **skips all of it** by emulating the disk at the *driver* level instead of the
*hardware* level.

`sony_trap()` is the A-line hook (so it runs only on `$Axxx` opcodes, not every
instruction). It intercepts the Mac's Device Manager OS traps (trap numbers `$02`–`$06`
= Read/Write/Control/Status/KillIO) whose parameter block targets the floppy driver
(`ioRefNum` −5, or −4 for the drive), and services them directly: a Read copies
512-byte sectors from the disk image at the requested `ioPosOffset`; a Status/Control
is acknowledged. The ROM's own File Manager, Resource Manager, and boot code run for
real on top of this — mount the volume, open the System file, load it, jump to it.

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

**Two drives.** The Mac 128K has an internal drive and can take an external one. The
drive the ROM is talking to comes from the IWM SELECT line, which `drive_sense()`
honours: with no second image the external drive's sense lines read "nothing connected"
and the ROM sees one drive; drop a 400 KB image at `roms/disk2_data.bin` and the
external drive is real — the HLE dispatches each read on the parameter block's
`ioVRefNum` (1 → the internal image, 2 → the external one) and sets each drive-queue
entry's size per `dQDrive`. So a second volume — an apps disk — mounts on the desktop.
(Before this the sense stub answered "disk present" for *both* drives and the HLE
ignored the drive number, so the one image mounted twice: the "two System Disks" bug.)

---

## Mapping a Macintosh into a cartridge

On the host, the Mac's RAM is a `uint8_t[131072]` array and the ROM is a pointer. On the
SNES neither works — a 128 KB array busts Calypsi's object-size limit, and 128 KB won't
fit in WRAM beside the runtime. So `mac.c` reaches memory through macros
(`RAM_RD/RAM_WR/ROM_RD/DISK_RD`, plus 16-bit variants) that are plain array access on
the host and, built `-DMAC_SNES`, are `static inline` accessors in `snes/macmem.h` —
inline so the address arithmetic lands straight in the bus decode with no call per
access.

- **RAM → half WRAM, half cartridge SRAM.** The Mac's low 8 KB (`$0000`–`$1FFF`) lives
  in one fixed cart-SRAM window; the rest (`$2000`–`$1FFFF`) is **WRAM at `$7E0000 + a`,
  linear** — one compare picks the region, then a constant-base far access, no window
  math. (An earlier design put *all* 128 KB in 8 KB SRAM windows and paid a bank
  computation on every byte; PERF.md item H replaced it. The runtime is squeezed under
  `$2000` so WRAM `$7E2000+` is free for the Mac.)
- **ROM + disks → cartridge ROM.** The 64 KB Mac ROM, the 400 KB floppy image, the
  fastboot warm state (below), and the optional second disk are appended to the cart at
  fixed byte offsets by `raw2sfc.py`. HiROM maps SNES `$C00000 + N` linearly to ROM byte
  *N*, so the ROM sits at `$C80000`, disk 1 at `$D00000`, the warm state at `$D80000`,
  disk 2 at `$E00000`, all read through far pointers. The cart is a 4 MB HiROM with its
  own header (`macheader.s`) so the embedded blobs aren't mirrored over the code, and it
  runs **FastROM** (map mode `$31` + `MEMSEL`; SRAM is reached through its fast `$A0`
  mirror), 6 master clocks per access instead of 8.

Two 65816-specific gotchas, both expensive to find:

- **Declare every accessor.** An implicit (C89) call makes Calypsi pass arguments with
  the 16-bit-int ABI, silently corrupting a 32-bit address or an 8-bit value — it looked
  like a broken SRAM write for an hour.
- **Cart SRAM is battery-backed**, so Mesen persists it between runs; `mac_init` wipes
  RAM on start (a 65816 `MVN` block fill), so the machine boots fresh every time.

### The display

The Mac framebuffer is 512×342, 1 bit per pixel, at `$1A700` in RAM (now linear WRAM). A
set bit is black. `macboot.c` shows it the same way snes86 shows CGA text: as SNES
tiles. It rebuilds a 256×224 window into a tile buffer — each Mac framebuffer byte
becomes the plane-0 byte of a 2bpp tile (Mac 1 = colour 1 = black) — and DMAs it to
VRAM, behind a two-colour palette and an identity Mode-0 tilemap. The rebuild is hand
65816 (`calypsi/tilecopy.s`, `tile_rows`): fully-unrolled strided copies straight from
WRAM into the SRAM tile buffer, ~13× cheaper than the original C loop, so panning and
the mouse feel responsive despite the slow CPU.

Only a 256×224 window is shown — the SNES addresses just 1024 tiles per background, and
the whole screen is 2752 — so there are two views:

- **1:1 pan** (default): hold the **L** shoulder and the **d-pad** slides the window
  (the d-pad alone is the mouse).
- **zoom-out** (**R** toggles): the *whole* 512×342 screen at once, 2×2-downscaled to
  256×171 with **four gray levels**. Because the 1:1 view is 1-bit, the second bitplane
  of the 2bpp mode and two spare palette entries were free; each output pixel maps its
  2×2 block's black-count (0–4) to white / light / dark / black, so the dithered desktop
  averages to a real gray instead of collapsing. It's table-driven (`tools/mkzoomtab.py`
  → `maczoom_tab.h`), a second renderer into the same tile buffer.

**The on-screen keyboard** is a second background (BG2) across the bottom five tile
rows: **SELECT** toggles it, the **d-pad** moves the highlighted key, **A** presses it
(calling `mac_post_key`), and a SHIFT key gives the shifted characters. Its tiles and
layout are generated (`tools/mkkbd.py` → `mackbd_tab.h`, from a vendored public-domain
8×8 font). Two SNES gotchas shaped it: VRAM is writable only during blank (the tilemap
update is force-blanked, like the DMA), and BG2 ignores the tilemap palette bits in
Mode 0 — so the selected-key highlight uses *inverted tiles* rather than a palette swap.

---

## `runmac` — the host emulator

`core-c/src/runmac.c` is the same machine model (`mac.c` + `m68k.c`) on your desktop,
driven from `main()`: load the ROM, attach a disk image (and `roms/disk2_data.bin` if
present), step, and dump the framebuffer as a PBM. It's the development mirror —
reproduce a boot problem with a disassembler and traces in milliseconds, no SNES layer
in the way — and it's where every HLE fix above was found before it ever ran on Calypsi.

### Fastboot

The 128K ROM burns ~1.5 million instructions before its first disk read — the power-on
RAM test, hardware init, and the boot-chime delay loops — none of it watchable. So
`make macboot` builds with `FASTBOOT=1` by default: a host tool boots *your* ROM up to
the instruction about to issue that first read and dumps the whole machine (registers,
VIA, all 128 KB of RAM); the live boot embeds that state in the cart and **teleports**
to it when it first reaches the RAM-test entry PC. The result is bit-identical to having
waited — verified by comparing framebuffer and register hashes against a cold boot at
several checkpoints — minus the wait, so the Happy Mac appears in moments.

There are two teleport targets, chosen by `TELEPORT`:

- **`boot`** (default) lands at the first disk read, so you still *watch* the disk boot:
  Happy Mac → Welcome → Finder.
- **`desktop`** (`make macboot TELEPORT=desktop`) runs the host boot all the way to the
  point where the framebuffer stops changing — the Finder desktop, drawn and idle — and
  captures *that*, so power-on jumps straight to the desktop in seconds.

Build `FASTBOOT=0` for the purist power-on. The warm state is regenerated from your own
ROM at build time and never committed. (Details, and the `_BlockMove` HLE that rides
with it, in [PERF.md](PERF.md).)

---

## Parity testing

Two independent checks, and (as in snes86) don't conflate them. (A third, the
benchmark ROM's host-vs-SNES CRC, is an *Apple-free* semantic gate that also times
every optimization — see [PERF.md](PERF.md).)

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
| the Mac machine, VIA, overlay, HLE `.Sony`/mouse/keyboard | `core-c/src/mac.c` |
| the host emulator / development mirror | `core-c/src/runmac.c` |
| the SNES memory backend (WRAM/SRAM/ROM, inline) | `snes/macmem.h`, `snes/m68kbus.h` |
| the hand-65816 inner loop + native handlers | `snes/calypsi/m68krun.s` |
| the hand-65816 video/RAM copy loops | `snes/calypsi/tilecopy.s` |
| the live-boot display, pan/zoom, on-screen keyboard | `snes/macboot.c` |
| the on-screen keyboard / zoom lookup tables | `snes/tools/mkkbd.py`, `mkzoomtab.py` |
| host↔SNES parity | `snes/macparity.c`, `core-c/tests/macgenref.c` |
| the benchmark ROM + timing harness | `bench/mkbenchrom.py`, `snes/macbench.c` |
| the fastboot warm-state generator | `core-c/tests/macfastboot_gen.c` |
| the whole performance story | [PERF.md](PERF.md) |
