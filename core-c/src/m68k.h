/* Lean, Calypsi-friendly Motorola 68000 core.
**
** The two things that made Musashi un-compilable on Calypsi are avoided by
** construction: 32-bit values are uint32_t (never `unsigned int`, which is
** 16-bit on Calypsi), and dispatch is a bitfield switch, never a 64K table.
**
** Same source compiles on the host (validated against SingleStepTests/m68000
** and, differentially, against Musashi) and for the SNES via Calypsi. */
#ifndef M68K_H
#define M68K_H

#include <stdint.h>

typedef struct {
  uint32_t d[8];
  uint32_t a[8];     /* a[7] is the active stack pointer */
  uint32_t pc;
  uint32_t usp;      /* inactive user   SP (valid while in supervisor mode) */
  uint32_t isp;      /* inactive superv SP (valid while in user mode) */
  uint16_t sr;       /* T-S--III---XNZVC */
  uint8_t  stopped;  /* set by STOP, cleared by reset/irq */
  uint8_t  irq;      /* pending autovector IRQ level 0-7 (set by the machine) */
} m68k_t;

extern m68k_t m68kc;

/* big-endian memory interface, provided by the embedder (24-bit address bus) */
extern uint8_t  m68k_read8(uint32_t addr);
extern uint16_t m68k_read16(uint32_t addr);
extern uint32_t m68k_read32(uint32_t addr);
extern void     m68k_write8(uint32_t addr, uint8_t v);
extern void     m68k_write16(uint32_t addr, uint16_t v);
extern void     m68k_write32(uint32_t addr, uint32_t v);

void m68k_set_sr(uint16_t val);  /* write SR with correct A7 bank swap */
void m68k_step(void);            /* execute one instruction / take a pending trap */

/* SR / CCR bits */
#define SR_C 0x0001u
#define SR_V 0x0002u
#define SR_Z 0x0004u
#define SR_N 0x0008u
#define SR_X 0x0010u
#define SR_I 0x0700u
#define SR_S 0x2000u
#define SR_T 0x8000u
#define SR_VALID 0xA71Fu  /* bits actually implemented on the 68000 */

/* exception vector numbers */
#define EXC_ILLEGAL     4
#define EXC_ZERODIV     5
#define EXC_CHK         6
#define EXC_TRAPV       7
#define EXC_PRIV        8
#define EXC_TRACE       9
#define EXC_LINEA      10
#define EXC_LINEF      11
#define EXC_TRAP0      32

#endif
