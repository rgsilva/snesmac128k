/* M4b: the live Mac 128K, booting on the SNES. Runs the machine model (mac.c +
** m68k.c, RAM in cart SRAM, ROM+disk in cart ROM via macmem.c) and rebuilds a
** 256x224 window of the Mac framebuffer ($1A700, in SRAM) into a WRAM tile
** buffer, DMA'd to VRAM — so the SNES shows the boot live (Happy Mac, "Welcome
** to Macintosh", then the Finder desktop). Same Mode-0 2bpp path as macvideo.c.
**
** Controls (auto-read joypad): the d-pad moves the MOUSE and A clicks; hold L
** and the d-pad instead PANS the 256x224 window across the 512x342 Mac screen
** (the SNES can't show it all at once — a background addresses 1024 tiles, the
** whole screen is 2752); R toggles the ZOOMED-OUT view — the whole screen,
** 2x2-downscaled to 256x171 with four gray levels (we're in 2bpp Mode 0 and
** the normal view only ever used two colors, so the second bitplane + CGRAM
** colors 2/3 were free), letterboxed, table-driven (build/maczoom_tab.h). The mouse is HLE'd straight into the Mac's low-memory
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

#include "maczoom_tab.h"              /* ZPC/ZP0/ZP1: 2x2->4-gray tables */
#include "mackbd_tab.h"               /* KBD_FONT/MAP/GRID/KEYS: on-screen kbd */
static uint8_t zoom;                  /* 0 = 1:1 pan view, 1 = whole screen */
static uint8_t r_prev;                /* R edge detect for the toggle */

/* On-screen keyboard (SELECT toggles, like emu8086). It's BG2 over the bottom
** five tile rows: d-pad picks a key, A presses it (posts a real keyDown via
** mac_post_key -> the ROM's Event Manager). BG2 tiles live at VRAM word $3000
** (char base 3), its tilemap at $0400; Mode-0 puts BG2 on CGRAM colours 32+,
** so the panel/highlight palettes don't touch the Mac view's black & white. */
#define KBD_ROW0   23                 /* top keyboard row -> screen tile row 23 */
static uint8_t kbd_vis, ksel, kshift; /* visible? / selected key / sticky shift */
static uint8_t sel_prev, ka_prev;     /* SELECT / A edge detect */
static uint16_t kd_prev, nav_hold;    /* d-pad edge + autorepeat countdown */

/* The visible window into the Mac screen. panXb is the left edge in framebuffer
** BYTES (x8 = pixels), 0..32; panY is the top edge in pixel rows, 0..118 — the
** slack over the 512x342 screen. Default: top-centre, so the menu bar shows. */
static int panXb = 16, panY = 0;

/* Mouse (the d-pad with L up): mx,my is the cursor in Mac screen coords
** (0..511 / 0..341); A clicks. Fed to the Mac via mac_set_mouse; the Mac then
** redraws its own cursor on its VBL, so it lags on the slow CPU. */
#include "macmem.h"                   /* mac_ram_rd: reading the Mac's globals */

#define RUN_BATCH  32                 /* instructions per mac_run/input poll */
#define MOUSE_STEP 4                  /* pixels per input poll */
#define CLICK_HOLD 600                /* polls: 600*32 = 19200 steps > one VBL
                                      ** (12000), so a tap of A is seen */
