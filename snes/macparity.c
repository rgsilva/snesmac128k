/* M4b step 1: run the full Mac 128K machine model (mac.c + m68k.c) on the SNES
** and check it matches the host for the first MACP_N steps — the M2 parity idea
** extended from a directed stream to the real ROM boot. RAM is cart SRAM, ROM
** is embedded in the cart (macmem.c); no floppy yet (the early boot doesn't read
** it). Proves the machine model + the SRAM/ROM memory backend are faithful on
** Calypsi before attempting the (very slow) full boot-to-desktop.
**
** NB: every accessor MUST be declared — an implicit (C89) declaration makes
** Calypsi pass args with the 16-bit-int ABI, silently corrupting them. */
#include <stdint.h>
#include "calypsi/mailbox.h"
#include "mac.h"
#include "macparity_hash.h"
#include "macparity_ref.h"      /* MACP_N, MACP_CPUHASH, MACP_RAMHASH (host) */

uint8_t mac_ram_rd(uint32_t a);
void    mac_ram_wr(uint32_t a, uint8_t v);
uint8_t mac_rom_rd(uint32_t a);

int main(void){
  uint32_t h = MACP_FNV_INIT, rh;
  uint32_t i;
  mb_init();
  mb_puts("macparity: Mac machine on SNES vs host\n");

  /* SRAM round-trip sanity (two spread-out Mac RAM cells). */
  mac_ram_wr(0x00100, 0x5A); mac_ram_wr(0x1FE00, 0xA5);
  mb_puts("sram "); mb_puthex32(((uint32_t)mac_ram_rd(0x00100) << 8) | mac_ram_rd(0x1FE00));
  mb_putc('\n');

  mac_init(0);                          /* ROM in cart, RAM (SRAM) cleared */
  mb_puts("init\n");
  for (i = 0; i < MACP_N; i++) {
    mac_step(); h = macp_hash_cpu(h);
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
