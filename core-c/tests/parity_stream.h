/* M2 parity: a directed 68000 program + canonical initial state, executed by
** the same m68k.c on host and SNES. Exercises the ALU/move/shift/rotate/bit/
** MOVEM/branch paths (and 32-bit math with carries) to stress the Calypsi
** codegen. Program is stored as big-endian words; parity_run loads them via
** m68k_write16. It ends in BRA-to-self, so any step count past completion just
** spins on a stable state — the final hash is deterministic on both sides.
**
** Determinism, not "meaningfulness", is what parity needs: both cores execute
** identical bytes for identical steps, so a hash mismatch == a Calypsi bug. */
#ifndef PARITY_STREAM_H
#define PARITY_STREAM_H
#include <stdint.h>

#define PARITY_PROG_ADDR 0x1000u
#define PARITY_INIT_SR   0x2700u   /* supervisor, IRQ mask 7, trace off */
#define PARITY_NSTEPS    400

static const uint16_t parity_prog[] = {
  0x7007,               /* moveq  #7,d0        */
  0x760A,               /* moveq  #10,d3       */
  0xD680,               /* add.l  d0,d3        */
  0xE588,               /* lsl.l  #2,d0        */
  0x0C00, 0x0005,       /* cmpi.b #5,d0        */
  0x6600, 0x0004,       /* bne.w  +4           */
  0x4E71,               /* nop                 */
  0x4E71,               /* nop                 */
  0x2200,               /* move.l d0,d1        */
  0xD3C0,               /* adda.l d0,a1        */
  0x3080,               /* move.w d0,(a0)      */
  0x3010,               /* move.w (a0),d0      */
  0x5340,               /* subq.w #1,d0        */
  0xB280,               /* cmp.l  d0,d1        */
  0x4840,               /* swap   d0           */
  0x4880,               /* ext.w  d0           */
  0x48E7, 0x8080,       /* movem.l d0/a0,-(a7) */
  0x4CDF, 0x0101,       /* movem.l (a7)+,d0/a0 */
  0xE318,               /* rol.b  #1,d0        */
  0xE290,               /* roxr.l #1,d0        */
  0x0840, 0x0003,       /* bchg   #3,d0        */
  0x7403,               /* moveq  #3,d2        */
  0xD640,               /* add.w  d0,d3 (loop) */
  0x51CA, 0xFFFC,       /* dbf    d2,loop      */
  0x4A80,               /* tst.l  d0           */
  0x4680,               /* not.l  d0           */
  0x4480,               /* neg.l  d0           */
  0x0280, 0x0000, 0x00FF, /* andi.l #0xFF,d0   */
  0xE19C,               /* rol.l  #8,d4        */
  0xD844,               /* add.w  d4,d4        */
  0x2F00,               /* move.l d0,-(a7)     */
  0x201F,               /* move.l (a7)+,d0     */
  0x7AFB,               /* moveq  #-5,d5       (negative imm -> sxt8) */
  0x41F0, 0x00FC,       /* lea    (-4,a0,d0.w),a0  (negative disp8 index -> sxt8) */
  0xE485,               /* asr.l  #2,d5        (sign-preserving shift) */
  0x60FE                /* bra.s  *  (spin)    */
};
#define PARITY_PROG_WORDS (int)(sizeof parity_prog / sizeof parity_prog[0])

static const uint32_t parity_init_d[8] = {
  0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
  0x0F0F0F0Fu, 0x66666666u, 0x77777777u, 0x88888888u
};
static const uint32_t parity_init_a[8] = {
  0x00002000u, 0x00002100u, 0x00002200u, 0x00002300u,
  0x00002400u, 0x00002500u, 0x00002600u, 0x00008000u   /* a7 = SP */
};

#endif
