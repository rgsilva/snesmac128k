/* Identical CPU-state fold used on both host and SNES so a per-step cumulative
** hash can be compared (the M2 parity idea, applied to the whole Mac machine).
** Endian- and compiler-independent (no raw struct hashing: Calypsi vs host
** padding differ). The mixer is rot1-xor-add, NOT FNV (legacy names kept):
** FNV's 32-bit multiply is a Calypsi runtime call, and at 20 folds per step
** the old hash made the parity gate ~4x slower than the machine it checks —
** which is what pushed runs into Mesen's hard 100-second testrunner kill.
** The rotate is done in 16-bit halves so Calypsi compiles it inline. */
#ifndef MACPARITY_HASH_H
#define MACPARITY_HASH_H
#include <stdint.h>
#include "m68k.h"

#define MACP_FNV_INIT 2166136261ul
#define MACP_RAM_STRIDE 16u   /* sample the RAM sweep: all 128 KB blew the budget */

static uint32_t macp_fold32(uint32_t h, uint32_t v){
  uint16_t hi = (uint16_t)(h >> 16), lo = (uint16_t)h;
  uint16_t nh = (uint16_t)((uint16_t)(hi << 1) | (lo >> 15));
  uint16_t nl = (uint16_t)((uint16_t)(lo << 1) | (hi >> 15));
  return ((((uint32_t)nh << 16) | nl) ^ v) + 0x9E3779B9ul;
}
static uint32_t macp_fnv1(uint32_t h, uint8_t b){ return macp_fold32(h, (uint32_t)b); }
static uint32_t macp_hash_cpu(uint32_t h){
  int i;
  for (i = 0; i < 8; i++) h = macp_fold32(h, m68kc.d[i]);
  for (i = 0; i < 8; i++) h = macp_fold32(h, m68kc.a[i]);
  h = macp_fold32(h, m68kc.pc);
  h = macp_fold32(h, m68kc.usp);
  h = macp_fold32(h, m68kc.isp);
  h = macp_fold32(h, (uint32_t)m68kc.sr);
  return h;
}

#endif
