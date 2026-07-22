/* SNES memory backend for the Mac 128K machine model (mac.c built -DMAC_SNES).
**
** RAM  — the Mac's 128 KB lives in HiROM cart SRAM, 8 KB windows at banks
**        $20-$2F, offset $6000-$7FFF (probed: $20-$3F are all backed in Mesen).
**        Mac address a (0..$1FFFF) -> bank $20+(a>>13), offset $6000+(a&$1FFF).
**        Address built with named uint32_t locals + `ul` constants (a
**        `(cast)const<<16` inside an expression truncated to 16 bits on Calypsi,
**        losing the bank byte), and the far pointer is a local variable (a helper
**        returning a __far pointer dropped writes).
** ROM  — the 64 KB Mac ROM and 400 KB floppy image are appended to the cart ROM
**        at fixed byte offsets by raw2sfc.py ($080000, $100000). HiROM maps SNES
**        $C00000+N linearly to ROM byte N, so ROM is at $C80000, disk at $D00000.
*/
#include <stdint.h>

/* SNES address of Mac RAM cell a: base $A06000 (the FAST mirror of $206000;
** banks $80-$FF run at 6 master clocks with MEMSEL) + window*$10000 + (a & $1FFF). */
#define MAC_SRAM(a)  (0xA06000ul + ((a) >> 13) * 0x10000ul + ((a) & 0x1FFFul))

uint8_t mac_ram_rd(uint32_t a){
  volatile __far uint8_t *p = (volatile __far uint8_t *)MAC_SRAM(a);
  return *p;
}
void mac_ram_wr(uint32_t a, uint8_t v){
  volatile __far uint8_t *p = (volatile __far uint8_t *)MAC_SRAM(a);
  *p = v;
}
uint8_t mac_rom_rd(uint32_t a){
  const __far uint8_t *p = (const __far uint8_t *)(0xC80000ul + a);
  return *p;
}
uint8_t mac_disk_rd(uint32_t a){
  const __far uint8_t *p = (const __far uint8_t *)(0xD00000ul + a);
  return *p;
}
