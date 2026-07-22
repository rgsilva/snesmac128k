/* Host runner for benchrom.bin (bench/mkbenchrom.py) — the golden reference.
** Boots our own 68000 ROM on the host build of the machine model, watches the
** phase word at RAM $0100, and prints per-phase dynamic instruction counts
** plus the final CRC from $0104. The SNES run (snes/macbench.c) must produce
** the same phase sequence and the SAME CRC; its per-phase frame counts (from
** test/macbench.lua) divided by these instruction counts are the
** cycles-per-instruction metric in docs/PERF.md.
**
** The disk is "inserted" (a dummy image) so the per-step .Sony trap guard
** runs, exactly as it does during a real boot — that guard is one of the
** measured costs. The bench ROM never executes an A-line trap, so the disk
** contents are never read. */
#include <stdio.h>
#include <stdlib.h>
#include "mac.h"

#define MAX_STEPS 100000ul

static const char *phase_name(unsigned m){
  static const char *names[] = {"?", "nop", "alu", "ram.b", "ram.w", "ram.l",
                                "shift", "muldiv", "copy", "fbfill", "crc", "trap"};
  if (m >= 1 && m <= 11) return names[m];
  if (m == 0xB0CA) return "done";
  return "?";
}

int main(int argc, char **argv){
  static uint8_t rom[MAC_ROM_SIZE];
  FILE *f; unsigned long steps, phase_start = 0; unsigned prev = 0;
  const uint8_t *ram;

  if (argc < 2) { fprintf(stderr, "usage: benchhost benchrom.bin\n"); return 2; }
  f = fopen(argv[1], "rb");
  if (!f || fread(rom, 1, MAC_ROM_SIZE, f) != MAC_ROM_SIZE) {
    fprintf(stderr, "cannot read %s\n", argv[1]); return 2;
  }
  fclose(f);

  mac_init(rom);
  mac_set_disk(rom, 409600);            /* dummy: arms the per-step trap guard */
  ram = mac_ram_ptr();

  for (steps = 0; steps < MAX_STEPS && !m68kc.stopped; steps++) {
    unsigned mark = (unsigned)(ram[0x100] << 8) | ram[0x101];
    if (mark != prev) {
      if (prev) printf("P %-6s %2u instr=%lu\n", phase_name(prev), prev, steps - phase_start);
      prev = mark; phase_start = steps;
    }
    mac_step();
  }
  if (!m68kc.stopped) { printf("RESULT FAIL bench never stopped\n"); return 1; }
  printf("CRC %02X%02X%02X%02X\n", ram[0x104], ram[0x105], ram[0x106], ram[0x107]);
  printf("TOTAL %lu\n", steps);
  return 0;
}
