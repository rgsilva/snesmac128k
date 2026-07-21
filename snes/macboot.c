/* M4b: the live Mac 128K, booting on the SNES. Runs the machine model (mac.c +
** m68k.c, RAM in cart SRAM, ROM+disk in cart ROM via macmem.c) and rebuilds a
** 256x224 window of the Mac framebuffer ($1A700, in SRAM) into a WRAM tile
** buffer, DMA'd to VRAM — so the SNES shows the boot live (Happy Mac, "Welcome
** to Macintosh", then the Finder desktop). Same Mode-0 2bpp path as macvideo.c.
**
** The Mac screen is 512x342 but the SNES can only show a 256x224 window at 1:1
** (a background addresses 1024 tiles; the full screen is 2752), so the window
** PANS: the L/R shoulder buttons and the d-pad slide it across the Mac screen.
** Panning is driven by the auto-read joypad and redraws on demand, independent
** of the (slow) emulated CPU, so it stays responsive even mid-boot.
**
** It is faithful but SLOW (a full boot is the documented tens-of-minutes-to-
** hours; SA-1 is the eventual fix) — a "leave it running, then pan and
** screenshot" demo. */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "mac.h"

#define REG8(a)  (*(volatile uint8_t  *)(unsigned long)(a))
#define REG16(a) (*(volatile uint16_t *)(unsigned long)(a))

#define DISK_SIZE     409600ul                        /* 400K floppy at $D00000 */
#define TILEBUF       ((volatile __far uint8_t *)0x7E8000ul)  /* 32x28 tiles, 14 KB */
#define TW 32
#define TH 28
#define TILEBYTES     (TW * TH * 16)                  /* 14336 */
#define PANY_STEP     4                               /* vertical pan, pixels/poll */
#define BOOT_REFRESH  4096u                           /* idle redraw cadence (boot progress) */

/* framebuffer byte -> cart-SRAM address (8 KB windows at banks $20-$2F:6000),
** same mapping as macmem.c but used here once per fb row, not per byte. */
#define FB_SRAM(a)    (0x206000ul + ((a) >> 13) * 0x10000ul + ((a) & 0x1FFFul))

/* The visible window into the Mac screen. panXb is the left edge in framebuffer
** BYTES (x8 = pixels), 0..32; panY is the top edge in pixel rows, 0..118 — the
** slack over the 512x342 screen. Default: top-centre, so the menu bar shows. */
static int panXb = 16, panY = 0;

/* DMA the WRAM tile buffer to VRAM word $1000 (A-bus WRAM -> B-bus $2118/$2119). */
static void blit_tiles(void){
  REG8(0x2115) = 0x80;                  /* VMAIN: word, +1 after $2119 */
  REG16(0x2116) = 0x1000;               /* VRAM word address */
  REG8(0x4300) = 0x01;                  /* A->B, 2 regs */
  REG8(0x4301) = 0x18;                  /* B-bus $2118 */
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x80; REG8(0x4304) = 0x7E;  /* src $7E8000 */
  REG16(0x4305) = (uint16_t)TILEBYTES;
  REG8(0x420B) = 0x01;
}

/* Rebuild the tile buffer from the panned window and DMA it. Tile (tx,ty) row r
** -> plane0 = the framebuffer byte at (panXb+tx, panY+ty*8+r); plane1 is left 0
** (set once in video_init). The force-blank wraps only the DMA, so the screen
** blinks for a few ms, not for the whole (slow) tile rebuild. */
static void refresh(void){
  uint16_t tx, ty; uint8_t r;
  /* The framebuffer's 64-byte rows align to the 8 KB SRAM windows (the fb starts
  ** 28 rows into a window and 8192/64 = 128 rows fill one), so a row never
  ** straddles a window. So compute the far pointer ONCE per fb row (the costly
  ** 32-bit window math) and read the 32 window bytes with plain 16-bit indexing
  ** — ~7000 per-byte address computations become ~224 per-row ones. */
  for (ty = 0; ty < TH; ty++) {
    uint16_t trow = (uint16_t)(ty * TW);
    for (r = 0; r < 8; r++) {
      uint16_t fbrow = (uint16_t)(panY + ty * 8 + r);
      uint32_t addr = 0x1A700ul + (uint32_t)fbrow * 64u + (uint32_t)panXb;
      const volatile __far uint8_t *sp = (const volatile __far uint8_t *)FB_SRAM(addr);
      for (tx = 0; tx < TW; tx++)
        TILEBUF[(uint16_t)((trow + tx) * 16 + r * 2)] = sp[tx];
    }
  }
  REG8(0x2100) = 0x8F;                  /* blank just for the VRAM DMA */
  blit_tiles();
  REG8(0x2100) = 0x0F;                  /* screen on */
}

