# Performance: where the time goes, and the plan to get it back

The live machine runs ~205 68000 instructions/second — about 13,000 SNES
master cycles per 68000 instruction (2.68 MHz / 205). A tuned C interpreter
on a 65816 should land in the 1,000–2,000 range and a hand-tuned core well
under that, so there is roughly an order of magnitude on the table *before*
SA-1 hardware. This document is the plan of record for getting it: what was
measured, what to change, in what order, and how every change is validated.

The analysis below comes from compiling the actual sources to 65816 assembly
(`cc65816 -S`) and counting instructions and calls in the hot paths, not from
guessing. Estimates are marked as estimates.

## 1. Where the time goes (measured)

Every 68000 instruction pays, in order:

| # | Cost | Evidence |
|---|------|----------|
| 1 | **The opcode is fetched twice.** `sony_trap()` runs first in `mac_step()` and does a full `m68k_read16(pc)` just to test for an A-trap — then `m68k_step()` fetches the same word again. | `mac.c` `sony_trap` guard; `m68k_step` `fetch16()` |
| 2 | **A 32-bit modulo per step.** `instr_count % VBL_PERIOD` is a runtime long-division call on Calypsi — hundreds of cycles, every instruction, to ask "is it VBL yet". | `mac.c` `mac_step` |
| 3 | **Byte-at-a-time bus.** `m68k_read16` = 2 × `bus_read`, `read32` = 4 ×. Each `bus_read` runs the full address-range decode (compiled with 17 jsr/jsl in it) and then the SRAM window math. | `mac.c:194-201`; `mac.asm` |
| 4 | **Window math per byte.** `MAC_SRAM(a)` = `>>13`, `*0x10000`, masks — ~35–45 instructions plus two `jsl`/`rtl` pairs per byte accessed. | `mm.asm` `?L17`/`?L18` |
| 5 | **SlowROM.** Header map mode is `$21` (HiROM, slow) and `$420D` is never set: every code/ROM/SRAM access costs 8 master clocks that could cost 6. | `macheader.s`; no `MEMSEL` anywhere |
| 6 | **Cross-call factoring.** `-O2` outlines repeated sequences into local `?L` subroutines (a 32-bit compare tail, etc.) — each use is a `jsl`+`rtl` (~14 cycles) around a 4-instruction body. 588 call sites in `m68k.asm` alone. | `m68k.asm`, `mac.asm` |
| 7 | 32-bit shift/flag helpers in wide instructions (`do_shift`: 81 instr + 5 calls; `fl_add`/`fl_sub`: 60 + 6 each). | `m68k.asm` |

The interpreter skeleton itself is lean (`m68k_step` dispatch 24 instr,
`ea_resolve` 19, `fetch16` 9 + calls) — the weight is items 1–5: **the memory
path, and per-step overhead that multiplies it**.

## 2. The plan (ranked: impact × effort)

Tier 1 — small diffs, big multipliers:

- **A. Fetch once.** Hoist the opcode fetch: `mac_step` fetches the word at
  PC once, hands it to the sony-trap check *and* to `m68k_step(op)`. Kills
  cost #1 (a full duplicate fetch). Est. ~1.5–2×.
- **B. VBL down-counter.** Replace `instr_count % 12000` with a 16-bit
  `if (--vbl_timer == 0)` reload. Same firing phase, no division. Est. 1.2–1.5×.
- **C. FastROM.** Map mode `$31`, set `$420D=1` at boot, move the SRAM base
  from `$206000` to the fast mirror `$A06000` (code, Mac ROM `$C8xxxx` and
  disk `$D0xxxx` already sit in fast banks). 8→6 master clocks on nearly
  everything. Est. ~1.25× global, mechanical change.
