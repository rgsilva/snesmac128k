/* Identical CPU-state fold used on both host and SNES so a per-step cumulative
** hash can be compared (the M2 parity idea, applied to the whole Mac machine).
** FNV-1a over each architectural field as little-endian bytes — endian- and
** compiler-independent (no raw struct hashing: Calypsi vs host padding differ). */
#ifndef MACPARITY_HASH_H
#define MACPARITY_HASH_H
#include <stdint.h>
#include "m68k.h"

#define MACP_FNV_INIT 2166136261ul

static uint32_t macp_fnv1(uint32_t h, uint8_t b){ return (h ^ (uint32_t)b) * 16777619ul; }
static uint32_t macp_fold32(uint32_t h, uint32_t v){
  h = macp_fnv1(h, (uint8_t)v);       h = macp_fnv1(h, (uint8_t)(v >> 8));
  h = macp_fnv1(h, (uint8_t)(v >> 16)); h = macp_fnv1(h, (uint8_t)(v >> 24));
  return h;
}
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
