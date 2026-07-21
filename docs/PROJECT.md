# snesmac — a Macintosh 128K on the SNES

Boot a Macintosh 128K (Motorola 68000, 128 KB RAM, 512×342 1-bit screen) on a
stock SNES. Slow is fine; "kinda broken" is fine. This is the third machine in
the family after **snes86** (an 8086 PC booting MS-DOS) and **snespico8** (a
PICO-8 runtime).

## The core bet

A Mac 128K sounds harder than the 8086 box, but it lands **closer to snespico8
than to snes86**, because of one decision: the 68000 is written **once, in
portable C, compiled to the 65816 with Calypsi** — not hand-written in 65816
assembly. Hand-writing a 68000 interpreter in asm (8×32-bit data + 8×32-bit
address registers, a huge addressing-mode matrix, 32-bit ALU on a 16-bit core)
would be the project killer. snespico8 already proved the escape hatch: write
the machine in C, let Calypsi compile it, eat the slowness.

Three facts make it feasible, and all three are load-bearing:

1. **The parity harness transfers.** [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000)
   provides TomHarte-style per-instruction JSON vectors for the 68000 — the same
   family as the 8088 v2 vectors snes86's C core passes. CPU correctness becomes
   the same solved-shape problem: set state → single-step → diff.
2. **C-to-SNES is proven.** snespico8's Calypsi toolchain (`--core=65816
   --target=snes --code-model=large -O0`), its cartridge-SRAM memory strategy,
   and its Mesen `--testrunner` + mailbox harness all apply unchanged.
3. **The memory problem is already solved.** snes86's BIGMEM build gives a
   172 KB guest from WRAM + cart SRAM; snespico8 probes 256 KB of cart SRAM at
   boot. The Mac's 128 KB RAM + 64 KB ROM is *smaller* than the 1 MB 8086 space
   that already boots MS-DOS.

## Architecture — which layer borrows from where

| Layer                       | Approach                                          | Borrowed from |
|-----------------------------|---------------------------------------------------|---------------|
| 68000 CPU core              | **lean C core** (`core-c/src/m68k.c`), Calypsi → 65816 | snespico8 |
| CPU oracle                  | Musashi (host only) — the reference to diff against | (Musashi won't compile on Calypsi) |
| CPU correctness             | both cores vs SingleStepTests/m68000 vectors      | snes86 (parity layer 1) |
| SNES-build correctness      | same-source host ⇄ Mesen, state-hash parity       | snespico8 (`snesparity.sh`) |
| Guest RAM > WRAM            | cartridge SRAM windows (→ SA-1 later)             | snespico8 `probe_sram` / snes86 BIGMEM |
| Machine model (VIA/IWM/SCC) | **HLE**; host `runmac` shares the contract        | snes86 (`runpc`/`dosboot`) |
| Machine correctness         | boot-trace lockstep vs Mini vMac / Musashi        | snes86 (`tracewin.sh`) |
| Display                     | 512×342 1bpp → planar tiles, dirty-rect           | new (snespico8 DMA/mailbox scaffolding) |
| Test automation             | Mesen `--testrunner` + Lua mailbox, RESULT PASS/FAIL | both       |

## Hardware budget (Mac 128K → SNES)

- **CPU** 68000 @ 7.83 MHz → interpreted in C on the 65816 @ 3.58 MHz. Expect
  ~1000× slower than real: **tens of minutes to boot to desktop on stock**,
  minutes on SA-1. A "watch it come alive" demo, not interactive computing.
- **RAM** 128 KB, read/write → cartridge SRAM (the emulator's own C runtime
  needs WRAM). The accessor functions translate the guest 24-bit address to
  WRAM / SRAM-window / ROM / I/O, exactly like any machine emulator. SA-1's
  linear 256 KB BW-RAM is the natural upgrade.
- **ROM** 64 KB, read-only → cartridge ROM. Apple copyright: **bring-your-own**.
- **Display** 512×342 1-bit → hi-res Mode 5/6 planar tiles. A Mac framebuffer
  byte (8 px, MSB-left, 1=black) *is* a valid SNES 2bpp bitplane-0 row, so the
  conversion is mostly a tile-major gather + zeroed 2nd plane. 512-wide needs
  ≥2752 unique tiles vs the 1024/BG limit → blank-tile dedup + dirty-rect, with
  an HDMA tile-base-swap or Mode-7 downscale fallback. Rendering is not the
  bottleneck (the CPU is), so slow blits are fine.
- **Disk/boot** 400 KB floppy → **HLE the ROM's `.Sony` driver**: serve sectors
  from a disk image in cart ROM (writes → SRAM overlay). Don't emulate IWM GCR.
- **Interrupts** generate the 60.15 Hz VBL so the cursor/Time Manager tick.
- **Sound** deferred / never (Mac PWM → SNES APU is its own project).

## Milestones

- **M0 — Scaffold + Calypsi spike.** `core-c/` (C 68000 + host `runmac` + vector
  runner) and `snes/` (Calypsi glue cloned from snespico8). Gate: a C stub
  compiles to `.sfc`, runs in Mesen, drains the mailbox. *(hello.c ready.)*
- **M1 — 68000 C core passes the m68000 vectors.** ← **the validation gate.**
  Gate: 100% of normal execution; address-error-frame details and TAS/TRAPV are
  documented divergences. *(done — see below.)*
- **M1a — Musashi under Calypsi. (done — NO.)** Two hard blockers: Musashi's
  dispatch is a flat `[0x10000]` jump table (Calypsi errors "array too large",
  16-bit object ceiling), and it stores 32-bit registers in `unsigned int`
  (16-bit on Calypsi → silent truncation). Pivot: keep Musashi as the *host
  oracle*, write a lean core for the SNES.
- **M1b — lean 68000 core. (done.)** `core-c/src/m68k.c` (~470 lines):
  `uint32_t` registers, bitfield dispatch (no 64K table). Compiles clean on
  Calypsi `--target=snes` (68 KB object). See result below.
- **M2 — Parity harness (C ⇄ SNES). (done.)** The Calypsi-compiled core runs on
  the SNES (Mesen) and matches the host bit-for-bit. Caught two real Calypsi
  bugs the host validation can't. See result below.
- **M3 — Mac machine model, boot ROM on host. (done — boots to the Happy Mac.)**
  `core-c/src/mac.c` + `runmac.c`: the real memory map (ROM-overlay at reset,
  RAM/ROM windows, VIA/IWM/SCC decode), VBL interrupt (core IRQ support added),
  minimal VIA (overlay + IFR/IER) and IWM (mode-register probe). A genuine Mac
  128K ROM boots through power-on, RAM test, the overlay switch, QuickDraw, the
  gray desktop, and draws the Happy Mac. See result below.
- **M4 — Framebuffer → SNES. (done — Happy Mac on real SNES output.)** Host
  boots the Mac, captures the framebuffer, and emits the center 256×224 crop as
  2bpp tiles (`mkmacfb`); the SNES ROM (`snes/macvideo.c`, Mode 0) DMAs them to
  VRAM and displays the Happy Mac in Mesen. See result below. (Full 512-wide
  hi-res + a live on-SNES boot are later refinements.)
- **M5 — Disk HLE → desktop. ✓** Trap `.Sony`, serve sectors from the image.
  System 1.1 boots to the Finder desktop (host). *The* milestone — see below.
- **M6 — Mouse.** SNES pad → mouse move/click via VIA/SCC HLE. Keyboard stretch.
- **M7 — Perf + SA-1 + release.** SA-1 for linear RAM + 3× speed; asm hot-paths;
  BYO-ROM public build + copyright scrub (snes86 release-packaging playbook).

## M1 — validation result

The 68000 core is **Musashi** (Karl Stenerud, MIT), vendored under
`core-c/musashi/`. The host harness `core-c/tests/m68ktest.c` wraps it as a
ctypes-drivable dylib (`libem68k.dylib`); `core-c/tests/run_vectors.py` decodes
the `.json.bin` vectors (via the repo's `decode.py` format) and single-steps
each one — the same shape as snes86's `run_vectors.py` + `libem86.dylib`.

Key conventions pinned during bring-up:
- **PC/prefetch**: the vectors report PC 4 bytes ahead of the opcode (2-word
  prefetch). Musashi runs with prefetch OFF and tracks true PC, so we set
  `PC = initial.pc - 4` and compare `tm_get(PC) + 4`.
- **A7 banking**: the vectors give `usp` and `ssp` separately; the active A7 is
  selected by the SR supervisor bit. Set/read mode-aware.
- **Single-step**: `m68k_pulse_reset()` burns the next `m68k_execute` on the
  ~40-cycle reset sequence, so we reset once at init and per-test only clear the
  `stopped`/`halted` flag (`m68kglue.c`).
- **STOP PC**: STOP halts and suppresses the trailing prefetch, so its final PC
  uses offset 0, not the usual +4 — special-cased.
- **Write-log**: the harness logs every byte Musashi writes and zeros exactly
  those between tests. Without it, stray writes not in a vector's final RAM
  (e.g. an odd-address write that would fault on real HW) leak across the ~317k
  tests of a single-process run and corrupt later ones.
- **Config**: trace OFF (enabling it drops NOP 100%→49% — the vectors don't
  model per-instruction trace the Musashi way) and **address-error emulation
  OFF** (Musashi runs a slow runaway handler per odd-address fault, ~50× slower,
  and can't single-step it cleanly; odd-address vectors then diverge fast and
  bucket correctly from the *test's* expected final state).

**Result (127 files, 317,500 vectors — `core-c/tests/build/FULLRUN.txt`):**
**81.5% pass byte-exact; 100.000% accounted for; ZERO unexplained failures.**
58 opcodes pass 100% exact. The remaining ~18% is entirely two benign,
documented classes: (1) **56,982 odd-address faults** — the 68000 takes an
address error, whose 14-byte frame Musashi only approximates vs the MAME
microcode core that generated these vectors (irrelevant to well-behaved Mac
code — an address error is a crash on real HW); (2) **1,624 undefined-result
ops** — invalid-BCD inputs to ABCD/SBCD/NBCD, DIVU/DIVS overflow, CHK, LINK A7,
and the vector authors' own known-bad TAS/TRAPV. Normal instruction execution
and all normal exceptions match **exactly**. The 68000 core is validated.

## M1b — lean core validation result

The shipped-to-SNES core (`core-c/src/m68k.c`) is validated against the same
vectors via the same harness — `EM68K_LIB=build/libem68k_lean.dylib` swaps it in
for Musashi (`make -C core-c/tests test-lean`). It is a full 68000: all
addressing modes, the ALU/shift/rotate/bit/BCD groups, MOVEM, and the
exception path (TRAP, TRAPV, CHK, zero-divide, privilege, line-A/line-F — all
2500/2500; line-A traps are how the Mac Toolbox is called, so that mattering is
the point).

**127 files, 317,500 vectors: 81.1% byte-exact, 100.000% accounted for, 0
unexplained failures** (59 opcodes 100% exact). Same benign classes as Musashi:
odd-address faults (the core doesn't fault-frame-match, irrelevant to the Mac)
and the undefined-result ops (invalid-BCD, DIV overflow, CHK flags, LINK A7,
TAS/TRAPV). MULU/MULS/DIVU/DIVS pass all defined cases exactly. Getting there
was a short bug hunt with the harness green at each step: the rotate type
encoding (ROX↔RO swapped), CMPM's postincrement, Bcc's `0xFF` short displacement,
UNLK A7, MOVEM's reversed predecrement order, and the `#imm` operand forms of
BTST / MOVE-to-CCR. The lean core is validated.