- **G. Compiler flags.** Build the hot files with `--speed --no-cross-call`
  (and try `--strong-inline`): un-outlines the `?L` helpers (cost #6) at some
  ROM-size cost — we have a 2 MB ROM, size is free. Est. 1.05–1.2×, one
  Makefile line.

Tier 2 — restructure the memory path (cost #3/#4):

- **D. Word-wise RAM ops.** 68000 word accesses are even-aligned and the
  SRAM windows are 8 KB-aligned, so *an aligned word never straddles a
  window*: `m68k_read16`/`write16` can do ONE range decode + ONE window
  computation and read both bytes off the same far pointer (read32 = two of
  those). Halves the window math. Est. 1.3–1.6×.
- **E. Flatten the layers.** Make the SRAM accessors `static inline` in a
  header (or fold `macmem.c` into `mac.c` under `MAC_SNES`) so
  `bus_read` → window math is one function, no `jsl` chain; reorder the
  decode so the RAM hit is the straight-line path. Est. 1.1–1.2×.
- **F. PC window cache.** Keep a resolved far pointer + offset for the 8 KB
  window the PC is in; `fetch16` reads through it directly while execution
  stays in-window (sequential fetches almost always do), refill on
  jump-out/overlay flip. Est. 1.3–1.8× on what remains.
- **H. WRAM-linear Mac RAM** (the endgame of the window tax). Map Mac RAM
  `$2000–$1FFFF` to WRAM `$7E2000–$7FFFFF` — *linear*, one compare + far
  deref, zero window math — and keep Mac `$0000–$1FFF` in a single fixed
  SRAM window (constant base, also zero math). Our own runtime keeps WRAM
  `$7E0000–$1FFF` (DP/stack/data via bank-0 mirrors); the 14 KB tile buffer
  and mailbox move to the *free upper half* of the 256 KB cart SRAM (banks
  `$30+`/fast `$B0+` — only 16 of 32 windows hold Mac RAM today), DMA'd to
  VRAM in two ≤8 KB chunks. `refresh()` then reads the framebuffer as plain
  WRAM. Est. 1.5–2.5× on top of D/E. Bigger diff: linker placement, tile
  buffer move, `mac_init` clear — behind it, RAM is as fast as it gets
  without new silicon.

Tier 3 — spend fewer 68000 instructions (multiplies with everything above):

- **I1. Skip the ROM RAM test** (build-flag). The 128K ROM's startup memory
  test is millions of instructions writing patterns over 128 KB. Detect the
  test's entry PC and hop to its exit with the expected register state — an
  execution-time HLE, the shipped ROM bytes stay untouched. Gated behind
  `MAC_FASTBOOT` since it is a faithfulness trade.
- **I2. HLE `_BlockMove`** (`A02E`) as a C memcpy over the fast memory
  backend — the boot copies the System file and resources through it
  constantly. Gated behind the same flag; end-state-equivalent, not
  step-equivalent.
- **K. MULU/DIVU/shift diet.** SNES hardware 8×8 multiply (`$4202`) and
  16÷8 divide (`$4204`) composed up to the 68000's 16×16/32÷16 (the CPU
  multiplier is free — `--no-ppu-mul` only forbids the *PPU* mode-7
  multiplier); mask tables instead of computed `1<<n`. Do when the bench
  shows heat — boot code is shift/branch heavy, QuickDraw uses more mul.
- **J. Assembly where it still hurts.** After the C plateau, hand-write the
  proven-hot leaves (`fetch16` fast path, RAM word read/write, `set_nz`) in
  65816 `.s` — Calypsi links them cleanly. Est. 1.5–2.5× on what remains.
  Deliberately *last*: every C-level fix above shrinks the asm surface.

Tier 4 — **L. SA-1 (milestone M7).** 10.74 MHz 65816 + 256 KB *linear*
BW-RAM (the whole Mac RAM, no windows, no WRAM juggling) + its own 16×16
multiplier / 16÷16 divider. Est. 4–6× on top of the C plateau. Real
engineering: new linker target, SNES↔SA-1 boot handshake, Mesen supports it.
Everything in tiers 1–3 carries over — the memory model of H maps straight
onto BW-RAM.

Trajectory if the estimates hold (they won't, exactly — that's what the
bench is for): 205/s → tier 1 ≈ 600–900/s → tier 2 ≈ 1,500–3,000/s → tier 3
cuts boot's instruction *count* ~2× → SA-1 ≈ 10,000–15,000/s. A cold boot to
Finder goes from ~4 h toward **~10–20 min at the C plateau and low minutes
on SA-1** — with fast-forward in an emulator, seconds.

## 3. The bench ROM (our own boot ROM)

Optimizing against the real boot is miserable (hours per data point) and the
Apple ROM can't be committed. So the harness boots **our own 64 KB 68000
ROM** — original code, checked into the repo, runs anywhere:

- `bench/asm68k.py` — a minimal 68000 assembler (we implemented the ISA;
  encoding it is the easy direction). Emits `bench/benchrom.bin`: reset
  SP/PC vectors + a phased workload.
- **Phases**, each a fixed dynamic instruction count (unrolled ×16, ~2,000
  instructions per phase): `nop/branch` (fetch+dispatch), `ram.b` / `ram.w` /
  `ram.l` read/write streams, `alu` (register-only ADD/EOR/CMP), `shift`
  (LSL/ROR by 1/8/register), `muldiv` (MULU/DIVU), `copy`
  (`MOVE.L (A0)+,(A1)+` — the BlockMove shape), `fb-fill` (paints the Mac
  framebuffer — *visible progress bar on the SNES screen*).
- Each phase writes its ID to a RAM mailbox word; at the end the ROM writes
  a CRC of registers + touched RAM and `STOP`s.
- **Host golden**: `bench/benchhost` (links the same core) prints the CRC
  and per-phase counts — committed as the reference.
- **SNES timing**: `make test-macbench` runs `macbench.sfc` (bench ROM
  embedded at `$080000`, no Apple bytes anywhere) under Mesen; a Lua script
  watches the phase word and records the frame count per phase →
  **master cycles per 68000 instruction, per workload class**. That table
  lives at the top of this file's changelog and every optimization lands
  with a before/after row.

The bench ROM doubles as the fun deliverable: it boots instantly and draws
to the framebuffer, so `macbench.sfc` is the first thing this project can
ship that *boots on real hardware with zero Apple bytes*.

## 4. Validation protocol

Correctness gates, in increasing cost — every optimization passes all that
apply before it lands:

1. **Host instruction vectors** (`core-c/tests`) — semantics unchanged on
   the host build (which never sees the SNES memory model).
2. **Bench CRC parity** — the bench ROM's final CRC on SNES equals the host
   golden. This is the Apple-free semantic gate; it exercises the *changed*
   code (SNES memory path) unlike gate 1. CI-able.
3. **Real-ROM step parity** — `make test-macparity` (N=50 lockstep hash vs
   host reference) for every change that claims per-instruction equivalence.
   Fresh Mesen (`pkill -9 -f Mesen` first — stacked instances corrupt
   timing and once cost an hour of ghost-chasing).
4. **Boot smoke** — real ROM to Happy Mac on fast-forward at each tier
   boundary; full boot-to-Finder wall-clock logged at milestones only.

Perf gate: the bench frames table, before/after, in the same commit.
Special cases: **A** and **B** are step-equivalent (same fetch value, same
VBL phase — the down-counter seeds at 12,000 so it fires on the same step)
so gate 3 applies. **I1/I2** are *by design* not step-equivalent: they skip
instructions. For those, gates 1–2 plus boot-to-Finder equivalence (same
screen, same VBL ballpark) replace parity, and they stay behind
`MAC_FASTBOOT`.

## 5. Sequencing

1. **P0 — build the harness** (bench ROM + host golden + Lua timer +
   baseline table). Nothing else lands without it.
2. **Tier 1** (A, B, C, G) — one commit each, bench row each. Cheap, huge.
3. **Tier 2** (D, E, then F, then H) — H is the big diff; D/E/F first both
   because they're smaller and because they de-risk H's accessor layout.
4. **Tier 3** (I1, I2 flagged; K if the bench says so).
5. **Tier 4** (J asm leaves, then L = SA-1 as milestone M7).

Estimates above are honest guesses ranked by measured evidence; the bench
exists precisely because the last "obvious" perf idea (`<<3` vs `*0x10000`)
turned out to compile to the identical runtime call. Measure, land, repeat.

## 6. Results

`make -C snes test-bench` (fresh Mesen). Total = frames from phase 1 to done
(13,985 instructions); columns are frames per phase (fewer = faster). The
CRC must print `A97233CE` on both host and SNES, every row.

"Total" = frames for phases 1–10 (13,985 instructions; the `trap` phase, added
with change A, is listed separately since earlier rows predate it).

| change | total | nop | alu | ram.b | ram.w | ram.l | shift | muldiv | copy | fbfill | trap | ≈instr/s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline (`63e1e1c`) | 2234 | 136 | 163 | 163 | 175 | 192 | 176 | 110 | 125 | 952 | — | 376 |
| G `--no-cross-call` | 2233 | 136 | 163 | 163 | 175 | 192 | 176 | 110 | 124 | 952 | — | 376 |
| B VBL down-counter | 1503 | 79 | 106 | 107 | 117 | 135 | 119 | 82 | 94 | 630 | — | 558 |
| A fetch-once + hook | 1309 | 64 | 91 | 91 | 102 | 120 | 104 | 74 | 86 | 544 | 47 | 641 |
| C FastROM ($31+MEMSEL) | 1139 | 55 | 79 | 79 | 89 | 105 | 90 | 64 | 76 | 474 | 42 | 737 |
| D+E word ops, inline | 975 | 50 | 75 | 75 | 76 | 86 | 86 | 60 | 62 | 378 | 36 | 861 |
| F PC-window fetch cache | 967 | 50 | 74 | 74 | 75 | 86 | 84 | 60 | 62 | 375 | 35 | 868 |
| H two-region RAM | ~985 | 54 | 78 | 76 | 78 | 86 | 89 | 63 | 65 | 359 | 38 | 852 |

**Cumulative: 2.3× (376 → ~860 instr/s).** Estimates vs reality, in public: B
was supposed to be the small one and delivered 1.49×; F was supposed to be
1.3–1.8× and delivered 1%; H was "the endgame" and *lost* 2% — because after
C, cart SRAM is a 6-master-clock memory while WRAM is 8, so the win H was
designed for had already been taken. H stays anyway: it cuts the cart-SRAM
requirement from 256 KB (no real cartridge) to **26 KB** (fits a real 32 KB
cart), which is what makes hardware snesmac plausible — and it is the memory
layout SA-1 BW-RAM maps onto.

## 7. Fastboot (items I1/I2) — the boot-time cut

`make macboot` now builds with `FASTBOOT=1` by default (`FASTBOOT=0` for the
purist boot):

- **I1 — skip the RAM test.** The 128K ROM burns ~1.5M instructions before
  its first disk read: the power-on RAM test (which does *not* zero RAM — the
  patterns stay), hardware init, and the boot-chime delay loops (we don't
  emulate sound). `macfastboot_gen` boots the user's ROM on the host up to
  the instruction about to issue the first .Sony read and dumps the complete
  machine state (registers, VIA + VBL phase, full 128 KB RAM image). The live
  boot teleports there when it first reaches the test's entry PC. Validated
  by trajectory comparison: framebuffer hash and VBL count match the cold
  boot exactly at 1.6M/2.5M/5M/15M-step checkpoints, and the machine lands on
  the identical Finder desktop (the PC micro-drifts a few instructions from
  uncaptured floppy-motor phase — invisible in behavior).
- **I2 — `_BlockMove` at C speed.** The A-line hook services A?2E as a host
  memmove over the fast accessors (~2 orders of magnitude faster than ~3
  emulated instructions per byte), D0=noErr, Z set. The System loads through
  BlockMove constantly, so the same desktop-equality run validates it.
- Both artifacts are generated at build time from the user's own ROM + disk
  (like `macparity_ref.h`) and are never committed; the RAM image rides the
  cart at `$D80000`.

On the SNES the teleport fires ~11k instructions in: the mailbox trace jumps
from `vbl=00000000` to `vbl=0000007D` and the boot proceeds straight into
disk I/O. At ~860 instr/s the pre-Happy-Mac dead time drops from ~34 minutes
of emulated grind to seconds.

**Measured-but-deferred:** hardware mul/div (K) — the bench's muldiv phase
sits only ~25% over the floor, and boot code barely multiplies; revisit if a
future workload says otherwise.

## 8. The profiler round (harness v2)

`test/macprof.lua` samples the 65816 PC twice per frame; symbolicate against
the linker map (attributing Calypsi's anonymous `?L` helper blocks to the
preceding symbol). First profile: **~54% interpreter, ~20% `refresh()`,
~17% `mac_init`** — the video copy and the one-time RAM clear, not the
emulator, were the top optimizable costs. Ablation confirmed it: removing
the per-phase refresh cut the bench 985 → 737 frames, i.e. **~25 frames
(0.4 s) per redraw** — a cost that had been landing inside the phase windows
and inflating every row of the table above (ratios stayed honest; absolutes
were ~25% pessimistic).

Fix: `calypsi/tilecopy.s`, hand 65816 — `tile_rows` (the fb→tile rebuild,
fully-unrolled strided copies, **~2 frames per redraw incl. DMA, 13×**),
`mac_ram_clear_all` (seed + overlapping-MVN fill, ~2 frames, was ~260),
`mac_fastboot_load` (three MVNs, ~3 frames, was ~130). Every pan step,
mouse move and boot redraw rides `tile_rows` now.

Bench v2 keeps the (now ~2-frame) per-phase refresh; pure-machine numbers
with refresh ablated:

| | total | nop | alu | ram.b | ram.w | ram.l | shift | muldiv | copy | fbfill | ≈instr/s |
|---|---|---|---|---|---|---|---|---|---|---|---|
| v2 machine-only | 737 | 30 | 54 | 52 | 53 | 63 | 64 | 40 | 39 | 336 | **1139** |

So the true state pre-SA-1: **376 → ~1,140 instr/s (3.0×)** plus the boot's
1.5M-instruction teleport. Next levers, in measured-priority order: inline
the RAM/ROM fast paths into m68k.c itself (the cross-module `jsl` per memory
access and the anonymous helper time around it ≈ 20%+ of the remainder),
`mac_run(n)` step batching + input-poll cadence, decode-chain reordering
(`op_group4` walks 6 masked compares before NOP), a parity-gated
`--strong-inline` experiment — and then item J proper (hand 65816 for
fetch/dispatch), the 2–3× that remains before SA-1 (M7).

Reading the baseline: a bare NOP costs ~44,600 master clocks — the floor
every instruction pays (two full fetch16s: the real one plus the sony-trap
guard's, the per-step modulo, dispatch). The spread up to ram.l/copy
(~63–77k) is the byte-wise window tax; muldiv's 72k is the runtime
mul/div helpers on top of the floor.

**Item G is dead, killed by its own measurement.** `--no-cross-call` un-outlines
all 588 jsl/rtl helper sites and buys *zero frames* — the factoring was never
on the critical path. Adding `--speed` "won" 7% but MISCOMPILES the machine:
the real-ROM boot derails deterministically around step 768 (the bench alone
passed — its instruction mix never hits the broken path; the parity gate
caught it). Flags reverted to plain `-O2`. This is the harness doing its job:
one static-analysis guess disproven, one compiler landmine flagged, before
either touched a real commit.

**Harness note — the Mesen guillotine.** Mesen's `--testrunner` hard-kills
runs at ~100 s wall clock (exit 255, no message) and runs throttled to
real-time 60 fps, so every gate must fit the budget *on a loaded machine* —
this, not "environment degradation", is what truncated past parity runs. The
parity gate was dieted to fit: the per-step hash mixer is now rot1-xor-add in
16-bit halves (FNV's 32-bit multiply is a Calypsi runtime call; 20 folds per
step made the gate ~4x the cost of the machine it checks), the RAM sweep
samples every 16th byte, and `MACP_N` defaults to 500. A full gate run is now
~20 s.
