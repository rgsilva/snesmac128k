/* SNES memory backend for the Mac 128K machine model (mac.c built -DMAC_SNES),
** as static-inline accessors — bus_read/bus_write compile straight through to
** the window math with no jsl/rtl layers (docs/PERF.md item E).
**
** RAM  — TWO regions, both with call-free constant-base addressing
**        (docs/PERF.md item H — the end of the window tax):
**          a <  $2000: ONE fixed cart-SRAM window, $A06000 + a (the fast
**                      mirror of bank $20; 6 master clocks with MEMSEL).
**          a >= $2000: WRAM, LINEAR: $7E0000 + a  ($7E2000-$7FFFFF; our
**                      own runtime owns $7E0000-$1FFF, see linker.scm).
**        The 8 KB-window math ((a>>13)*$10000) is gone entirely.
** ROM  — the 64 KB Mac ROM and 400 KB floppy image are appended to the cart
**        ROM by raw2sfc.py ($080000, $100000): HiROM maps them linearly at
**        $C80000 / $D00000 (fast banks).
** WORD ops (item D): 68000 word accesses are even-aligned and the windows are
** 8 KB-aligned, so an aligned word NEVER straddles a window — one window
** computation covers both bytes, big-endian composed. Callers guarantee even
** addresses (mac.c routes odd/IO through the byte path).
**
** Calypsi gotchas (learned the hard way, keep): every caller must see these
** declarations (an implicit C89 call corrupts args via the 16-bit-int ABI),
** the address math needs `ul` constants + a real local for the far pointer. */
#ifndef MACMEM_H
#define MACMEM_H
#include <stdint.h>

/* Mac RAM cell -> SNES far address. An even-aligned word never spans the
** $1FFF/$2000 seam (both bytes land in the same region), so the word ops
** need only one region test. Each accessor branches to a complete
** straight-line path (a helper returning the pointer made Calypsi spill
** it through the direct page — measurably slower). */
#define MAC_RAM_HI(a) ((volatile __far uint8_t *)(0x7E0000ul + (a)))
#define MAC_RAM_LO(a) ((volatile __far uint8_t *)(0xA06000ul + (a)))
static inline const volatile __far uint8_t *mac_ram_ptr8(uint32_t a){
  if (a >= 0x2000ul) return MAC_RAM_HI(a);
  return MAC_RAM_LO(a);
}

static inline uint8_t mac_ram_rd(uint32_t a){
  if (a >= 0x2000ul) return *MAC_RAM_HI(a);
  return *MAC_RAM_LO(a);
}
static inline void mac_ram_wr(uint32_t a, uint8_t v){
  if (a >= 0x2000ul) { *MAC_RAM_HI(a) = v; return; }
  *MAC_RAM_LO(a) = v;
}
static inline uint16_t mac_ram_rd16(uint32_t a){
  if (a >= 0x2000ul) {
    volatile __far uint8_t *p = MAC_RAM_HI(a);
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
  } else {
    volatile __far uint8_t *p = MAC_RAM_LO(a);
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
  }
}
static inline void mac_ram_wr16(uint32_t a, uint16_t v){
  if (a >= 0x2000ul) {
    volatile __far uint8_t *p = MAC_RAM_HI(a);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
  } else {
    volatile __far uint8_t *p = MAC_RAM_LO(a);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
  }
}
static inline uint8_t mac_rom_rd(uint32_t a){
  const __far uint8_t *p = (const __far uint8_t *)(0xC80000ul + a);
  return *p;
}
static inline uint16_t mac_rom_rd16(uint32_t a){
  const __far uint8_t *p = (const __far uint8_t *)(0xC80000ul + a);
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint8_t mac_disk_rd(uint32_t a){
  const __far uint8_t *p = (const __far uint8_t *)(0xD00000ul + a);
  return *p;
}

#endif
