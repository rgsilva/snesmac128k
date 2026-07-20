# snesmac128k

A **Macintosh 128K** (Motorola 68000, 128 KB RAM, 512×342 1-bit screen) emulator
for a stock **Super Nintendo**. Third machine in the family after
[snes86](https://github.com/rgsilva) (an 8086 PC booting MS-DOS) and snespico8
(a PICO-8 runtime). Slow and "kinda broken" by design — the goal is to boot a
Mac on a SNES *at all*.

The enabling bet: the 68000 is written **once in portable C and compiled to the
65816 with Calypsi**, not hand-written in assembly. See
**[docs/PROJECT.md](docs/PROJECT.md)** for the full feasibility analysis,
architecture, and M0–M7 milestone plan.

## Status — M1 (68000 core validation) done

The core is [Musashi](https://github.com/kstenerud/Musashi) (MIT), validated
against the [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000)
per-instruction vectors — the same approach snes86 uses for its 8088 core:

- **127 opcode files, 317,500 tests: 81.5% byte-exact, 100.0% accounted for, 0
  unexplained failures** (58 opcodes pass 100% exact).
- Normal instruction execution and all normal exceptions match **exactly**. The
  non-exact remainder is entirely two benign, documented classes: odd-address
  faults (the 68000 address-error frame, which Musashi only approximates vs the
  MAME microcode core that generated the vectors) and undefined-result ops
  (invalid-BCD ABCD/SBCD/NBCD, DIV overflow, CHK, LINK A7, and the vector
  authors' own known-bad TAS/TRAPV). None affect well-behaved Mac code.

## Layout

- `core-c/musashi/` — vendored 68000 core (MIT).
- `core-c/tests/` — host validation harness: `m68ktest.c` → `libem68k.dylib`,
  driven by `run_vectors.py` over the m68000 vectors.
- `snes/` — Calypsi C → 65816 SNES build (mailbox + Mesen harness, cloned from
  snespico8); `hello.c` is the M0 plumbing spike.
- `docs/PROJECT.md` — the plan.

## Building & validating

```sh
cd core-c/tests
make            # build the dylib (regenerates Musashi's opcode tables via m68kmake)
make vectors    # fetch the ~113 MB vector set once
make test       # run the full validation
```

## Legal

The Mac 128K ROM and System/Finder disk are Apple copyright and are **not**
distributed here — bring your own dumps. The emulator ships freely.