## M2 — SNES parity result

A 68000 now runs *on the SNES*. A directed 68000 program + canonical state is
executed by the same `m68k.c` on host and SNES (its 64 KB address space backed
by WRAM bank $7F), folding the full register file into an FNV hash each step.
The host build computes 64 per-step reference hashes; the Calypsi ROM computes
its own, compares in C, and mails the first mismatch. Driver:
`core-c/tests/parity_snes.sh` (→ `make -C snes test-parity`).

**Result: PARITY OK** — identical hashes for all 64 steps. And the harness did
its job, catching two Calypsi issues invisible to host-only testing:
1. a 16-bit-int overflow in the 64 KB clear loop (`int i < 0x10000` never
   terminates — Calypsi `int` is 16-bit); glue, fixed with a `uint32_t` counter.
2. **`(int8_t)` sign-extension miscompiled** in the core — a value with bit 7
   set didn't sign-extend, silently breaking every short *backward* branch and
   negative `MOVEQ`. Fixed with a branchless `sxt8()` (`(x^0x80)-0x80`). This is
   the M2 payoff: a codegen bug that passes all 317,500 host vectors but breaks
   on real SNES hardware.

## M3 — the Mac boots (Happy Mac)

`core-c/src/mac.c` wraps the lean core with the real Macintosh 128K machine: the
24-bit address decode (ROM-overlay at reset → RAM at $0 once the boot clears VIA
PA4; ROM $400000; SCC/IWM/VIA windows), a minimal VIA (overlay latch, IFR/IER,
CA1=VBL), and an IWM stubbed just enough to pass the ROM's mode-register probe
(no disk). The core gained autovector interrupt support (`m68kc.irq`, vector
24+level) for the 60 Hz VBL. Host driver: `core-c/src/runmac.c`
(`make -C core-c/tests boot`).