static int mx = 256, my = 171, last_btn = 0;
static uint8_t mouse_synced;          /* first input adopts the Mac's RawMouse */
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
/* The zoomed-out renderer: 2x2 blocks -> one 4-gray pixel, whole screen at
** 256x171, letterboxed into tile rows 3-24 (3 blank rows above, ~3.6 below).
** Per output byte: two table adds give eight 2x2 black-counts (nibble sums
** can't carry — max 4), two more tables emit the bitplane pairs. ~C speed is
** fine here: this view is for orientation, not fast interaction. */
static void refresh_zoom(void){
  uint16_t ty, tx; uint8_t r;
  for (ty = 0; ty < TH; ty++) {
    volatile __far uint8_t *tb = (ty < 16) ? TB0 : TB1;
    uint16_t tbase = (uint16_t)(((ty < 16) ? ty : ty - 16) * TW * 16);
    for (r = 0; r < 8; r++) {
      int line = ty * 8 + r;
      int orow = line - 24;             /* content rows: screen lines 24-194 */
      uint16_t off = (uint16_t)(tbase + r * 2);
      if (orow < 0 || orow >= 171) {
        for (tx = 0; tx < TW; tx++) {
          tb[(uint16_t)(off + tx * 16)] = 0;      /* letterbox: white */
          tb[(uint16_t)(off + tx * 16 + 1)] = 0;
        }
      } else {
        const volatile __far uint8_t *ra =
          (const volatile __far uint8_t *)(0x7E0000ul + 0x1A700ul + (uint32_t)(orow * 2) * 64u);
        const volatile __far uint8_t *rb = ra + 64;
        for (tx = 0; tx < TW; tx++) {
          uint16_t i2 = (uint16_t)(tx * 2);
          uint16_t sL = (uint16_t)(ZPC[ra[i2]] + ZPC[rb[i2]]);
          uint16_t sR = (uint16_t)(ZPC[ra[i2 + 1]] + ZPC[rb[i2 + 1]]);
          tb[(uint16_t)(off + tx * 16)] = (uint8_t)
            ((ZP0[sL >> 8] << 6) | (ZP0[sL & 0xFF] << 4) |
             (ZP0[sR >> 8] << 2) |  ZP0[sR & 0xFF]);
          tb[(uint16_t)(off + tx * 16 + 1)] = (uint8_t)
            ((ZP1[sL >> 8] << 6) | (ZP1[sL & 0xFF] << 4) |
             (ZP1[sR >> 8] << 2) |  ZP1[sR & 0xFF]);
        }
      }
    }
  }
}

/* Leaving zoom: the 1:1 renderer only writes plane 0 (stride 16), so the
** grays' plane-1 bits would linger — clear them once. */
static void clear_plane1(void){
  uint16_t i;
  for (i = 0; i < 8192u; i += 2) TB0[i + 1] = 0;
  for (i = 0; i < TILEBYTES - 8192u; i += 2) TB1[i + 1] = 0;
}

static void refresh(void){
  if (zoom) {
    refresh_zoom();
  } else {
    tile_pan_xb = (uint16_t)panXb;
    tile_pan_y  = (uint16_t)panY;
    tile_rows();                        /* asm: fb -> tile buffer, ~1 ms */
  }
  REG8(0x2100) = 0x8F;                  /* blank just for the VRAM DMA */
  blit_tiles();
  REG8(0x2100) = 0x0F;                  /* screen on */
}

/* Write BG2's tilemap rows for the keyboard, highlighting the selected key
** (palette 1 = inverted). 5 rows x 32 cols at tilemap $0400, rows 23-27. */
static void kbd_draw(void){
  uint16_t r, c;
  REG8(0x2100) = 0x8F;                  /* force blank: VRAM is writable only in
                                        ** blank, else the PPU drops the writes */
  for (r = 0; r < 5; r++) {
    REG8(0x2115) = 0x80;
    REG16(0x2116) = (uint16_t)(0x0400 + (KBD_ROW0 + r) * 32);
    for (c = 0; c < 32; c++) {
      uint16_t t = KBD_MAP[r][c];
      if (KBD_GRID[r][c] == ksel) t += KBD_HILITE;   /* selected -> inverted tile */
      REG16(0x2118) = t;
    }
  }
  REG8(0x2100) = 0x0F;                  /* screen on */
}

/* Center column of a key, for up/down navigation. */
static int kbd_cx(uint8_t k){ return KBD_KEYS[k].x + KBD_KEYS[k].w / 2; }

