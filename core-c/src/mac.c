/* Macintosh 128K machine model — see mac.h. Enough of the memory map + VIA to
** run the ROM's power-on / RAM test / hardware init; IWM and SCC are stubbed
** (no disk, no serial) and the VBL interrupt is delivered via the VIA. This is
** HLE-leaning: we model registers and the overlay, not chip-accurate timing. */
#include <string.h>
#include "mac.h"

static uint8_t        ram[MAC_RAM_SIZE];
static const uint8_t *rom;                 /* 64 KB */
static int            overlay;             /* 1 at reset: ROM at $0 */
static uint32_t       vbl_count;
static uint32_t       instr_count;

/* VIA 6522 (minimal): port A/B, DDRs, interrupt flag/enable, two timers */
static uint8_t via_orb, via_ora, via_ddrb, via_ddra;
static uint8_t via_ifr, via_ier;
static uint8_t via_acr, via_pcr, via_sr;
static uint16_t via_t1, via_t2;

/* VBL cadence: the real machine is 60.15 Hz (~130k CPU cycles). We don't count
** cycles, so approximate with an instruction count — exact rate doesn't affect
** boot correctness, only wall-clock timing. */
#define VBL_PERIOD 12000u

int      mac_overlay(void)   { return overlay; }
uint32_t mac_vbl_count(void) { return vbl_count; }
uint8_t *mac_ram_ptr(void)   { return ram; }

static void via_update_irq(void){
  /* VIA (CA1=VBL, etc.) is wired to 68000 IPL level 1 */
  m68kc.irq = (via_ifr & via_ier & 0x7F) ? 1 : 0;
}

/* ---- VIA register access (reg = address bits A9-A12) ---- */
static uint8_t via_read(uint32_t a){
  int reg = (int)((a >> 9) & 0xF);
  switch (reg) {
    case 0:  return via_orb;
    case 1:  case 15:                       /* IRA: PA4=overlay reads back its latch */
             return via_ora;
    case 2:  return via_ddrb;
    case 3:  return via_ddra;
    case 4:  via_ifr &= (uint8_t)~0x40; via_update_irq(); return (uint8_t)via_t2; /* T1C-L: clears T1 flag */
    case 5:  return (uint8_t)(via_t1 >> 8);
    case 8:  via_ifr &= (uint8_t)~0x20; via_update_irq(); return (uint8_t)via_t2; /* T2C-L: clears T2 flag */
    case 9:  return (uint8_t)(via_t2 >> 8);
    case 10: return via_sr;
    case 11: return via_acr;
    case 12: return via_pcr;
    case 13: return (uint8_t)(via_ifr | ((via_ifr & via_ier & 0x7F) ? 0x80 : 0)); /* IFR (bit7=any) */
    case 14: return (uint8_t)(via_ier | 0x80);                                     /* IER (bit7 reads 1) */
    default: return 0;
  }
}
static void via_write(uint32_t a, uint8_t v){
  int reg = (int)((a >> 9) & 0xF);
  switch (reg) {
    case 0:  via_orb = v; break;
    case 1:  case 15:                       /* ORA — bit 4 is the ROM overlay */
             via_ora = v;
             if ((via_ddra & 0x10) && !(v & 0x10)) overlay = 0;   /* driven low => overlay off */
             break;
    case 2:  via_ddrb = v; break;
    case 3:  via_ddra = v; break;
    case 11: via_acr = v; break;
    case 12: via_pcr = v; break;
    case 13: via_ifr &= (uint8_t)~(v & 0x7F); via_update_irq(); break;   /* IFR: write-1-to-clear */
    case 14: if (v & 0x80) via_ier |= (v & 0x7F); else via_ier &= (uint8_t)~(v & 0x7F);  /* IER set/clear */
             via_update_irq(); break;
    default: break;
  }
}