**Result:** a genuine Macintosh 128K ROM (checksum `0x28BA61CE`, validated)
boots — reset at `$40002A`, ROM checksum, IWM probe, **overlay switch** (RAM at
`$0`, real stack at `$FBE0`), QuickDraw init, the 50% gray desktop, and the
**Happy Mac** drawn dead-center — then settles into the disk-search loop with
VBL interrupts firing. Hardware self-test passed (a failure would draw the Sad
Mac with a hex code). The bring-up was a "boot and fix what it polls" loop: the
IWM status bit-5 spin, then its mode-register write/read-back probe, were the
two gates to get past. Advancing to the flashing "insert disk" icon and loading
a System is M5 (disk HLE). The ROM is Apple copyright — bring-your-own,
gitignored at `roms/mac128k.rom`.

## M4 — the Happy Mac on real SNES output

The video path, proven end to end: `core-c/src/mkmacfb.c` boots the Mac on the
host, captures the `$1A700` framebuffer, and emits the center 256×224 crop as
SNES 2bpp tiles (a Mac framebuffer byte — 8 px, MSB-left, 1=black — *is* a valid
plane-0 row; the crop is byte-aligned so it's a direct gather, no dedup needed:
896 tiles < the 1024/BG limit). `snes/macvideo.c` (Mode 0, one 2bpp BG) DMAs the
tiles to VRAM, writes an identity tilemap, sets a white/black palette, and turns
the screen on. `make -C snes test-macvideo` runs it headless in Mesen; the Lua
harness hex-dumps `emu.takeScreenshot()` (Mesen sandboxes `io`) and the shell
decodes the PNG.

