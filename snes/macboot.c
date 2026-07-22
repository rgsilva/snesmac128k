/* M4b: the live Mac 128K, booting on the SNES. Runs the machine model (mac.c +
** m68k.c, RAM in cart SRAM, ROM+disk in cart ROM via macmem.c) and rebuilds a
** 256x224 window of the Mac framebuffer ($1A700, in SRAM) into a WRAM tile
** buffer, DMA'd to VRAM — so the SNES shows the boot live (Happy Mac, "Welcome
** to Macintosh", then the Finder desktop). Same Mode-0 2bpp path as macvideo.c.
**
** Controls (auto-read joypad): the d-pad moves the MOUSE and A clicks; hold L
** and the d-pad instead PANS the 256x224 window across the 512x342 Mac screen
** (the SNES can't show it all at once — a background addresses 1024 tiles, the
** whole screen is 2752). The mouse is HLE'd straight into the Mac's low-memory
** globals (mac_set_mouse); the Mac redraws its own cursor on its VBL, so the
** cursor lags on the slow CPU (fast-forward helps).
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
#define TW 32
#define TH 28
#define TILEBYTES     (TW * TH * 16)                  /* 14336 */
#define PANY_STEP     4                               /* vertical pan, pixels/poll */
#define BOOT_REFRESH  4096u                           /* idle redraw cadence (boot progress) */

/* The tile buffer lives in cart SRAM windows 16+17 (banks $B0/$B1:6000, fast
** mirrors, above the Mac's 16 windows) — WRAM $7E2000+ belongs to the Mac now
** (PERF.md item H). Rows 0-15 (tiles 0-511 = 8192 bytes) fill window 16
** exactly; rows 16-27 (6144 bytes) go in window 17 — a tile row never
** straddles. The framebuffer itself is read straight out of WRAM, linear. */
#define TB0           ((volatile __far uint8_t *)0xB06000ul)
#define TB1           ((volatile __far uint8_t *)0xB16000ul)

/* the tile rebuild is hand 65816 (calypsi/tilecopy.s) — the C loop compiled
** to ~45 instructions/byte and cost 0.4 s per redraw; pan is passed via
** these globals (no Calypsi calling convention in asm) */
extern void tile_rows(void);
extern uint16_t tile_pan_xb, tile_pan_y;

/* The visible window into the Mac screen. panXb is the left edge in framebuffer
** BYTES (x8 = pixels), 0..32; panY is the top edge in pixel rows, 0..118 — the
** slack over the 512x342 screen. Default: top-centre, so the menu bar shows. */
static int panXb = 16, panY = 0;

/* Mouse (the d-pad with L up): mx,my is the cursor in Mac screen coords
** (0..511 / 0..341); A clicks. Fed to the Mac via mac_set_mouse; the Mac then
** redraws its own cursor on its VBL, so it lags on the slow CPU. */
#define MOUSE_STEP 4
#define CLICK_HOLD 16000              /* hold a tap of A down > one VBL (12000) so it's seen */
static int mx = 256, my = 171, last_btn = 0;
static uint32_t click_hold = 0;

/* DMA the two SRAM tile-buffer windows to VRAM word $1000 (A-bus -> $2118/9). */
static void blit_tiles(void){
  REG8(0x2115) = 0x80;                  /* VMAIN: word, +1 after $2119 */
  REG16(0x2116) = 0x1000;               /* VRAM word address */
  REG8(0x4300) = 0x01;                  /* A->B, 2 regs */
  REG8(0x4301) = 0x18;                  /* B-bus $2118 */
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x60; REG8(0x4304) = 0xB0;  /* src $B06000 */
  REG16(0x4305) = 8192u;
  REG8(0x420B) = 0x01;
  REG16(0x2116) = 0x2000;               /* continue at word $1000 + 4096 */
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x60; REG8(0x4304) = 0xB1;  /* src $B16000 */
  REG16(0x4305) = (uint16_t)(TILEBYTES - 8192u);
  REG8(0x420B) = 0x01;
}

