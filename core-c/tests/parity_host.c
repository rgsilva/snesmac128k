/* Host side of the M2 parity check: backs the 68000's memory with a plain
** array and prints the reference hash the SNES ROM must reproduce. */
#include <stdio.h>
#include <stdint.h>
#include "m68k.h"

static uint8_t g[0x10000];
uint8_t  m68k_read8(uint32_t a){ return g[a & 0xFFFF]; }
uint16_t m68k_read16(uint32_t a){ a &= 0xFFFF; return ((uint16_t)g[a] << 8) | g[(a + 1) & 0xFFFF]; }
uint16_t m68k_fetch16(uint32_t a){ return m68k_read16(a); }
uint32_t m68k_read32(uint32_t a){ a &= 0xFFFF;
  return ((uint32_t)g[a] << 24) | ((uint32_t)g[(a+1)&0xFFFF] << 16) | ((uint32_t)g[(a+2)&0xFFFF] << 8) | g[(a+3)&0xFFFF]; }
void m68k_write8(uint32_t a, uint8_t v){ g[a & 0xFFFF] = v; }
void m68k_write16(uint32_t a, uint16_t v){ a &= 0xFFFF; g[a] = (uint8_t)(v >> 8); g[(a+1)&0xFFFF] = (uint8_t)v; }
void m68k_write32(uint32_t a, uint32_t v){ a &= 0xFFFF;
  g[a] = (uint8_t)(v >> 24); g[(a+1)&0xFFFF] = (uint8_t)(v >> 16); g[(a+2)&0xFFFF] = (uint8_t)(v >> 8); g[(a+3)&0xFFFF] = (uint8_t)v; }

#include "parity_run.h"

#define PARITY_TRACE_STEPS 64

int main(int argc, char **argv){
  if (argc > 1) {                     /* trace mode: per-step cumulative hash */
    uint32_t h = PARITY_FNV_INIT; int i;
    parity_setup();
    for (i = 0; i < PARITY_TRACE_STEPS; i++) { m68k_step(); h = parity_fold_state(h); printf("0x%08Xul,\n", h); }
    return 0;
  }
  printf("%08X\n", parity_run());
  return 0;
}