/* ---- IWM (floppy) — track the state-machine lines (ca0..q7, selected by
** address bits A9-A12: line = reg>>1, on = reg&1) and model the mode register
** so the ROM's write/read-back probe passes. No disk (sense=0, no data). ---- */
static uint8_t iwm_lines, iwm_mode;
static uint8_t iwm_access(uint32_t a, int is_write, uint8_t wv){
  int reg = (int)((a >> 9) & 0xF);
  if (reg & 1) iwm_lines |= (uint8_t)(1u << (reg >> 1)); else iwm_lines &= (uint8_t)~(1u << (reg >> 1));
  { int q6 = (iwm_lines >> 6) & 1, q7 = (iwm_lines >> 7) & 1, en = (iwm_lines >> 4) & 1;
    if (is_write) { if (q7 && q6 && !en) iwm_mode = wv; return 0; }   /* write mode reg (drive off) */
    if (q6 && !q7) return (uint8_t)(iwm_mode & 0x1F);                 /* status: mode bits, sense=0 */
    return 0x00; }                                                    /* data / handshake: no disk */
}
static uint8_t iwm_read(uint32_t a){ return iwm_access(a, 0, 0); }
static void    iwm_write(uint32_t a, uint8_t v){ (void)iwm_access(a, 1, v); }
static uint8_t scc_read(uint32_t a){ (void)a; return 0x00; }

/* ---- 24-bit address decode ---- */
static uint8_t bus_read(uint32_t a){
  a &= 0xFFFFFF;
  if (a < 0x400000) return overlay ? rom[a & 0xFFFF] : ram[a & 0x1FFFF];
  if (a < 0x500000) return rom[a & 0xFFFF];                        /* ROM $400000 */
  if (a >= 0x600000 && a < 0x800000) return overlay ? ram[a & 0x1FFFF] : 0xFF;
  if (a >= 0x800000 && a < 0xA00000) return scc_read(a);
  if (a >= 0xC00000 && a < 0xE00000) return iwm_read(a);
  if (a >= 0xE80000) return via_read(a);
  return 0xFF;
}
static void bus_write(uint32_t a, uint8_t v){
  a &= 0xFFFFFF;
  if (a < 0x400000) { if (!overlay) ram[a & 0x1FFFF] = v; return; }
  if (a >= 0x600000 && a < 0x800000) { if (overlay) ram[a & 0x1FFFF] = v; return; }
  if (a >= 0xC00000 && a < 0xE00000) { iwm_write(a, v); return; }
  if (a >= 0xE80000) { via_write(a, v); return; }
  /* ROM / SCC writes: ignore for now */
}

/* ---- m68k memory interface (big-endian) ---- */
uint8_t  m68k_read8(uint32_t a){ return bus_read(a); }
uint16_t m68k_read16(uint32_t a){ return (uint16_t)((bus_read(a) << 8) | bus_read(a + 1)); }
uint32_t m68k_read32(uint32_t a){
  return ((uint32_t)bus_read(a) << 24) | ((uint32_t)bus_read(a+1) << 16) | ((uint32_t)bus_read(a+2) << 8) | bus_read(a+3); }
void m68k_write8(uint32_t a, uint8_t v){ bus_write(a, v); }
void m68k_write16(uint32_t a, uint16_t v){ bus_write(a, (uint8_t)(v >> 8)); bus_write(a + 1, (uint8_t)v); }
void m68k_write32(uint32_t a, uint32_t v){
  bus_write(a, (uint8_t)(v >> 24)); bus_write(a+1, (uint8_t)(v >> 16)); bus_write(a+2, (uint8_t)(v >> 8)); bus_write(a+3, (uint8_t)v); }

void mac_init(const uint8_t *rom_){
  rom = rom_;
  memset(ram, 0, sizeof ram);
  overlay = 1;
  via_orb = via_ora = via_ddrb = via_ddra = 0;
  via_ifr = via_ier = via_acr = via_pcr = via_sr = 0;
  via_t1 = via_t2 = 0xFFFF;
  vbl_count = instr_count = 0;
  memset(&m68kc, 0, sizeof m68kc);
  m68kc.a[7] = m68k_read32(0);      /* overlay on => reads ROM: SP=checksum */
  m68kc.pc = m68k_read32(4);        /* PC = $0040002A */
  m68kc.isp = m68kc.a[7];
  m68kc.sr = 0x2700;                /* supervisor, IRQ mask 7, trace off */
}

void mac_step(void){
  m68k_step();
  instr_count++;
  via_t1 = (uint16_t)(via_t1 - 1);   /* rough timer motion so poll loops progress */
  via_t2 = (uint16_t)(via_t2 - 1);
  if (instr_count % VBL_PERIOD == 0) {
    via_ifr |= 0x02;                 /* CA1 = vertical blank */
    vbl_count++;
    via_update_irq();
  }
}
