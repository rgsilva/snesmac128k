/* Macintosh 128K machine model — see mac.h. Enough of the memory map + VIA to
** run the ROM's power-on / RAM test / hardware init; IWM and SCC are stubbed
** (no disk, no serial) and the VBL interrupt is delivered via the VIA. This is
** HLE-leaning: we model registers and the overlay, not chip-accurate timing. */
#include <string.h>
#include "mac.h"

/* Memory backend. On the host, the Mac's RAM/ROM/disk are plain arrays and
** pointers. On the SNES (-DMAC_SNES) a 128 KB array exceeds Calypsi's object
** size limit and won't fit in WRAM beside the runtime, so RAM lives in cart
** SRAM and ROM/disk in cart ROM, reached through accessor functions the SNES
** main provides (mac_snesmem.c). Everything else in this file is identical. */
#ifdef MAC_SNES
uint8_t mac_ram_rd(uint32_t a);
void    mac_ram_wr(uint32_t a, uint8_t v);
uint8_t mac_rom_rd(uint32_t a);
uint8_t mac_disk_rd(uint32_t a);
#  define RAM_RD(a)     mac_ram_rd(a)
#  define RAM_WR(a, v)  mac_ram_wr((a), (v))
#  define ROM_RD(a)     mac_rom_rd(a)
#  define DISK_RD(a)    mac_disk_rd(a)
#else
static uint8_t        ram[MAC_RAM_SIZE];
static const uint8_t *rom;                 /* 64 KB */
#  define RAM_RD(a)     ram[a]
#  define RAM_WR(a, v)  (ram[a] = (v))
#  define ROM_RD(a)     rom[a]
#  define DISK_RD(a)    disk_data[a]
#endif
static int            overlay;             /* 1 at reset: ROM at $0 */
static uint32_t       vbl_count;
static uint16_t       vbl_timer;           /* down-counter to the next VBL: a
                                           ** 16-bit compare-to-zero, where the
                                           ** old `instr_count % VBL_PERIOD` was
                                           ** a 32-bit runtime division EVERY
                                           ** instruction (docs/PERF.md item B) */

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
#ifndef MAC_SNES
uint8_t *mac_ram_ptr(void)   { return ram; }    /* host tools read the framebuffer here */
#endif
int mac_dbg_ier(void){ return via_ier; }
int mac_dbg_ifr(void){ return via_ifr; }

/* Mouse: position is injected into the low-memory globals (mac_set_mouse); the
** button is VIA port B bit 3 (SW, low = pressed), read back below. */
static int mouse_btn;                    /* 1 = pressed */

static void via_update_irq(void){
  /* VIA (CA1=VBL, etc.) is wired to 68000 IPL level 1 */
  m68kc.irq = (via_ifr & via_ier & 0x7F) ? 1 : 0;
}

