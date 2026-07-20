/* Host driver: boot a real Macintosh 128K ROM through the machine model and
** trace how far it gets (like snes86's runpc). Reports the reset vector, the
** overlay switch, PC trajectory samples, and stuck-loop / settle detection. */
#include <stdio.h>
#include <stdlib.h>
#include "mac.h"

int main(int argc, char **argv){
  const char *path = argc > 1 ? argv[1] : "roms/mac128k.rom";
  long steps = argc > 2 ? atol(argv[2]) : 30000000L;
  static uint8_t rom[65536];
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
  size_t n = fread(rom, 1, 65536, f); fclose(f);
  if (n != 65536) { fprintf(stderr, "ROM is %zu bytes, expected 65536\n", n); return 1; }

  mac_init(rom);
  printf("reset: SP=%08X PC=%08X\n", m68kc.a[7], m68kc.pc);

  int overlay_switched = 0;
  uint32_t pcmin = 0xFFFFFFFFu, pcmax = 0;
  uint32_t last_pc = 0; long same = 0;
  /* settle detection: track the PC window over the last N steps */
  uint32_t win_lo = 0xFFFFFFFFu, win_hi = 0; long win_start = 0;

  for (long i = 0; i < steps; i++) {
    uint32_t pc = m68kc.pc;
    if (i < 24)
      printf("  step %2ld: pc=%06X op=%04X sp=%06X sr=%04X d0=%08X a0=%06X\n",
             i, pc, m68k_read16(pc), m68kc.a[7], m68kc.sr, m68kc.d[0], m68kc.a[0]);
    if (!overlay_switched && !mac_overlay()) {
      overlay_switched = 1;
      printf("[%ld] OVERLAY OFF — RAM mapped at $0. pc=%06X sp=%06X\n", i, pc, m68kc.a[7]);
    }
    if (pc < pcmin) pcmin = pc;
    if (pc > pcmax) pcmax = pc;
    if (pc < win_lo) win_lo = pc;
    if (pc > win_hi) win_hi = pc;

    mac_step();

    if (m68kc.pc == pc) { if (++same == 200000) { printf("[%ld] STUCK single-instr loop at pc=%06X\n", i, pc); break; } }
    else same = 0;

    if ((i % 2000000) == 1999999) {
      printf("[%ld] pc-window last 2M steps: [%06X..%06X]  overlay=%d vbl=%u sp=%06X\n",
             i, win_lo, win_hi, mac_overlay(), mac_vbl_count(), m68kc.a[7]);
      /* if the window is tiny after real progress, we've settled into a loop */
      if (win_hi - win_lo < 0x400 && overlay_switched) {
        printf("[%ld] SETTLED into loop [%06X..%06X] (%u bytes) — likely a wait loop\n",
               i, win_lo, win_hi, win_hi - win_lo);
        break;
      }
      win_lo = 0xFFFFFFFFu; win_hi = 0; win_start = i;
    }
    (void)last_pc; (void)win_start;
  }
  printf("done. pc=%06X pc-range[%06X..%06X] overlay=%d vbl=%u\n",
         m68kc.pc, pcmin, pcmax, mac_overlay(), mac_vbl_count());

  /* render the Mac 128K screen buffer ($1A700, 512x342 1bpp, set bit = black) */
  {
    uint8_t *r = mac_ram_ptr();
    uint32_t base = 0x1A700u; long nz = 0;
    int x, y;
    for (x = 0; x < 21888; x++) if (r[base + x]) nz++;
    printf("\nframebuffer @ $%05X: %ld/%d non-zero bytes\n", base, nz, 21888);
    printf("center crop x[192,320) y[124,208), 1 char/pixel (set bit = '#'):\n");
    for (y = 124; y < 208; y++) {
      for (x = 192; x < 320; x++) {
        uint8_t b = r[base + y * 64 + (x >> 3)];
        putchar(((b >> (7 - (x & 7))) & 1) ? '#' : '.');
      }
      putchar('\n');
    }
  }
  return 0;
}
