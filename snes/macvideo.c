/* M4: display a captured Mac framebuffer on the SNES. Mode 0, one 2bpp BG:
** DMA the 256x224 tile crop (from mkmacfb) to VRAM, write an identity tilemap,
** set a 2-colour white/black palette (Mac 1 = black), and turn the screen on.
** The Happy Mac appears in Mesen. Proves the framebuffer->SNES video path;
** a live boot on the SNES (slow) reuses it later. */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "macfb_tiles.h"      /* MACFB_TW, MACFB_TH, MACFB_TILES[] — generated */

#define REG8(a)  (*(volatile uint8_t  *)(unsigned long)(a))
#define REG16(a) (*(volatile uint16_t *)(unsigned long)(a))

static void dma_to_vram(uint16_t vword, const unsigned char *src, uint16_t bytes){
  unsigned long s = (unsigned long)src;
  REG8(0x2115) = 0x80;                 /* VMAIN: word, increment after $2119 */
  REG16(0x2116) = vword;               /* VRAM word address */
  REG8(0x4300) = 0x01;                 /* DMA0: A->B, 2 regs ($2118/$2119) */
  REG8(0x4301) = 0x18;                 /* B-bus = $2118 (VMDATAL) */
  REG8(0x4302) = (uint8_t)s;
  REG8(0x4303) = (uint8_t)(s >> 8);
  REG8(0x4304) = (uint8_t)(s >> 16);
  REG16(0x4305) = bytes;
  REG8(0x420B) = 0x01;                 /* start channel 0 */
}

int main(void){
  uint16_t i, n;
  mb_init();
  REG8(0x2100) = 0x8F;                 /* force blank while we load VRAM */

  REG8(0x2105) = 0x00;                 /* BGMODE 0 */
  REG8(0x2107) = 0x00;                 /* BG1SC: tilemap at word $0000, 32x32 */
  REG8(0x210B) = 0x01;                 /* BG12NBA: BG1 char base = word $1000 */

  /* palette: colour 0 = white, colour 1 = black (Mac sets bit = black).
  ** CGDATA ($2122) is a byte register — write low then high, NOT a 16-bit
  ** store (that would put the high byte in $2123). */
  REG8(0x2121) = 0;
  REG8(0x2122) = 0xFF; REG8(0x2122) = 0x7F;   /* colour 0 = white 0x7FFF */
  REG8(0x2122) = 0x00; REG8(0x2122) = 0x00;   /* colour 1 = black 0x0000 */

  dma_to_vram(0x1000, MACFB_TILES, (uint16_t)(MACFB_TW * MACFB_TH * 16));

  /* identity tilemap: entry k -> tile k; off-screen rows (>=896) -> tile 0 */
  n = (uint16_t)(MACFB_TW * MACFB_TH);
  REG8(0x2115) = 0x80;
  REG16(0x2116) = 0x0000;
  for (i = 0; i < 1024; i++) REG16(0x2118) = (i < n) ? i : 0;

  REG8(0x210D) = 0; REG8(0x210D) = 0;  /* BG1 H scroll = 0 */
  REG8(0x210E) = 0; REG8(0x210E) = 0;  /* BG1 V scroll = 0 */
  REG8(0x212C) = 0x01;                 /* main screen: BG1 */
  REG8(0x2100) = 0x0F;                 /* screen on, full brightness */

  mb_puts("macvideo: Happy Mac on the SNES\n");
  mb_finish(MB_STATUS_PASS);
  return 0;
}
