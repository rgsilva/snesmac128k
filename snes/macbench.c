/* macbench: boot OUR OWN 68000 ROM (bench/mkbenchrom.py, embedded in the cart
** at the usual $C80000 slot — zero Apple bytes) and run its phased workload on
** the live machine model. The ROM writes its current phase to RAM $0100; we
** poll that every 16 steps and print each transition to the mailbox, where
** test/macbench.lua stamps it with the frame count — frames-per-phase over
** the host's instructions-per-phase (benchhost) is the cycles-per-instruction
** table in docs/PERF.md. The final CRC (RAM $0104) must equal the host's: the
** semantic gate for every optimization.
**
** The disk is "inserted" (dummy) so the per-step .Sony guard cost is measured
** exactly as a real boot pays it. The fbfill phase paints the Mac framebuffer,
** so the screen shows the run: white, then stripes when the fill lands. */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "mac.h"

#include "macmem.h"               /* inline accessors (mac_ram_rd) */

#define REG8(a)  (*(volatile uint8_t  *)(unsigned long)(a))
#define REG16(a) (*(volatile uint16_t *)(unsigned long)(a))

#define TW 32
#define TH 28
#define TILEBYTES (TW * TH * 16)
/* tile buffer in cart SRAM windows 16+17; framebuffer read linear from WRAM
** (item H — see macboot.c for the layout notes) */
#define TB0       ((volatile __far uint8_t *)0xB06000ul)
#define TB1       ((volatile __far uint8_t *)0xB16000ul)

extern void tile_rows(void);             /* calypsi/tilecopy.s */
extern uint16_t tile_pan_xb, tile_pan_y; /* fixed centre crop */

static void blit_tiles(void){
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x1000;
  REG8(0x4300) = 0x01; REG8(0x4301) = 0x18;
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x60; REG8(0x4304) = 0xB0;
  REG16(0x4305) = 8192u;
  REG8(0x420B) = 0x01;
  REG16(0x2116) = 0x2000;
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x60; REG8(0x4304) = 0xB1;
  REG16(0x4305) = (uint16_t)(TILEBYTES - 8192u);
  REG8(0x420B) = 0x01;
}

static void refresh(void){               /* fixed centre crop, as macboot */
  tile_pan_xb = 16; tile_pan_y = 0;
  tile_rows();
  REG8(0x2100) = 0x8F; blit_tiles(); REG8(0x2100) = 0x0F;
}

static void video_init(void){
  uint16_t i, n = TW * TH;
  REG8(0x2100) = 0x8F; REG8(0x2105) = 0x00; REG8(0x2107) = 0x00; REG8(0x210B) = 0x01;
  REG8(0x2121) = 0;
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x0000;
  for (i = 0; i < 1024; i++) REG16(0x2118) = (i < n) ? i : 0;
  REG8(0x210D) = 0; REG8(0x210D) = 0;
  REG8(0x210E) = 0; REG8(0x210E) = 0;
  REG8(0x212C) = 0x01;
  for (i = 0; i < 8192u; i++) TB0[i] = 0;
  for (i = 0; i < TILEBYTES - 8192u; i++) TB1[i] = 0;
}

int main(void){
  uint16_t prev = 0; uint8_t poll = 0;
  mb_init();
  mb_puts("macbench: our own 68000 ROM, phased workload\n");
  REG8(0x420D) = 0x01;                  /* MEMSEL: FastROM (map mode $31) */
  video_init();
  mac_init(0);
  mac_set_disk(0, 409600ul);             /* dummy: arm the per-step trap guard */
  refresh();
  for (;;) {
    mac_step();
    if (++poll >= 16) {
      poll = 0;
      uint16_t mark = (uint16_t)((mac_ram_rd(0x100u) << 8) | mac_ram_rd(0x101u));
      if (mark != prev) {
        prev = mark;
        mb_puts("P "); mb_puthex32(mark); mb_putc('\n');
        if (mark == 0xB0CAu) {
          uint32_t crc = ((uint32_t)mac_ram_rd(0x104u) << 24) |
                         ((uint32_t)mac_ram_rd(0x105u) << 16) |
                         ((uint32_t)mac_ram_rd(0x106u) << 8)  |
                          (uint32_t)mac_ram_rd(0x107u);
          mb_puts("CRC "); mb_puthex32(crc); mb_putc('\n');
          refresh();                     /* show the filled framebuffer */
          mb_finish(MB_STATUS_PASS);
        }
        refresh();                       /* ~1 ms in asm — visible progress
                                         ** without polluting the phase times */
      }
    }
  }
  return 0;
}
