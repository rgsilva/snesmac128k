/* M2 parity ROM: runs the lean 68000 core (compiled by Calypsi) over the same
** directed stream as the host and compares its per-step cumulative hash to the
** host reference baked in at build time (parity_ref.h). The comparison is done
** in C — only the first-mismatch step + hashes are mailed (a few lines, which
** survive Mesen's stdout on exit; streaming 64 lines did not). The emulated
** 68000's 64 KB memory is WRAM bank $7F (plain far pointer, --data-model=large,
** the snespico8 pattern). */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "m68k.h"

static uint8_t *const M68 = (uint8_t *)0x7F0000ul;   /* 68000 memory = WRAM $7F */
uint8_t  m68k_read8(uint32_t a){ return M68[a & 0xFFFF]; }
uint16_t m68k_read16(uint32_t a){ a &= 0xFFFF; return ((uint16_t)M68[a] << 8) | M68[(a + 1) & 0xFFFF]; }
uint16_t m68k_fetch16(uint32_t a){ return m68k_read16(a); }
uint32_t m68k_read32(uint32_t a){ a &= 0xFFFF;
  return ((uint32_t)M68[a] << 24) | ((uint32_t)M68[(a+1)&0xFFFF] << 16) | ((uint32_t)M68[(a+2)&0xFFFF] << 8) | M68[(a+3)&0xFFFF]; }
void m68k_write8(uint32_t a, uint8_t v){ M68[a & 0xFFFF] = v; }
void m68k_write16(uint32_t a, uint16_t v){ a &= 0xFFFF; M68[a] = (uint8_t)(v >> 8); M68[(a+1)&0xFFFF] = (uint8_t)v; }
void m68k_write32(uint32_t a, uint32_t v){ a &= 0xFFFF;
  M68[a] = (uint8_t)(v >> 24); M68[(a+1)&0xFFFF] = (uint8_t)(v >> 16); M68[(a+2)&0xFFFF] = (uint8_t)(v >> 8); M68[(a+3)&0xFFFF] = (uint8_t)v; }

#include "parity_run.h"
#include "parity_ref.h"    /* PARITY_REF_TRACE[] + PARITY_TRACE_N, from the host build */

int main(void){
  uint32_t h = PARITY_FNV_INIT;
  int i, bad = -1;
  uint32_t bad_snes = 0, bad_host = 0;
  mb_init();
  mb_puts("m2 parity: 68000 core on SNES vs host\n");
  parity_setup();
  for (i = 0; i < PARITY_TRACE_N; i++) {
    m68k_step();
    h = parity_fold_state(h);
    if (bad < 0 && h != PARITY_REF_TRACE[i]) { bad = i; bad_snes = h; bad_host = PARITY_REF_TRACE[i]; }
  }
  if (bad < 0) {
    mb_puts("PARITY OK\n");
    mb_finish(MB_STATUS_PASS);
  } else {
    mb_puts("MISMATCH step="); mb_puthex32((uint32_t)bad);
    mb_puts(" snes="); mb_puthex32(bad_snes);
    mb_puts(" host="); mb_puthex32(bad_host); mb_putc('\n');
    mb_finish(2);
  }
  return 0;
}
