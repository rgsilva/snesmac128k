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
/* optional trace hook (host debugging): (addr, write?, iwm lines, value) */
void (*mac_iwm_hook)(uint32_t a, int wr, unsigned lines, unsigned val) = 0;

/* Sony 400K drive: the ROM selects a drive register with ca0-2 + SEL and reads
** it on the status SENSE bit (bit 7); it steps the head with LSTRB strobes and
** reads GCR data via the data register. We model "disk present" + the sense
** lines and head position; GCR data streaming comes next. */
static int      disk_in = 1, drive_track = 0, motor_on = 0, step_dir = 0;
static uint16_t tach_ctr;
static uint8_t  iwm_lines, iwm_mode;

static int drive_sense(void){
  /* Sony read register = (CA2,CA1,CA0,SEL); SEL is VIA port A bit 5 (not the
  ** IWM SELECT line). Authoritative mapping (Inside Mac / MAME sonydriv). */
  int ca0 = iwm_lines & 1, ca1 = (iwm_lines >> 1) & 1, ca2 = (iwm_lines >> 2) & 1;
  int sel = (via_ora >> 5) & 1;
  switch ((ca2 << 3) | (ca1 << 2) | (ca0 << 1) | sel) {
    case 0x0: return step_dir;              /* DIRTN */
    case 0x1: return disk_in ? 0 : 1;       /* CSTIN: 0 = disk in place */
    case 0x2: return 1;                     /* STEP: 1 = step idle (0 = stepping) */
    case 0x3: return 1;                     /* WRTPRT: 1 = not write-protected */
    case 0x4: return motor_on ? 0 : 1;      /* MOTORON: 0 = motor on */
    case 0x5: return drive_track ? 1 : 0;   /* TK0: 0 = head at track 0 */
    case 0x7: return (tach_ctr >> 3) & 1;   /* TACH */
    case 0x9: return 0;                     /* RDDATA1 */
    case 0xB: return 0;                     /* SIDES: 0 = single-sided (400K) */
    case 0xC: return 0;                     /* READY: 0 = ready */
    case 0xD: return 0;                     /* DRVIN: 0 = drive installed */
    default:  return 0;
  }
}

static uint8_t iwm_access(uint32_t a, int is_write, uint8_t wv){
  int reg = (int)((a >> 9) & 0xF);
  uint8_t rv = 0;
  int old_lstrb = (iwm_lines >> 3) & 1;
  if (reg & 1) iwm_lines |= (uint8_t)(1u << (reg >> 1)); else iwm_lines &= (uint8_t)~(1u << (reg >> 1));
  {
    int q6 = (iwm_lines >> 6) & 1, q7 = (iwm_lines >> 7) & 1, en = (iwm_lines >> 4) & 1;
    int lstrb = (iwm_lines >> 3) & 1;
    int ca0 = iwm_lines & 1, ca1 = (iwm_lines >> 1) & 1, ca2 = (iwm_lines >> 2) & 1, sel = (via_ora >> 5) & 1;
    motor_on = en;
    if (lstrb && !old_lstrb) {               /* strobe writes a drive control register */
      switch ((sel << 3) | (ca2 << 2) | (ca1 << 1) | ca0) {
        case 0x0: step_dir = 0; break;                              /* step toward higher tracks */
        case 0x1: step_dir = 1; break;                              /* step toward track 0 */
        case 0x2: drive_track += step_dir ? (drive_track ? -1 : 0) : (drive_track < 79 ? 1 : 0); break; /* STEP */
        default: break;
      }
    }
    if (is_write) { if (q7 && q6 && !en) iwm_mode = wv; }
    else if (q6 && !q7) rv = (uint8_t)((drive_sense() ? 0x80 : 0) | (iwm_mode & 0x1F));  /* status */
    else if (!q6 && !q7) rv = 0x00;          /* data register (GCR) — stub, next step */
  }
  tach_ctr++;
  if (mac_iwm_hook) mac_iwm_hook(a, is_write, iwm_lines, is_write ? wv : rv);
  return rv;
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
