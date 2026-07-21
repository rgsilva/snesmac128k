# Where snesmac goes next

It boots a Macintosh to the Finder desktop on a Super Nintendo. Two things stand
between that and something you'd actually want to sit in front of: it's **slow**, and
you **can't touch it** yet. Roughly in priority order:

## Make it watchable (performance)

The interpreted 68000 runs at ~200 instructions/sec, so a boot is hours (see
[COMPATIBILITY.md](COMPATIBILITY.md)). The levers, smallest to biggest:

- **Rewrite the hot path in assembly.** The fetch/decode/dispatch inner loop and the
  memory accessors are the cost centre; Calypsi's `-O2` C is fine but not tight. Hand
  65816 (threaded dispatch, the register file in the direct page — the snes86 playbook)
  should be worth ~2–4×.
- **Cut the memory-window tax.** The Mac's RAM lives in 8 KB SRAM windows, so every
  access does bank math. Put the *hot* RAM — the stack, the low globals, the
  framebuffer — in flat WRAM instead, precompute per-window base pointers, or use a
  faster window map. (Each access is only ~13 instructions today, but there are ~10 per
  emulated instruction, so it adds up.)
- **Precompute the slow arithmetic.** The 68000's `MUL`/`DIV` and some address math run
  as software routines on the 16-bit 65816; lookup tables and strength-reduction can
  help.
- **SA-1.** The biggest single lever, and the reason a Mac-on-SNES can be watchable at
  all: the SA-1 is a 65816 *in the cartridge* at 10.7 MHz (3× the console CPU) with flat,
  linear RAM — which also erases the window tax. On its own that's ~5×; stacked with the
  assembly and memory work, "tens of minutes" is realistic. This is milestone **M7**,
  alongside a bring-your-own-ROM public release.

## Make it usable (input)

The Finder is drawn but inert. This is milestone **M6** — wiring the Mac's input
hardware (the VIA mouse lines and the keyboard) to the SNES controller:

- **Mouse** — the SNES D-pad (or a real SNES Mouse) drives the Mac cursor; a button
  clicks.
- **Virtual keyboard** — an on-screen keyboard, snes86-style: **SELECT** opens/closes
  it, **A** confirms the highlighted key, keystrokes feed the Mac keyboard.

## Make it fit the screen (display)

The SNES shows a 256×224 window of the 512×342 Mac screen. The SNES can't show the whole
1-bit screen at once (a background addresses 1024 tiles; the full screen needs 2752), so:

- **Pan — done.** The **L/R** shoulder buttons and the **d-pad** slide the window across
  the Mac screen (8px horizontal steps, 4px vertical); the framebuffer read is done once
  per row so it stays responsive on the slow CPU. Still to add: smooth sub-tile scroll
  via the BG scroll registers.
- **Fit-to-screen toggle** — **X** or **START** to downscale the full 512×342 into
  ~256×171 (OR-ing 2×2 pixel blocks so thin 1px menu lines survive) so you can see
  everything at once, and back to 1:1 pan for detail.

## Reach further

- **More System software.** System 2.0 hasn't been driven to the desktop yet; larger
  (800 KB) disks and a second drive would follow.
- **Writable disk.** Persist disk writes to cart SRAM (a second, writable image).
- **Real hardware.** Everything so far is tested in Mesen. A flashcart boot on a real
  SNES is the milestone that makes the whole thing real — if you try it, please report
  back.
- **Full-resolution video.** A hi-res (Mode 5/6) or multi-background scheme that shows
  more of the 512-wide screen without panning.

## The milestone map

`M0` build plumbing · `M1` 68000 core validated · `M2` core runs on the SNES (parity) ·
`M3` the Mac boots to the Happy Mac (host) · `M4` the Happy Mac on real SNES output ·
**`M5` disk boot to the Finder desktop** · **`M4b` the live machine on the SNES** ·
`M6` input (mouse + keyboard) · `M7` SA-1 + performance + release.

The full milestone log and the original feasibility brief are in
[PROJECT.md](PROJECT.md).