/* ---- VIA register access (reg = address bits A9-A12) ---- */
static uint8_t via_read(uint32_t a){
  int reg = (int)((a >> 9) & 0xF);
  switch (reg) {
    case 0:  return (uint8_t)((via_orb & ~0x08u) | (mouse_btn ? 0u : 0x08u));  /* PB3 = mouse SW */
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

void (*mac_sense_hook)(int reg, int val) = 0;   /* host debugging */

static int drive_sense(void){
  /* Sony read register = (CA2,CA1,CA0,SEL); SEL is VIA port A bit 5 (not the
  ** IWM SELECT line). Authoritative mapping (Inside Mac / MAME sonydriv). */
  int ca0 = iwm_lines & 1, ca1 = (iwm_lines >> 1) & 1, ca2 = (iwm_lines >> 2) & 1;
  int sel = (via_ora >> 5) & 1;
  int reg = (ca2 << 3) | (ca1 << 2) | (ca0 << 1) | sel;
  int v;
  switch (reg) {
    case 0x0: v = step_dir; break;              /* DIRTN */
    case 0x1: v = disk_in ? 0 : 1; break;       /* CSTIN: 0 = disk in place */
    case 0x2: v = 1; break;                     /* STEP: 1 = step idle */
    case 0x3: v = 1; break;                     /* WRTPRT: 1 = not write-protected */
    case 0x4: v = motor_on ? 0 : 1; break;      /* MOTORON: 0 = motor on */
    case 0x5: v = drive_track ? 1 : 0; break;   /* TK0: 0 = head at track 0 */
    case 0x7: v = (tach_ctr >> 3) & 1; break;   /* TACH */
    case 0x9: v = 0; break;                     /* RDDATA1 */
    case 0xB: v = 0; break;                     /* SIDES: 0 = single-sided (400K) */
    case 0xC: v = 0; break;                     /* READY: 0 = ready */
    case 0xD: v = 0; break;                     /* DRVIN: 0 = drive installed */
    default:  v = 0; break;
  }
  if (mac_sense_hook) mac_sense_hook(reg, v);
  return v;
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
  if (a < 0x400000) return overlay ? ROM_RD(a & 0xFFFF) : RAM_RD(a & 0x1FFFF);
  if (a < 0x500000) return ROM_RD(a & 0xFFFF);                     /* ROM $400000 */
  if (a >= 0x600000 && a < 0x800000) return overlay ? RAM_RD(a & 0x1FFFF) : 0xFF;
  if (a >= 0x800000 && a < 0xA00000) return scc_read(a);
  if (a >= 0xC00000 && a < 0xE00000) return iwm_read(a);
  if (a >= 0xE80000) return via_read(a);
  return 0xFF;
}
static void bus_write(uint32_t a, uint8_t v){
  a &= 0xFFFFFF;
  if (a < 0x400000) { if (!overlay) RAM_WR(a & 0x1FFFF, v); return; }
  if (a >= 0x600000 && a < 0x800000) { if (overlay) RAM_WR(a & 0x1FFFF, v); return; }
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
#ifdef MAC_SNES
  (void)rom_;                                     /* ROM is fixed in cart, via mac_rom_rd */
  { uint32_t i; for (i = 0; i < MAC_RAM_SIZE; i++) mac_ram_wr(i, 0); }
#else
  rom = rom_;
  memset(ram, 0, sizeof ram);
#endif
  overlay = 1;
  via_orb = via_ora = via_ddrb = via_ddra = 0;
  via_ifr = via_ier = via_acr = via_pcr = via_sr = 0;
  via_t1 = via_t2 = 0xFFFF;
  vbl_count = 0;
  vbl_timer = VBL_PERIOD;           /* first VBL after exactly VBL_PERIOD steps,
                                    ** same phase as the old modulo */
  memset(&m68kc, 0, sizeof m68kc);
  m68kc.a[7] = m68k_read32(0);      /* overlay on => reads ROM: SP=checksum */
  m68kc.pc = m68k_read32(4);        /* PC = $0040002A */
  m68kc.isp = m68kc.a[7];
  m68kc.sr = 0x2700;                /* supervisor, IRQ mask 7, trace off */
}

/* ---- HLE the .Sony floppy driver ---------------------------------------
** Intercept Device-Manager OS traps (Read/Write/Control/Status/KillIO) whose
** parameter block targets refnum -5 (.Sony) and service them from a logical
** disk image, skipping the real driver — and thus all the IWM/GCR/motor-speed
** emulation. ioPosOffset is the byte offset of the block on the disk. */
static const uint8_t *disk_data;
static uint32_t       disk_size;
void mac_set_disk(const uint8_t *data, uint32_t size){ disk_data = data; disk_size = size; }
void (*mac_sony_hook)(int tnum, uint32_t pos, uint32_t cnt) = 0;   /* host debugging */

/* HLE the mouse: rather than emulate the quadrature hardware + its interrupts,
** write the cursor position straight into the low-memory globals the ROM's VBL
** cursor task follows (RawMouse / MTemp, a Point {v,h}) and flag CrsrNew so it
** redraws; the button is a VIA PB3 line (above). x in [0,511], y in [0,341]. */
void mac_set_mouse(int x, int y, int btn){
  if (x < 0) x = 0; else if (x > 511) x = 511;
  if (y < 0) y = 0; else if (y > 341) y = 341;
  m68k_write16(0x0828u, (uint16_t)y); m68k_write16(0x082Au, (uint16_t)x);  /* MTemp   {v,h} */
  m68k_write16(0x082Cu, (uint16_t)y); m68k_write16(0x082Eu, (uint16_t)x);  /* RawMouse{v,h} */
  m68k_write8(0x08CEu, 0xFFu);                                             /* CrsrNew: moved */
  mouse_btn = btn;
}

static int sony_trap(void){
  uint16_t op;
  int tnum, refnum;
  uint32_t pb;
  if (!disk_size) return 0;
  op = m68k_read16(m68kc.pc);
  if ((op & 0xF800u) != 0xA000u) return 0;       /* not an OS (Device Manager) trap */
  tnum = op & 0xFF;
  if (tnum < 0x02 || tnum > 0x06) return 0;       /* Read/Write/Control/Status/KillIO only */
  pb = m68kc.a[0];
  refnum = (int16_t)m68k_read16(pb + 0x18);
  if (refnum == -4) { if (tnum != 0x02) return 0; }  /* -4: serve reads only */
  else if (refnum != -5) return 0;                    /* -5 (.Sony): all I/O */

  /* The real .Sony driver stores the disk geometry in the drive-queue element
  ** when a disk is inserted; _MountVol validates the volume size against it.
  ** Our HLE bypasses the driver, leaving dQDrvSz = 0, so the mount rejects the
  ** MDB with badMDBErr (-60). Populate it here (400K floppy = 800 blocks). */
  {
    uint32_t e = m68k_read32(0x030A);             /* DrvQHdr.qHead */
    while (e && e < MAC_RAM_SIZE) {
      if ((int16_t)m68k_read16(e + 12) == 0) {    /* dQDrvSz still unset */
        m68k_write16(e + 12, (uint16_t)(disk_size / 512u));  /* logical blocks */
        m68k_write16(e + 14, 0);                  /* dQDrvSz2 (high word) */
      }
      e = m68k_read32(e);                          /* qLink -> next element */
    }
  }

  if (tnum == 0x02) {                             /* _Read */
    uint32_t buf = m68k_read32(pb + 0x20);        /* ioBuffer */
    uint32_t cnt = m68k_read32(pb + 0x24);        /* ioReqCount */
    uint32_t pos = m68k_read32(pb + 0x2E);        /* ioPosOffset (byte offset) */
    uint32_t k;
    for (k = 0; k < cnt; k++)
      m68k_write8(buf + k, (pos + k < disk_size) ? DISK_RD(pos + k) : 0);
    m68k_write32(pb + 0x28, cnt);                 /* ioActCount */
    if (mac_sony_hook) mac_sony_hook(tnum, pos, cnt);
  } else if (mac_sony_hook) mac_sony_hook(tnum, m68k_read16(pb + 0x1A), 0);  /* csCode */
  m68k_write16(pb + 0x10, 0);                     /* ioResult = noErr */
  m68kc.d[0] = 0;
  /* The real driver's IODone sets the CCR from the result code (TST D0) before
  ** returning / calling the completion; the ROM relies on Z. Missing this Z
  ** made a completion take a "moveq #0,D3" error branch that zeroed the map
  ** block number and read the wrong sector. */
  m68kc.sr = (uint16_t)((m68kc.sr & ~0x0Fu) | 0x04u);  /* Z=1 (noErr), N/V/C=0 */

  /* Emulate the driver's IODone. An asynchronous request carries a completion
  ** routine (ioCompletion, pb+0x0C != 0); the real driver would invoke it on
  ** I/O completion, and that routine advances the File Manager's mount/open
  ** state machine and ultimately clears the OUTER request's ioResult that the
  ** caller busy-waits on. Opcode-level interception skips the driver, so we run
  ** the completion ourselves: jump to it (A0=pb, D0=result, Z from result) with
  ** a return address just past the trap word. Any further async reads it issues
  ** are served and chained the same way, unwinding the whole operation. */
  if (op & 0x0400u) {                             /* async trap (bit 10) only */
    uint32_t iocompl = m68k_read32(pb + 0x0C);
    if (iocompl) {
      m68kc.a[7] -= 4;
      m68k_write32(m68kc.a[7], m68kc.pc + 2);     /* completion RTS -> past trap */
      m68kc.a[0] = pb;                            /* completion arg: A0 = pb */
      m68kc.pc = iocompl;
      return 1;
    }
  }
  /* Synchronous request: caller continues inline past the trap word. */
  m68kc.pc += 2;
  return 1;
}

void mac_step(void){
  if (!sony_trap()) m68k_step();
  via_t1 = (uint16_t)(via_t1 - 1);   /* rough timer motion so poll loops progress */
  via_t2 = (uint16_t)(via_t2 - 1);
  if (--vbl_timer == 0) {
    vbl_timer = VBL_PERIOD;
    via_ifr |= 0x02;                 /* CA1 = vertical blank */
    vbl_count++;
    via_update_irq();
  }
}