**Result: the Happy Mac renders correctly on SNES output** — black-on-white on
the gray checkerboard desktop, dead center. Two bring-up bugs: the DMA/geometry
was right first try, but CGRAM (`$2122`) is a byte register — a 16-bit store put
the high colour byte in `$2123` (garbage colours until fixed with two byte
writes). A *live* boot on the SNES (the interpreter is far too slow at -O0 to
reach the Happy Mac in reasonable time) and full 512-wide hi-res are later
work. The captured framebuffer contains Apple's Happy Mac artwork, so the
generated tiles (and any screenshot) are gitignored, like the ROM.

## M5 — disk boot to the desktop ✓ (System 1.1 Finder, host)

**Done — a real Mac 128K ROM boots System 1.1 to the Finder desktop:** the
🍎 File/Edit/View/Special menu bar, the "System Disk" floppy icon, and the
Trash. Reached by **high-level-emulating the `.Sony` disk driver** rather than
finishing the IWM/GCR path (which is the single hardest part of Mac emulation
and was abandoned as too deep for the payoff).

`sony_trap()` in `mac.c` (called from `mac_step` before each instruction)
intercepts Device-Manager OS traps — opcode `(op & 0xF800) == 0xA000`, trap
number 0x02–0x06 (Read/Write/Control/Status/KillIO) — whose paramblock (`A0`)
`ioRefNum` (+0x18) is **−5** (the `.Sony` driver) or **−4** (the drive itself,
reads only). For a read it copies sectors straight from the disk image
(`roms/sys11_data.bin`) at the absolute `ioPosOffset` (+0x2E), fills
`ioActCount`/`ioResult`, and returns — skipping the driver, the IWM, GCR
encoding, and motor-speed timing entirely.

Three fixes were each load-bearing (and each needed a deep host-side trace —
capstone with the M68K backend was the tool that cracked the ROM disassembly):

