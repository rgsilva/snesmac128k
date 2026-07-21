# What runs, and how fast

## What boots

**System 1.1 boots to the Finder desktop.** On the host (`runmac`) this is verified end
to end — the 🍎 File/Edit/View/Special menu bar, the "System Disk" icon, and the Trash.
On the SNES the *same* machine boots live (confirmed in Mesen): Happy Mac → "Welcome to
Macintosh" → the desktop, given enough time.

<p align="center">
  <img src="assets/macboot_001.png" width="360" alt="'Welcome to Macintosh' on the SNES"><br>
  <sub>"Welcome to Macintosh", live on the SNES — the ROM's <em>dsGreeting</em>, on the way to the desktop.</sub>
</p>

The boot you'll see:

| stage | what it means |
|---|---|
| **Happy Mac** | power-on self-test passed; the ROM found the disk and is booting it |
| **"Welcome to Macintosh"** | the System is loading (this is `SysError(40)` = `dsGreeting`, normal boot, not a crash) |
| **Finder desktop** | you made it |

The whole boot runs the *real* Apple ROM code — the File Manager, the Resource Manager,
QuickDraw, the Finder — on top of the emulated 68000. snesmac only fakes the disk
*hardware* (via the [HLE `.Sony` driver](INTERNALS.md#the-hle-sony-driver--how-it-reads-a-disk)),
not the software.

## What doesn't (yet)

- **Mouse works; no keyboard yet.** The **d-pad** moves the cursor and **A** clicks — you
  can pull down menus and click around the Finder. (The cursor lags, because the Mac only
  redraws it on its VBL and the CPU is slow — fast-forward helps.) The **keyboard** isn't
  wired up yet, so no typing; that's next (see [FUTURE.md](FUTURE.md)).
- **One disk, read-only.** A single 400 KB System floppy is served from the cartridge.
  Writes are acknowledged but not persisted; there's no second drive.
- **The display is a 256×224 window** into the 512×342 Mac screen (the SNES can't show
  the whole 1-bit screen at once — a background addresses only 1024 tiles; the full
  screen is 2752). The window **pans** — **hold L** and the **d-pad** slides it, so you
  can reach the menu bar, the disk icon, and the Trash to frame a shot (the d-pad on its
  own is reserved for the mouse). A "fit the whole screen at once" (downscaled) mode is
  still planned.
- **System 2.0 is untested.** Only System 1.1 has been driven all the way to the desktop.
  Later Systems (and larger/800 KB disks) haven't been tried.

## The performance picture (the honest part)

snesmac is **slow**. Measured in Mesen, the interpreted 68000 runs at roughly **200
instructions per second**, so the ~3 million instructions it takes to reach the desktop
is **tens of minutes to hours** of fast-forward. That's ~3,800× slower than a real
Macintosh.

It's not one bug — it's the shape of the problem:

- **You're running a CPU interpreter, so one Mac instruction is a whole subroutine.**
  Fetch the opcode, decode the addressing mode, do the op, write it back, update the
  flags — thousands of 65816 cycles for what a real 68000 does in ~10.
- **The CPUs are mismatched both ways.** The 68000 is 32-bit at 7.83 MHz; the 65816 is
  16-bit at 3.58 MHz. Every 32-bit operation the Mac does becomes several 16-bit ones,
  on a slower clock, with no usable hardware multiply/divide.
- **The boot genuinely does a lot** — it checksums all 64 KB of ROM, pattern-tests all
  128 KB of RAM, then mounts the disk and loads the System and Finder.

This is a "watch a Macintosh come alive on a Super Nintendo" demo, not a usable computer.
Making it *watchable* (tens of minutes, or minutes) is the point of the perf work in
[FUTURE.md](FUTURE.md) — chiefly the SA-1 coprocessor (3× clock + linear RAM) and
hand-written assembly for the interpreter's hot path.

## Getting a disk

Bootable 400 KB System disks in DiskCopy 4.2 format are archived at sites like
[earlymacintosh.org](https://earlymacintosh.org/disk_images.html). Convert one to raw
sectors with `core-c/tests/dc42.py` (see [BUILDING.md](BUILDING.md)). Disk images are
Apple-licensed — bring your own; they're gitignored.
