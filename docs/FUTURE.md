# Where snesmac goes next

It boots a Macintosh to the Finder desktop on a Super Nintendo. Two things stand
between that and something you'd actually want to sit in front of: it's **slow**, and
you **can't touch it** yet. Roughly in priority order:

## Make it watchable (performance)

**The plan of record is [PERF.md](PERF.md)** — a measured cost breakdown, a
ranked fix list, and a committable Apple-free 68000 bench ROM that gives every
change a hard cycles-per-instruction number. Most of that ranked list is now
**done**; what's left is the arithmetic tail and the big coprocessor lever:

- **Hot path in assembly — done.** The fetch/decode/dispatch inner loop is hand
  65816 (`snes/calypsi/m68krun.s`) with the register file in the direct page, and
  it grew from a NOP/MOVEQ/branch skeleton to native handlers for the boot's
  whole hot instruction mix: MOVE.B/.W/.L, the register ALU (ADD/SUB/CMP/AND/OR),
  ADDQ/SUBQ, all 14 Bcc plus BSR/JSR/RTS, LSL/LSR/ROL/ROR, CMPI, and BTST (the
  single biggest boot op — ~78k of the first 200k instructions). Anything
  unhandled punts to the C decoder; the bench phases the handlers target run
  **2–4.7× faster** (`nop` and `shift` are within a hair of all-native).
- **Memory-window tax — done.** RAM is two flat regions (WRAM-linear above
  `$2000`, the fast bank-`$20` SRAM mirror below), so there's no per-access window
  math left, and the stack/MOVE/BTST handlers reuse that one fast path.
- **Precompute the slow arithmetic — open.** `MUL`/`DIV` still run as C software
  routines on the 16-bit 65816 (the `muldiv` bench phase is the last
  un-accelerated one); the SNES's hardware multiplier could be wired in.
- **The instruction tail — open.** mem→mem / ROM-source MOVE (the OS's ROM→RAM
  startup copies, the `copy` phase), the immediate ALU (ORI/ANDI/SUBI/ADDI/EORI),
  BCHG/BCLR/BSET, and JSR with a fetched EA still punt — each rarer in the boot.
- **SA-1.** The biggest single remaining lever: a 65816 *in the cartridge* at
  10.7 MHz (3× the console CPU) with flat, linear RAM. On its own ~5×, stacked on
  the assembly work already in place. This is milestone **M7**, alongside a
  bring-your-own-ROM public release.

## Make it usable (input) — milestone **M6**

- **Mouse — done.** The **d-pad** moves the cursor and **A** clicks: the position is
  written straight into the Mac's low-memory mouse globals and the button into VIA PB3,
  so menus pull down and the Finder responds. Still to do: smooth it out (the cursor
  redraws only on the Mac's VBL, so it lags on the slow CPU), and maybe support a real
  SNES Mouse peripheral.
- **On-screen keyboard — done.** **SELECT** shows/hides a QWERTY keyboard across
  the bottom five rows (BG2 overlay); the **d-pad** moves the highlight, **A**
  presses, and a **SHIFT** key gives the shifted characters. Keys post real
  keyDown events into the ROM's Event Manager (an HLE that piggybacks
  `_PostEvent` when the guest polls for events), so menus, the Finder's rename,
  and desk accessories all receive them.

## Make it fit the screen (display)

The SNES shows a 256×224 window of the 512×342 Mac screen. The SNES can't show the whole
1-bit screen at once (a background addresses 1024 tiles; the full screen needs 2752), so:

- **Pan — done.** Hold **L** and the **d-pad** slides the window across the Mac screen
  (8px horizontal, 4px vertical); the d-pad alone is left free for the mouse. The
  framebuffer read is done once per row so it stays responsive on the slow CPU. Still to
  add: smooth sub-tile scroll via the BG scroll registers.
- **Fit-to-screen toggle — done.** Tap **R** to see the whole 512×342 at once,
  2×2-downscaled to 256×171 — with **four gray levels** rather than the planned
  OR-blocks: the normal view is 1-bit, so the second plane of the SNES's 2bpp mode
  and two spare palette entries were free, and the count-of-black-pixels average
  renders the dithered desktop as true gray while 1px window borders stay visible
  as dark gray. Tap R again for 1:1 pan.

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
