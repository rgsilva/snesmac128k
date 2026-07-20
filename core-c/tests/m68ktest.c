/* Host test harness around Musashi for the SingleStepTests/m68000 vectors.
** Built into a dylib and driven from run_vectors.py via ctypes — the same
** shape as snes86's core-c/tests + libem86.dylib.
**
** Musashi is configured (m68kconf.h defaults) with PREFETCH off, so the core
** fetches each opcode straight from g_mem at PC (no stale prefetch queue to
** keep in sync with the vectors' 2-word queue). g_mem is a flat 16 MB image
** matching the 68000's 24-bit bus; all access is big-endian. */
#include <string.h>
#include <stdint.h>
#include "m68k.h"

/* Defined in m68kglue.c (isolated so m68kcpu.h's internal macros don't leak in
** here): clears the m68ki_cpu.stopped flag left by STOP / double-faults. */
extern void tm_clear_stopped(void);

static uint8_t g_mem[0x1000000]; /* 16 MB, 24-bit address space */
#define MASK 0xFFFFFFu

/* Log of every byte address written during a step, so the test runner can zero
** exactly what changed — including stray writes NOT in the vector's final RAM
** (e.g. an odd-address write that would fault on real HW). Without this, such
** writes leak across tests in a long single-process run and corrupt later ones.
** Worst case per instruction is MOVEM (16 regs * 4 bytes) + a stack frame. */
static unsigned int g_wlog[256];
static int g_wn;
static void wlog(unsigned int a){ if (g_wn < 256) g_wlog[g_wn++] = a & MASK; }

/* ---- big-endian memory interface Musashi calls ---- */
unsigned int m68k_read_memory_8(unsigned int a){ return g_mem[a & MASK]; }
unsigned int m68k_read_memory_16(unsigned int a){ a&=MASK; return ((unsigned)g_mem[a]<<8)|g_mem[(a+1)&MASK]; }
unsigned int m68k_read_memory_32(unsigned int a){ a&=MASK;
  return ((unsigned)g_mem[a]<<24)|((unsigned)g_mem[(a+1)&MASK]<<16)|((unsigned)g_mem[(a+2)&MASK]<<8)|g_mem[(a+3)&MASK]; }
void m68k_write_memory_8(unsigned int a, unsigned int v){ a&=MASK; g_mem[a]=(uint8_t)v; wlog(a); }
void m68k_write_memory_16(unsigned int a, unsigned int v){ a&=MASK; g_mem[a]=(uint8_t)(v>>8); g_mem[(a+1)&MASK]=(uint8_t)v; wlog(a); wlog(a+1); }
void m68k_write_memory_32(unsigned int a, unsigned int v){ a&=MASK;
  g_mem[a]=(uint8_t)(v>>24); g_mem[(a+1)&MASK]=(uint8_t)(v>>16); g_mem[(a+2)&MASK]=(uint8_t)(v>>8); g_mem[(a+3)&MASK]=(uint8_t)v;
  wlog(a); wlog(a+1); wlog(a+2); wlog(a+3); }

/* SEPARATE_READS is off, but define these so the disassembler and any stray
** reference link cleanly; they just alias the plain reads. */
unsigned int m68k_read_immediate_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a){ return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a){ return m68k_read_memory_8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a){ return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a){ return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return m68k_read_memory_32(a); }

/* ---- exported control API for the Python runner ---- */
void tm_init(void){
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_pulse_reset();
  m68k_execute(1);   /* consume the ~40-cycle reset sequence so real steps run one instr each */
}
/* Per-test clean slate: a full pulse_reset would burn the next execute on the
** reset sequence (PC wouldn't advance), so instead just clear the stopped/
** halted state that STOP / double-faults leave behind. All registers are
** overwritten by the runner afterwards. */
void tm_reset_cpu(void){ tm_clear_stopped(); }
void tm_clear_mem(void){ memset(g_mem, 0, sizeof g_mem); }
void tm_w8(unsigned int a, unsigned int v){ g_mem[a & MASK] = (uint8_t)v; }
unsigned int tm_r8(unsigned int a){ return g_mem[a & MASK]; }
void tm_set(int reg, unsigned int v){ m68k_set_reg((m68k_register_t)reg, v); }
unsigned int tm_get(int reg){ return m68k_get_reg((void*)0, (m68k_register_t)reg); }
int tm_step(void){ return m68k_execute(1); }
unsigned int tm_disasm(char *buf, unsigned int pc){ return m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000); }