/* Move the selection by a d-pad direction (bit set in d). */
static void kbd_nav(uint16_t d){
  uint8_t r = KBD_KEYS[ksel].row, best = ksel, i;
  if (d & 0x0200u) {                   /* Left: nearest key left, same row */
    int bx = -1;
    for (i = 0; i < KBD_NKEYS; i++)
      if (KBD_KEYS[i].row == r && KBD_KEYS[i].x < KBD_KEYS[ksel].x &&
          (int)KBD_KEYS[i].x > bx) { bx = KBD_KEYS[i].x; best = i; }
  } else if (d & 0x0100u) {            /* Right */
    int bx = 0x7FFF;
    for (i = 0; i < KBD_NKEYS; i++)
      if (KBD_KEYS[i].row == r && KBD_KEYS[i].x > KBD_KEYS[ksel].x &&
          KBD_KEYS[i].x < bx) { bx = KBD_KEYS[i].x; best = i; }
  } else {                             /* Up / Down: nearest column in next row */
    int tr = (d & 0x0800u) ? (int)r - 1 : (int)r + 1;
    int cx = kbd_cx(ksel), bd = 0x7FFF;
    if (tr < 0) tr = 4; else if (tr > 4) tr = 0;
    for (i = 0; i < KBD_NKEYS; i++)
      if (KBD_KEYS[i].row == tr) {
        int dd = kbd_cx(i) - cx; if (dd < 0) dd = -dd;
        if (dd < bd) { bd = dd; best = i; }
      }
  }
  ksel = best;
}

/* Press the selected key: SHIFT toggles sticky shift, anything else posts a
** keyDown with the shifted or plain character (then clears a one-shot shift). */
static void kbd_press(void){
  uint8_t ch = KBD_KEYS[ksel].ch;
  if (ch == 0xFF) { kshift ^= 1; kbd_draw(); return; }   /* SHIFT */
  mac_post_key(KBD_KEYS[ksel].kc, kshift ? KBD_KEYS[ksel].sh : ch);
  if (kshift) { kshift = 0; }
}