1. **CCR from the result.** The real trap glue does `TST D0` before returning,
   so the caller sees `Z` set for `noErr`. The boot block's `_Read; BEQ` accept
   relies on it, and — critically — a mount completion runs
   `beq …; moveq #0,D3` right after restoring its saved registers. Missing `Z`
   fell through and zeroed `D3` (the block number of the volume allocation map),
   so the map's continuation was read from block 1 instead of block 3, the
   free-block count disagreed with `drFreeBks`, and `_MountVol` failed with
   `badMDBErr` (−60).
2. **Drive geometry.** The real driver stores the disk size in the drive-queue
   element on insertion; the mount validates the volume against it. The HLE
   walks `DrvQHdr` (`$030A` → qHead) and sets `dQDrvSz` (+12) = 800 (400 KB /
   512), else the mount rejects the volume on the size check.
3. **Asynchronous `IODone`.** An async request (trap bit 10, opcode `0xA402`)
   carrying an `ioCompletion` (pb + 0x0C ≠ 0) must have that routine *run* — the
   HLE jumps to it (`A0` = pb, `D0` = 0, `Z` = 1, return address past the trap).
   It advances the File Manager's mount/open state machine and clears the outer
   request's `ioResult` that the caller busy-waits on; without it `_MountVol`
   spins forever polling its paramblock.

Boot chain, all traced: read boot blocks (block 0 `LK` = `0x4C4B`) → execute the
boot code in RAM at `$010000` → `_MountVol` reads the MDB at block 2 (MFS
signature `0xD2D7`) and validates the allocation map → open + read the System
file → `_SysError(40)` = `dsGreeting`, the **"Welcome to Macintosh"** box (normal
boot, not a crash) → the Finder draws the desktop.

Disk images and the ROM are Apple copyright — bring-your-own, gitignored; the
desktop screenshot is likewise not committed. Next: try the System 2.0 disk
(swap `roms/sys11_data.bin`), then the live-on-SNES port (M4b).

## M4b — the live machine on the SNES (parity ✓, boot perf-bound)

The machine model itself now runs on real hardware, not just the host. `mac.c`
compiles `-DMAC_SNES` and, behind `RAM_RD/RAM_WR/ROM_RD/DISK_RD` macros, reaches
a small SNES memory backend (`snes/macmem.c`): the Mac's **128 KB RAM lives in
HiROM cart SRAM** (8 KB windows at banks `$20-$2F:6000-7FFF`), and the **64 KB
ROM + 400 KB floppy are embedded in the cart ROM** at fixed offsets (`raw2sfc.py
--at 0x080000 / 0x100000` → HiROM's linear map puts them at `$C80000` / `$D00000`
for far-pointer reads).

`snes/macparity.c` extends the M2 parity idea to the whole machine: it boots the
real ROM for N steps on the SNES and compares a per-step cumulative CPU hash plus
a final RAM hash against a host reference (`core-c/tests/macgenref.c`, shared fold
in `macparity_hash.h`). **Result: PARITY OK — the SNES matches the host exactly**
(cpu `303993EB` at N=500), so the SRAM/ROM backend and the machine model are
faithful on Calypsi at `-O2`.

`snes/macboot.c` is the live boot: it runs the machine and, every few thousand
instructions, rebuilds a 256×224 crop of the framebuffer (in SRAM) into a WRAM
tile buffer and DMAs it to VRAM — the M4 Mode-0 path, live. Verified running in
Mesen (it boots and refreshes the display). But it is **slow**: ~tens-to-hundreds
of 68000-steps/sec under `-O2`, so a full boot (~3M steps to the desktop) is tens
of minutes to hours — the documented "~1000× slower". A live boot is a
leave-it-running / background demo; **SA-1 + asm hot paths (M7) is the fix for a
watchable boot.** `make -C snes macparity | macboot`.

## Legal

The 64 KB Mac 128K ROM and the System 0.97 / Finder 1.0 disk are Apple
copyright, and — unlike MS-DOS (MIT-freed) — there is no freely-licensed Mac
ROM. So the public build is **bring-your-own-ROM**: the emulator ships freely,
but booting a real Mac OS needs the user's own dumps (or a homebrew 68000
payload). Same copyright-scrub playbook as snes86's release packaging.