/* Rebuild the tile buffer from the panned window and DMA it. Tile (tx,ty) row r
** -> plane0 = the framebuffer byte at (panXb+tx, panY+ty*8+r); plane1 is left 0
** (set once in video_init). The force-blank wraps only the DMA, so the screen
** blinks for a few ms, not for the whole (slow) tile rebuild. */
static void refresh(void){
  tile_pan_xb = (uint16_t)panXb;
  tile_pan_y  = (uint16_t)panY;
  tile_rows();                          /* asm: fb -> tile buffer, ~1 ms */
  REG8(0x2100) = 0x8F;                  /* blank just for the VRAM DMA */
  blit_tiles();
  REG8(0x2100) = 0x0F;                  /* screen on */
}

/* Poll the auto-read joypad. Two modes: hold L and the d-pad PANS the window
** (8px horiz / 4px vert); with L up the d-pad moves the MOUSE and A clicks.
** Returns 1 if the display should be redrawn now (view moved, or the cursor /
** button changed). */
static int handle_input(void){
  uint16_t p = REG16(0x4218);          /* JOY1: B Y Sel St U D L R | A X L R .... */
  if (p & 0x0020u) {                   /* L held -> PAN */
    int ox = panXb, oy = panY;
    if (p & 0x0200u) panXb--;          /* Left */
    if (p & 0x0100u) panXb++;          /* Right */
    if (p & 0x0800u) panY -= PANY_STEP; /* Up */
    if (p & 0x0400u) panY += PANY_STEP; /* Down */
    if (panXb < 0) panXb = 0; else if (panXb > 32)  panXb = 32;
    if (panY  < 0) panY  = 0; else if (panY  > 118) panY  = 118;
    return (panXb != ox) || (panY != oy);
  } else {                             /* MOUSE */
    int omx = mx, omy = my, a = (p & 0x0080u) ? 1 : 0, btn;
    if (a) click_hold = CLICK_HOLD;    /* A held tops it up; a tap decays over CLICK_HOLD */
    btn = (a || click_hold) ? 1 : 0;   /* so the button stays down long enough to span a VBL */
    if (p & 0x0200u) mx -= MOUSE_STEP; /* Left */
    if (p & 0x0100u) mx += MOUSE_STEP; /* Right */
    if (p & 0x0800u) my -= MOUSE_STEP; /* Up */
    if (p & 0x0400u) my += MOUSE_STEP; /* Down */
    if (mx < 0) mx = 0; else if (mx > 511) mx = 511;
    if (my < 0) my = 0; else if (my > 341) my = 341;
    if (click_hold) click_hold--;
    if (mx != omx || my != omy || btn != last_btn) {
      mac_set_mouse(mx, my, btn);      /* position + button into the Mac */
      last_btn = btn;
      return 1;
    }
    return 0;
  }
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
  for (i = 0; i < 8192u; i++) TB0[i] = 0;               /* plane1 stays 0 forever */
  for (i = 0; i < TILEBYTES - 8192u; i++) TB1[i] = 0;
}

int main(void){
  uint32_t idle = 0;
  mb_init();
  mb_puts("macboot: live Mac 128K on the SNES -- d-pad: mouse, A: click, L+d-pad: pan\n");
  REG8(0x420D) = 0x01;                  /* MEMSEL: FastROM (map mode $31) */
  REG8(0x4200) = 0x01;                  /* enable auto joypad read (no NMI) */
  video_init();
  mac_init(0);                          /* ROM in cart, RAM cleared */
  mac_set_disk(0, DISK_SIZE);           /* enable the HLE .Sony (disk at $D00000) */
#ifdef MAC_FASTBOOT
  mac_fastboot_arm(0);                  /* skip the RAM test + boot chime: teleport
                                        ** to the first disk read (state embedded
                                        ** at $D80000; see docs/PERF.md I1) */
#endif
  refresh();                            /* first draw, screen on */
  for (;;) {
    mac_step();
    if (handle_input()) {               /* view/cursor changed: redraw now */
      refresh();
      idle = 0;
    } else if (++idle >= BOOT_REFRESH) { /* idle: redraw for boot progress */
      idle = 0;
      refresh();
      mb_puts("t vbl="); mb_puthex32(mac_vbl_count());
      mb_puts(" pan="); mb_puthex32((uint32_t)panXb);
      mb_puts(" mouse="); mb_puthex32((uint32_t)mx);
      mb_putc(','); mb_puthex32((uint32_t)my); mb_putc('\n');
    }
  }
  return 0;
}