/* Whole-test runner in one FFI call (the ctypes-per-op path is ~40 calls/test
** and dominates runtime). State arrays are 19 words:
**   [0..7]=d0..d7  [8..14]=a0..a6  [15]=usp [16]=ssp [17]=sr [18]=pc
** ram is parallel addr[]/val[]. ccr_mask = CCR bits (low 5) that must match;
** the upper SR byte (T,S,I) is always compared. Returns a bitmask of what
** diverged: 1=regs 2=usp/ssp 4=pc 8=sr 16=memory. */
enum { TR_USP=15, TR_SSP=16, TR_SR=17, TR_PC=18 };
int tm_run1(const unsigned int *ini, const unsigned int *fin,
            const unsigned int *ira, const unsigned char *irv, int ni,
            const unsigned int *fra, const unsigned char *frv, int nf,
            unsigned int ccr_mask, unsigned int pc_off)
{
  int i, r = 0; unsigned int sr, fsr, m, gusp, gssp;
  tm_clear_stopped();
  for (i = 0; i < 8; i++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), ini[i]);
  for (i = 0; i < 7; i++) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), ini[8 + i]);
  sr = ini[TR_SR];
  m68k_set_reg(M68K_REG_SR, sr);
  if (sr & 0x2000) { m68k_set_reg(M68K_REG_A7, ini[TR_SSP]); m68k_set_reg(M68K_REG_USP, ini[TR_USP]); }
  else             { m68k_set_reg(M68K_REG_A7, ini[TR_USP]); m68k_set_reg(M68K_REG_ISP, ini[TR_SSP]); }
  m68k_set_reg(M68K_REG_PC, (ini[TR_PC] - 4) & MASK);
  for (i = 0; i < ni; i++) g_mem[ira[i] & MASK] = irv[i];

  g_wn = 0;
  m68k_execute(1);

  for (i = 0; i < 8; i++)
    if (m68k_get_reg((void*)0, (m68k_register_t)(M68K_REG_D0 + i)) != fin[i]) { r |= 1; break; }
  for (i = 0; i < 7; i++)
    if ((m68k_get_reg((void*)0, (m68k_register_t)(M68K_REG_A0 + i)) & MASK) != (fin[8 + i] & MASK)) { r |= 1; break; }
  fsr = m68k_get_reg((void*)0, M68K_REG_SR);
  if (fsr & 0x2000) { gssp = m68k_get_reg((void*)0, M68K_REG_A7) & MASK; gusp = m68k_get_reg((void*)0, M68K_REG_USP) & MASK; }
  else              { gusp = m68k_get_reg((void*)0, M68K_REG_A7) & MASK; gssp = m68k_get_reg((void*)0, M68K_REG_ISP) & MASK; }
  if (gusp != (fin[TR_USP] & MASK) || gssp != (fin[TR_SSP] & MASK)) r |= 2;
  if (((m68k_get_reg((void*)0, M68K_REG_PC) + pc_off) & MASK) != (fin[TR_PC] & MASK)) r |= 4;
  m = 0xFF00u | (ccr_mask & 0x1F);
  if ((fsr & m) != (fin[TR_SR] & m)) r |= 8;
  for (i = 0; i < nf; i++) if (g_mem[fra[i] & MASK] != frv[i]) { r |= 16; break; }

  /* zero everything that changed: my initial-RAM writes + every CPU write
  ** (g_wlog catches stray writes not in the vector's final RAM) */
  for (i = 0; i < ni; i++) g_mem[ira[i] & MASK] = 0;
  for (i = 0; i < nf; i++) g_mem[fra[i] & MASK] = 0;
  for (i = 0; i < g_wn; i++) g_mem[g_wlog[i]] = 0;
  return r;
}
