/* Host harness for the lean 68000 core (core-c/src/m68k.c), exposing the SAME
** tm_run1 ABI as m68ktest.c (the Musashi harness). So run_vectors.py drives
** either one — point EM68K_LIB at build/libem68k_lean.dylib. This is the
** differential setup: the lean core is validated against the same vectors AND
** can be diffed against Musashi. */
#include <string.h>
#include <stdint.h>
#include "m68k.h"

static uint8_t g_mem[0x1000000];
#define MASK 0xFFFFFFu
static uint32_t g_wlog[256];
static int g_wn;
static void wlog(uint32_t a){ if (g_wn < 256) g_wlog[g_wn++] = a & MASK; }

uint8_t  m68k_read8(uint32_t a){ return g_mem[a & MASK]; }
uint16_t m68k_read16(uint32_t a){ a &= MASK; return ((uint16_t)g_mem[a] << 8) | g_mem[(a + 1) & MASK]; }
uint32_t m68k_read32(uint32_t a){ a &= MASK;
  return ((uint32_t)g_mem[a] << 24) | ((uint32_t)g_mem[(a+1)&MASK] << 16) | ((uint32_t)g_mem[(a+2)&MASK] << 8) | g_mem[(a+3)&MASK]; }
void m68k_write8(uint32_t a, uint8_t v){ a &= MASK; g_mem[a] = v; wlog(a); }
void m68k_write16(uint32_t a, uint16_t v){ a &= MASK; g_mem[a] = (uint8_t)(v >> 8); g_mem[(a+1)&MASK] = (uint8_t)v; wlog(a); wlog(a+1); }
void m68k_write32(uint32_t a, uint32_t v){ a &= MASK;
  g_mem[a] = (uint8_t)(v >> 24); g_mem[(a+1)&MASK] = (uint8_t)(v >> 16); g_mem[(a+2)&MASK] = (uint8_t)(v >> 8); g_mem[(a+3)&MASK] = (uint8_t)v;
  wlog(a); wlog(a+1); wlog(a+2); wlog(a+3); }

/* dump final state as 19 words (d0-7,a0-6,usp,ssp,sr,pc) for debugging diffs */
void tm_get_state(unsigned int *out){
  int i; unsigned int fsr = m68kc.sr;
  for (i = 0; i < 8; i++) out[i] = m68kc.d[i];
  for (i = 0; i < 7; i++) out[8 + i] = m68kc.a[i];
  if (fsr & SR_S) { out[16] = m68kc.a[7]; out[15] = m68kc.usp; }
  else            { out[15] = m68kc.a[7]; out[16] = m68kc.isp; }
  out[17] = fsr; out[18] = m68kc.pc;
}

void tm_init(void){ memset(&m68kc, 0, sizeof m68kc); }
void tm_reset_cpu(void){ m68kc.stopped = 0; }
void tm_w8(unsigned int a, unsigned int v){ g_mem[a & MASK] = (uint8_t)v; }
unsigned int tm_r8(unsigned int a){ return g_mem[a & MASK]; }

int tm_run1(const unsigned int *ini, const unsigned int *fin,
            const unsigned int *ira, const unsigned char *irv, int ni,
            const unsigned int *fra, const unsigned char *frv, int nf,
            unsigned int ccr_mask, unsigned int pc_off)
{
  int i, r = 0; unsigned int sr, fsr, m, gusp, gssp;
  m68kc.stopped = 0;
  for (i = 0; i < 8; i++) m68kc.d[i] = ini[i];
  for (i = 0; i < 7; i++) m68kc.a[i] = ini[8 + i];
  sr = ini[17];
  m68kc.sr = (uint16_t)(sr & SR_VALID);
  if (sr & SR_S) { m68kc.a[7] = ini[16]; m68kc.usp = ini[15]; m68kc.isp = ini[16]; }
  else           { m68kc.a[7] = ini[15]; m68kc.isp = ini[16]; m68kc.usp = ini[15]; }
  m68kc.pc = (ini[18] - 4) & MASK;
  for (i = 0; i < ni; i++) g_mem[ira[i] & MASK] = irv[i];

  g_wn = 0;
  m68k_step();

  for (i = 0; i < 8; i++) if (m68kc.d[i] != fin[i]) { r |= 1; break; }
  for (i = 0; i < 7; i++) if ((m68kc.a[i] & MASK) != (fin[8 + i] & MASK)) { r |= 1; break; }
  fsr = m68kc.sr;
  if (fsr & SR_S) { gssp = m68kc.a[7] & MASK; gusp = m68kc.usp & MASK; }
  else            { gusp = m68kc.a[7] & MASK; gssp = m68kc.isp & MASK; }
  if (gusp != (fin[15] & MASK) || gssp != (fin[16] & MASK)) r |= 2;
  if (((m68kc.pc + pc_off) & MASK) != (fin[18] & MASK)) r |= 4;
  m = 0xFF00u | (ccr_mask & 0x1F);
  if ((fsr & m) != (fin[17] & m)) r |= 8;
  for (i = 0; i < nf; i++) if (g_mem[fra[i] & MASK] != frv[i]) { r |= 16; break; }

  for (i = 0; i < ni; i++) g_mem[ira[i] & MASK] = 0;
  for (i = 0; i < nf; i++) g_mem[fra[i] & MASK] = 0;
  for (i = 0; i < g_wn; i++) g_mem[g_wlog[i]] = 0;
  return r;
}
