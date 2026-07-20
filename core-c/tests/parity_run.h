/* Shared M2 parity runner: identical on host and SNES so it can't itself be a
** source of divergence. Clears the 68000's 64 KB space, loads the directed
** program, sets the canonical state, single-steps PARITY_NSTEPS times folding
** the full register file into an FNV-1a hash each step, then folds the data
** region. Uses only m68k_step + the memory hooks, so the backing (host array
** vs WRAM) is the only per-platform difference. */
#ifndef PARITY_RUN_H
#define PARITY_RUN_H
#include "m68k.h"
#include "parity_stream.h"

static uint32_t p_fnv(uint32_t h, uint8_t b){ return (h ^ b) * 16777619ul; }
static uint32_t p_fold(uint32_t h, uint32_t v){
  h = p_fnv(h, (uint8_t)(v >> 24)); h = p_fnv(h, (uint8_t)(v >> 16));
  h = p_fnv(h, (uint8_t)(v >> 8));  h = p_fnv(h, (uint8_t)v);
  return h;
}

#define PARITY_FNV_INIT 2166136261ul

static void parity_setup(void){
  uint32_t addr; int r;
  /* clear 64 KB — 32-bit counter: Calypsi `int` is 16-bit and 0x10000 overflows it */
  for (addr = 0; addr < 0x10000ul; addr += 4) m68k_write32(addr, 0);
  for (r = 0; r < PARITY_PROG_WORDS; r++)
    m68k_write16(PARITY_PROG_ADDR + (uint32_t)(r * 2), parity_prog[r]);
  for (r = 0; r < 8; r++) { m68kc.d[r] = parity_init_d[r]; m68kc.a[r] = parity_init_a[r]; }
  m68kc.pc = PARITY_PROG_ADDR;
  m68kc.sr = PARITY_INIT_SR;
  m68kc.usp = 0; m68kc.isp = 0; m68kc.stopped = 0;
}

/* fold the whole register file into the running hash */
static uint32_t parity_fold_state(uint32_t h){
  int r;
  for (r = 0; r < 8; r++) h = p_fold(h, m68kc.d[r]);
  for (r = 0; r < 8; r++) h = p_fold(h, m68kc.a[r]);
  h = p_fold(h, m68kc.pc);
  h = p_fold(h, (uint32_t)m68kc.sr);
  return h;
}

static uint32_t parity_run(void){
  uint32_t h = PARITY_FNV_INIT, addr;
  int i;
  parity_setup();
  for (i = 0; i < PARITY_NSTEPS; i++) { m68k_step(); h = parity_fold_state(h); }
  for (addr = 0x2000; addr < 0x2400; addr++) h = p_fnv(h, m68k_read8(addr));
  return h;
}

#endif