/* Poll the auto-read joypad. Two modes: hold L and the d-pad PANS the window
** (8px horiz / 4px vert); with L up the d-pad moves the MOUSE and A clicks.
** Returns 1 if the display should be redrawn now (view moved, or the cursor /
** button changed). */
static int handle_input(void){
  uint16_t p = REG16(0x4218);          /* JOY1: B Y Sel St U D L R | A X L R .... */
  uint8_t sel = (p & 0x2000u) ? 1 : 0; /* SELECT: toggle the on-screen keyboard */
  uint8_t rr = (p & 0x0010u) ? 1 : 0;  /* R shoulder: toggle the zoomed view */

  if (sel && !sel_prev) {              /* SELECT edge: show/hide the keyboard */
    sel_prev = sel;
    kbd_vis ^= 1;
    if (kbd_vis) kbd_draw();
    REG8(0x212C) = kbd_vis ? 0x03 : 0x01;   /* main screen: BG1 (+BG2 when up) */
    return 0;                          /* BG2 is independent of the tile blit */
  }
  sel_prev = sel;

  if (kbd_vis) {                        /* keyboard mode: d-pad navigates, A types */
    uint8_t a = (p & 0x0080u) ? 1 : 0;
    uint16_t d = p & 0x0F00u;           /* d-pad bits */
    uint16_t fresh = (uint16_t)(d & ~kd_prev);   /* newly-pressed directions */
    kd_prev = d;
    if (a && !ka_prev) kbd_press();     /* A edge -> press the selected key */
    ka_prev = a;
    /* Move on a fresh press; while held, autorepeat on a coarse countdown
    ** (input is polled many times per frame, so the count is large). */
    if (fresh) { kbd_nav(fresh); kbd_draw(); nav_hold = 2000; }
    else if (d) { if (nav_hold) nav_hold--; else { kbd_nav(d); kbd_draw(); nav_hold = 600; } }
    else nav_hold = 0;
    return 0;
  }
  if (rr && !r_prev) {
    r_prev = rr;
    zoom ^= 1;
    if (!zoom) clear_plane1();         /* grays out, 1-bit view back */
    return 1;                          /* redraw in the new mode */
  }
  r_prev = rr;
  if (!zoom && (p & 0x0020u)) {        /* L held -> PAN (zoom shows it all) */
    int ox = panXb, oy = panY;
    if (p & 0x0200u) panXb--;          /* Left */
    if (p & 0x0100u) panXb++;          /* Right */
    if (p & 0x0800u) panY -= PANY_STEP; /* Up */
    if (p & 0x0400u) panY += PANY_STEP; /* Down */
    if (panXb < 0) panXb = 0; else if (panXb > 32)  panXb = 32;
    if (panY  < 0) panY  = 0; else if (panY  > 118) panY  = 118;
    return (panXb != ox) || (panY != oy);
  } else {                             /* MOUSE */
    int omx, omy, a, btn;
    if (!mouse_synced) {               /* adopt the Mac's real cursor position:
                                       ** ours started as a guess, and the first
                                       ** press used to TELEPORT the cursor to it
                                       ** (RawMouse $082C = Point{v,h}) */
      my = (int)((mac_ram_rd(0x082Cu) << 8) | mac_ram_rd(0x082Du));
      mx = (int)((mac_ram_rd(0x082Eu) << 8) | mac_ram_rd(0x082Fu));
      if (mx < 0 || mx > 511) mx = 256;
      if (my < 0 || my > 341) my = 171;
      mouse_synced = 1;
    }
    omx = mx; omy = my; a = (p & 0x0080u) ? 1 : 0;
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
  REG8(0x2108) = 0x04;                  /* BG2SC tilemap word $0400, 32x32 */
  REG8(0x210B) = 0x31;                  /* char base: BG1 word $1000, BG2 $3000 */
  REG8(0x2110) = 0; REG8(0x2110) = 0;   /* BG2 scroll 0 */
  REG8(0x2121) = 0;                     /* CGRAM colour 0 = white, 1 = black */
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;   /* 0: white */
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;   /* 1: black */
  REG8(0x2122) = 0xF7; REG8(0x2122) = 0x5E;   /* 2: light gray (zoom) */
  REG8(0x2122) = 0x8C; REG8(0x2122) = 0x31;   /* 3: dark gray  (zoom) */
  /* BG2 palette (Mode 0 offsets BG2 to CGRAM colour 32). Keyboard tiles use
  ** value 2 = panel, 3 = ink; palette 0 = white key / black text, palette 1
  ** (colour 36+) = the inverted highlight for the selected key. */
  REG8(0x2121) = 34;
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;   /* 34: black key panel */
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;   /* 35: white ink       */
  /* highlight is done with inverted tiles (KBD_HILITE), not a palette swap:
  ** BG2 ignores the tilemap palette bits in Mode 0 (verified in Mesen). */
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x0000;          /* identity tilemap */
  for (i = 0; i < 1024; i++) REG16(0x2118) = (i < n) ? i : 0;
  REG8(0x210D) = 0; REG8(0x210D) = 0;
  REG8(0x210E) = 0; REG8(0x210E) = 0;
  REG8(0x212C) = 0x01;                  /* main screen: BG1 */
  for (i = 0; i < 8192u; i++) TB0[i] = 0;               /* plane1 stays 0 forever */
  for (i = 0; i < TILEBYTES - 8192u; i++) TB1[i] = 0;

  /* Upload the keyboard font tiles to VRAM word $3000 (BG2 char base) and
  ** clear its tilemap ($0400) to the transparent tile 0. */
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x3000;
  for (i = 0; i < (uint16_t)(sizeof KBD_FONT / 2); i++)
    REG16(0x2118) = (uint16_t)(KBD_FONT[i * 2] | (KBD_FONT[i * 2 + 1] << 8));
  REG16(0x2116) = 0x0400;
  for (i = 0; i < 1024; i++) REG16(0x2118) = 0;
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
#ifdef DISK2_SIZE
  mac_set_disk2(0, DISK2_SIZE);         /* the EXTERNAL drive ($E00000): present
                                        ** only when roms/disk2_data.bin exists */
#endif
#ifdef MAC_FASTBOOT
  mac_fastboot_arm(0);                  /* skip the RAM test + boot chime: teleport
                                        ** to the first disk read (state embedded
                                        ** at $D80000; see docs/PERF.md I1) */
#endif
  refresh();                            /* first draw, screen on */
  for (;;) {
    mac_run(RUN_BATCH);                 /* one call per batch, not per step */
    if (handle_input()) {               /* view/cursor changed: redraw now */
      refresh();
      idle = 0;
    } else if ((idle += RUN_BATCH) >= BOOT_REFRESH) { /* idle: boot progress */
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
