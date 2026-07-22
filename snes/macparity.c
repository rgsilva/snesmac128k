/* M4b step 1: run the full Mac 128K machine model (mac.c + m68k.c) on the SNES
** and check it matches the host for the first MACP_N steps — the M2 parity idea
** extended from a directed stream to the real ROM boot. RAM is cart SRAM, ROM
** is embedded in the cart (macmem.c); no floppy yet (the early boot doesn't read
** it). Proves the machine model + the SRAM/ROM memory backend are faithful on
** Calypsi before attempting the (very slow) full boot-to-desktop.
**
** Two builds from this one source (the reference is the SAME host hash either
** way — macparity_ref.h):
**   default    steps via mac_step()  -> the portable C core (validates the
**              Calypsi-compiled core: the -O2 correctness baseline).
**   -DMACP_HOT steps via mac_run(1)  -> the hand-65816 inner loop
**              (calypsi/m68krun.s): validates the ASM native handlers against
**              real boot code. The bench only exercises DBRA/BRA + long
**              register ALU, so it cannot cover conditional Bcc, ADDQ #d,An,
**              etc.; this can. Same VBL/timer storage, so the two stay in
**              lockstep and the host hash still applies.
**
** NB: every accessor MUST be declared — an implicit (C89) declaration makes
** Calypsi pass args with the 16-bit-int ABI, silently corrupting them. */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "mac.h"
#include "macmem.h"
#include "macparity_hash.h"
#include "macparity_ref.h"      /* MACP_N, MACP_CPUHASH, MACP_RAMHASH (host) */

#define REG8(a) (*(volatile uint8_t *)(unsigned long)(a))

uint8_t mac_ram_rd(uint32_t a);
void    mac_ram_wr(uint32_t a, uint8_t v);
uint8_t mac_rom_rd(uint32_t a);
void    mac_run(uint16_t n);            /* asm hot loop (calypsi/m68krun.s) */

int main(void){
  uint32_t h = MACP_FNV_INIT, rh;
  uint32_t i;
  mb_init();
#ifdef MACP_HOT
  mb_puts("macparity-hot: asm inner loop (m68k_run_hot) vs host\n");
#else
  mb_puts("macparity: Mac machine (C core) on SNES vs host\n");
#endif
  REG8(0x420D) = 0x01;                  /* MEMSEL: FastROM (map mode $31) */

  /* SRAM round-trip sanity (two spread-out Mac RAM cells). */
  mac_ram_wr(0x00100, 0x5A); mac_ram_wr(0x1FE00, 0xA5);
  mb_puts("sram "); mb_puthex32(((uint32_t)mac_ram_rd(0x00100) << 8) | mac_ram_rd(0x1FE00));
  mb_putc('\n');

  mac_init(0);                          /* ROM in cart, RAM (SRAM) cleared */
  mb_puts("init\n");
  for (i = 0; i < MACP_N; i++) {
#ifdef MACP_HOT
    mac_run(1);                         /* the hand-65816 inner loop */
#else
    mac_step();                         /* the portable C core */
#endif
    h = macp_hash_cpu(h);
    if ((i & 0xFF) == 0xFF) { mb_puts("@"); mb_puthex32(i + 1); mb_putc('\n'); }
  }

  rh = MACP_FNV_INIT;
  for (i = 0; i < 0x20000u; i += MACP_RAM_STRIDE) rh = macp_fnv1(rh, mac_ram_rd(i));

  mb_puts("cpu "); mb_puthex32(h); mb_puts(" ram "); mb_puthex32(rh); mb_putc('\n');

  if (h == MACP_CPUHASH && rh == MACP_RAMHASH) {
    mb_puts("PARITY OK\n");
    mb_finish(MB_STATUS_PASS);
  } else {
    if (h != MACP_CPUHASH) { mb_puts("CPU host="); mb_puthex32(MACP_CPUHASH); mb_putc('\n'); }
    if (rh != MACP_RAMHASH) { mb_puts("RAM host="); mb_puthex32(MACP_RAMHASH); mb_putc('\n'); }
    mb_finish(2);
  }
  return 0;
}
