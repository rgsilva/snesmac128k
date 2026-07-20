/* M0: prove the snesmac Calypsi build plumbing (linker.scm + header.s +
** mailbox + raw2sfc + Mesen harness) works before dropping the 68000 core in.
** Same mailbox protocol as snespico8's hello.c / hello.lua. */
#include <stdint.h>
#include "calypsi/mailbox.h"

int main(void)
{
  mb_init();
  mb_puts("snesmac M0: calypsi C alive\n");

  /* far WRAM sweep ($7E above the linker's far sections) — the Mac's RAM will
  ** live in banked SRAM, but this just proves 24-bit far pointers work. */
  volatile __far uint8_t *wram = (volatile __far uint8_t *)0x7ea000;
  for (unsigned i = 0; i < 4096; i++)
    wram[i] = (uint8_t)(i * 7 + 3);
  for (unsigned i = 0; i < 4096; i++)
    if (wram[i] != (uint8_t)(i * 7 + 3)) {
      mb_puts("far FAIL\n");
      mb_finish(7);
    }
  mb_puts("far ok\n");

  mb_puts("M0 PASS\n");
  mb_finish(MB_STATUS_PASS);
  return 0;
}