/* Poll the auto-read joypad and pan. L/R shoulders or d-pad left/right move the
** window horizontally (8px steps); d-pad up/down move it vertically. Returns 1
** if the window moved. */
static int read_pad(void){
  uint16_t p = REG16(0x4218);          /* JOY1: B Y Sel St U D L R | A X L R .... */
  int ox = panXb, oy = panY;
  if (p & (0x0200u | 0x0020u)) panXb--;                   /* d-pad Left  / L shoulder */
  if (p & (0x0100u | 0x0010u)) panXb++;                   /* d-pad Right / R shoulder */
  if (p & 0x0800u) panY -= PANY_STEP;                     /* d-pad Up */
  if (p & 0x0400u) panY += PANY_STEP;                     /* d-pad Down */
  if (panXb < 0) panXb = 0; else if (panXb > 32)  panXb = 32;
  if (panY  < 0) panY  = 0; else if (panY  > 118) panY  = 118;
  return (panXb != ox) || (panY != oy);  /* moved only if the clamp actually changed it */
}

static void video_init(void){
  uint16_t i, n = TW * TH;
  REG8(0x2100) = 0x8F;                  /* force blank */
  REG8(0x2105) = 0x00;                  /* BGMODE 0 */
  REG8(0x2107) = 0x00;                  /* BG1SC tilemap word $0000, 32x32 */
  REG8(0x210B) = 0x01;                  /* BG1 char base word $1000 */
  REG8(0x2121) = 0;                     /* CGRAM colour 0 = white, 1 = black */
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x0000;          /* identity tilemap */
  for (i = 0; i < 1024; i++) REG16(0x2118) = (i < n) ? i : 0;
  REG8(0x210D) = 0; REG8(0x210D) = 0;
  REG8(0x210E) = 0; REG8(0x210E) = 0;
  REG8(0x212C) = 0x01;                  /* main screen: BG1 */
  for (i = 0; i < TILEBYTES; i++) TILEBUF[i] = 0;       /* plane1 stays 0 forever */
}

int main(void){
  uint32_t idle = 0;
  mb_init();
  mb_puts("macboot: live Mac 128K on the SNES -- L/R + d-pad to pan\n");
  REG8(0x4200) = 0x01;                  /* enable auto joypad read (no NMI) */
  video_init();
  mac_init(0);                          /* ROM in cart, RAM (SRAM) cleared */
  mac_set_disk(0, DISK_SIZE);           /* enable the HLE .Sony (disk at $D00000) */
  refresh();                            /* first draw, screen on */
  for (;;) {
    mac_step();
    if (read_pad()) {                   /* view moved: redraw now */
      refresh();
      idle = 0;
      mb_puts("pan xb="); mb_puthex32((uint32_t)panXb);
      mb_puts(" y="); mb_puthex32((uint32_t)panY); mb_putc('\n');
    } else if (++idle >= BOOT_REFRESH) { /* idle: redraw for boot progress */
      idle = 0;
      refresh();
      mb_puts("pan xb="); mb_puthex32((uint32_t)panXb);
      mb_puts(" y="); mb_puthex32((uint32_t)panY);
      mb_puts(" vbl="); mb_puthex32(mac_vbl_count()); mb_putc('\n');
    }
  }
  return 0;
}
