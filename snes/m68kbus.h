/* Inline bus fast paths for the 68000 core on the SNES (docs/PERF.md item 1
** of the profiler round). Included by m68k.c when built -DM68K_BUS_INLINE:
** the RAM/ROM decode and the PC-window fetch-cache hit run straight-line at
** the call site — no jsl into mac.c per memory access. Anything else (odd
** addresses, VIA/IWM/SCC, open bus) falls back to the mac.c bus functions,
** so behavior is byte-identical: this header mirrors the decode in mac.c's
** m68k_read16/write16/bus_read (the Mac 128K address map, see mac.h) and
** must be kept in sync with it — the parity gate is the enforcement.
**
** State shared with mac.c: mac_overlay_on (the reset ROM-overlay), and the
** fetch cache (mac_fetch_tag/mac_fetch_base, invalidated by mac.c on
** overlay flips and re-init). */
#ifndef M68KBUS_H
#define M68KBUS_H
#include "macmem.h"

extern M68K_HOT int mac_overlay_on;
extern M68K_HOT uint32_t mac_fetch_tag;
extern M68K_HOT uint32_t mac_fetch_base;   /* raw SNES address of the block */

static inline uint8_t m68k_bus_rd8(uint32_t a){
  a &= 0xFFFFFFul;
  if (a < 0x400000ul) {
    if (!mac_overlay_on) return mac_ram_rd(a & 0x1FFFFul);
    return mac_rom_rd(a & 0xFFFFul);
  }
  if (a < 0x500000ul) return mac_rom_rd(a & 0xFFFFul);
  if (a >= 0x600000ul && a < 0x800000ul && mac_overlay_on)
    return mac_ram_rd(a & 0x1FFFFul);
  return m68k_read8(a);                       /* I/O / open bus: the full bus */
}
static inline uint16_t m68k_bus_rd16(uint32_t a){
  a &= 0xFFFFFFul;
  if (!(a & 1)) {
    if (a < 0x400000ul) {
      if (!mac_overlay_on) return mac_ram_rd16(a & 0x1FFFFul);
      return mac_rom_rd16(a & 0xFFFFul);
    }
    if (a < 0x500000ul) return mac_rom_rd16(a & 0xFFFFul);
    if (a >= 0x600000ul && a < 0x800000ul && mac_overlay_on)
      return mac_ram_rd16(a & 0x1FFFFul);
  }
  return m68k_read16(a);                      /* odd / I/O / edge */
}
static inline uint32_t m68k_bus_rd32(uint32_t a){
  return ((uint32_t)m68k_bus_rd16(a) << 16) | m68k_bus_rd16(a + 2);
}
static inline void m68k_bus_wr8(uint32_t a, uint8_t v){
  a &= 0xFFFFFFul;
  if (a < 0x400000ul) { if (!mac_overlay_on) mac_ram_wr(a & 0x1FFFFul, v); return; }
  if (a >= 0x600000ul && a < 0x800000ul) { if (mac_overlay_on) mac_ram_wr(a & 0x1FFFFul, v); return; }
  m68k_write8(a, v);                          /* IWM / VIA / ignored ranges */
}
static inline void m68k_bus_wr16(uint32_t a, uint16_t v){
  a &= 0xFFFFFFul;
  if (!(a & 1)) {
    if (a < 0x400000ul) { if (!mac_overlay_on) { mac_ram_wr16(a & 0x1FFFFul, v); return; } }
    else if (a >= 0x600000ul && a < 0x800000ul) { if (mac_overlay_on) { mac_ram_wr16(a & 0x1FFFFul, v); return; } }
  }
  m68k_write16(a, v);
}
static inline void m68k_bus_wr32(uint32_t a, uint32_t v){
  m68k_bus_wr16(a, (uint16_t)(v >> 16)); m68k_bus_wr16(a + 2, (uint16_t)v);
}
static inline uint16_t m68k_bus_fetch16(uint32_t pc){
  if ((pc & 0xFFFFE000ul) == mac_fetch_tag) {
    uint16_t off = (uint16_t)(pc & 0x1FFFu);
    const volatile __far uint8_t *p = (const volatile __far uint8_t *)mac_fetch_base;
    return (uint16_t)(((uint16_t)p[off] << 8) | p[off + 1]);
  }
  return m68k_fetch16(pc);                    /* refill (mac.c) */
}

#define M68K_RD8(a)      m68k_bus_rd8(a)
#define M68K_RD16(a)     m68k_bus_rd16(a)
#define M68K_RD32(a)     m68k_bus_rd32(a)
#define M68K_WR8(a, v)   m68k_bus_wr8((a), (v))
#define M68K_WR16(a, v)  m68k_bus_wr16((a), (v))
#define M68K_WR32(a, v)  m68k_bus_wr32((a), (v))
#define M68K_FETCH16(a)  m68k_bus_fetch16(a)

#endif
