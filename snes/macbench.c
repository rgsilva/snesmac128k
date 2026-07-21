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

uint8_t mac_ram_rd(uint32_t a);          /* macmem.c (declare: 16-bit-int ABI!) */

#define REG8(a)  (*(volatile uint8_t  *)(unsigned long)(a))
#define REG16(a) (*(volatile uint16_t *)(unsigned long)(a))

#define TILEBUF   ((volatile __far uint8_t *)0x7E8000ul)
#define TW 32
#define TH 28
#define TILEBYTES (TW * TH * 16)
#define FB_SRAM(a) (0x206000ul + ((a) >> 13) * 0x10000ul + ((a) & 0x1FFFul))

static void blit_tiles(void){
  REG8(0x2115) = 0x80; REG16(0x2116) = 0x1000;
  REG8(0x4300) = 0x01; REG8(0x4301) = 0x18;
  REG8(0x4302) = 0x00; REG8(0x4303) = 0x80; REG8(0x4304) = 0x7E;
  REG16(0x4305) = (uint16_t)TILEBYTES;
  REG8(0x420B) = 0x01;
}

static void refresh(void){               /* fixed centre crop, as macboot */
  uint16_t tx, ty; uint8_t r;
  for (ty = 0; ty < TH; ty++) {
    uint16_t trow = (uint16_t)(ty * TW);
    for (r = 0; r < 8; r++) {
      uint32_t addr = 0x1A700ul + (uint32_t)(ty * 8 + r) * 64u + 16u;
      const volatile __far uint8_t *sp = (const volatile __far uint8_t *)FB_SRAM(addr);
      for (tx = 0; tx < TW; tx++)
        TILEBUF[(uint16_t)((trow + tx) * 16 + r * 2)] = sp[tx];
    }
  }
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
  for (i = 0; i < TILEBYTES; i++) TILEBUF[i] = 0;
}

int main(void){
  uint16_t prev = 0; uint8_t poll = 0;
  mb_init();
  mb_puts("macbench: our own 68000 ROM, phased workload\n");
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
        refresh();                       /* phase boundary: cheap, off the clock */
      }
    }
  }
  return 0;
}
