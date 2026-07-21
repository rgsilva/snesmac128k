/* M4b: the live Mac 128K, booting on the SNES. Runs the machine model (mac.c +
** m68k.c, RAM in cart SRAM, ROM+disk in cart ROM via macmem.c) and, every so
** many instructions, rebuilds a 256x224 centre crop of the Mac framebuffer
** ($1A700, in SRAM) into a WRAM tile buffer and DMAs it to VRAM — so the SNES
** shows the boot progress live (Happy Mac, then, given enough time, the disk
** boots to the Finder desktop). It is faithful but SLOW (a full boot is the
** documented tens-of-minutes-to-hours; SA-1 is the eventual fix), so this is a
** "leave it running" demo. Same Mode-0 2bpp display path as macvideo.c (M4). */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "mac.h"

#define REG8(a)  (*(volatile uint8_t  *)(unsigned long)(a))
#define REG16(a) (*(volatile uint16_t *)(unsigned long)(a))

#define DISK_SIZE      409600ul                       /* 400K floppy at $D00000 */
#define TILEBUF        ((volatile __far uint8_t *)0x7E8000ul)  /* 32x28 tiles, 14 KB */
#define REFRESH_STEPS  4096u                          /* redraw every N instructions */
#define TW 32
#define TH 28

uint8_t mac_ram_rd(uint32_t a);

/* DMA the WRAM tile buffer to VRAM word $1000 (A-bus WRAM -> B-bus $2118/$2119). */
static void blit_tiles(void){
  REG8(0x2115) = 0x80;                 /* VMAIN: word, +1 after $2119 */
  REG16(0x2116) = 0x1000;              /* VRAM word address */
  REG8(0x4300) = 0x01;                 /* A->B, 2 regs */
  REG8(0x4301) = 0x18;                 /* B-bus $2118 */
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x80; REG8(0x4304) = 0x7E;  /* src $7E8000 */
  REG16(0x4305) = (uint16_t)(TW * TH * 16);
  REG8(0x420B) = 0x01;
}

/* Rebuild the tile buffer from the SRAM framebuffer: centre crop = Mac bytes
** 16..47 (columns 128..383) of rows 59..282. Tile (tx,ty) row r -> plane0 =
** that fb byte (Mac 1 = black = colour 1), plane1 = 0. */
static void refresh(void){
  uint16_t tx, ty; uint8_t r;
  REG8(0x2100) = 0x8F;                 /* force blank while we touch VRAM */
  for (ty = 0; ty < TH; ty++) {
    for (tx = 0; tx < TW; tx++) {
      uint16_t k = (uint16_t)(ty * TW + tx);
      uint32_t base = 0x1A700ul + (uint32_t)(59 + ty * 8) * 64u + (16 + tx);
      uint16_t off = (uint16_t)(k * 16);
      for (r = 0; r < 8; r++) {
        TILEBUF[off + r * 2]     = mac_ram_rd(base + (uint32_t)r * 64u);
        TILEBUF[off + r * 2 + 1] = 0;
      }
    }
  }
  blit_tiles();
  REG8(0x2100) = 0x0F;                 /* screen on */
}

static void video_init(void){
  REG8(0x2100) = 0x8F;                 /* force blank */
  REG8(0x2105) = 0x00;                 /* BGMODE 0 */
  REG8(0x2107) = 0x00;                 /* BG1SC tilemap word $0000, 32x32 */
  REG8(0x210B) = 0x01;                 /* BG1 char base word $1000 */
  REG8(0x2121) = 0;                    /* CGRAM colour 0 = white, 1 = black */
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;
  { uint16_t i, n = TW * TH;           /* identity tilemap */
    REG8(0x2115) = 0x80; REG16(0x2116) = 0x0000;
    for (i = 0; i < 1024; i++) REG16(0x2118) = (i < n) ? i : 0; }
  REG8(0x210D) = 0; REG8(0x210D) = 0;
  REG8(0x210E) = 0; REG8(0x210E) = 0;
  REG8(0x212C) = 0x01;                 /* main screen: BG1 */
}

int main(void){
  uint32_t steps = 0, k;
  mb_init();
  mb_puts("macboot: live Mac 128K on the SNES\n");
  video_init();
  mac_init(0);                         /* ROM in cart, RAM (SRAM) cleared */
  mac_set_disk(0, DISK_SIZE);          /* enable the HLE .Sony (disk at $D00000) */
  refresh();                           /* show the (blank) screen, then boot */
  for (;;) {
    for (k = 0; k < REFRESH_STEPS; k++) mac_step();
    steps += REFRESH_STEPS;
    refresh();
    mb_puts("steps "); mb_puthex32(steps);
    mb_puts(" vbl "); mb_puthex32(mac_vbl_count()); mb_putc('\n');
  }
  return 0;
}
