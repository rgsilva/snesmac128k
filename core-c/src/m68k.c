/* Lean 68000 core — see m68k.h. Dispatch is a bitfield switch on the opcode's
** top nibble then a sub-decode; 32-bit values are uint32_t throughout. Flag
** formulas follow the Motorola M68000 PRM; validated against the
** SingleStepTests/m68000 vectors and, differentially, against Musashi. */
#include "m68k.h"

/* Memory-access macro layer. Default: the extern embedder interface
** (m68k.h). A machine build may define M68K_BUS_INLINE to inject inline
** fast paths (snes/m68kbus.h: RAM/ROM decode + the PC-window fetch cache
** hit straight-line in the core, no jsl per access — docs/PERF.md). */
#ifdef M68K_BUS_INLINE
#  include "m68kbus.h"
#else
#  define M68K_RD8(a)      m68k_read8(a)
#  define M68K_RD16(a)     m68k_read16(a)
#  define M68K_RD32(a)     m68k_read32(a)
#  define M68K_WR8(a, v)   m68k_write8((a), (v))
#  define M68K_WR16(a, v)  m68k_write16((a), (v))
#  define M68K_WR32(a, v)  m68k_write32((a), (v))
#  define M68K_FETCH16(a)  m68k_fetch16(a)
#endif

M68K_HOT m68k_t m68kc;

/* size is always in bytes: 1, 2, or 4 */
static const uint32_t MASKSZ[5] = {0, 0xFFu, 0xFFFFu, 0, 0xFFFFFFFFu};
static const uint32_t MSBSZ[5]  = {0, 0x80u, 0x8000u, 0, 0x80000000u};

static uint16_t fetch16(void){ uint16_t w = M68K_FETCH16(m68kc.pc); m68kc.pc += 2; return w; }
static uint32_t fetch32(void){ uint32_t l = (uint32_t)fetch16() << 16; return l | fetch16(); }

/* Explicit sign-extend of the low 8 bits to 32. Calypsi miscompiles the
** (int8_t) cast of a value with bit 7 set (16->8->32 truncation+extend), which
** silently broke short backward branches and negative MOVEQ — caught by the M2
** SNES parity harness. This branchless form ((x^0x80)-0x80) sidesteps it. */
static uint32_t sxt8(uint32_t v){ return (uint32_t)((int32_t)((v & 0xFFu) ^ 0x80u) - 128); }

static uint32_t sext(uint32_t v, int size){
  if (size == 1) return sxt8(v);
  if (size == 2) return (uint32_t)(int32_t)(int16_t)v;
  return v;
}

/* ---- SR / stack banking ---- */
void m68k_set_sr(uint16_t val){
  val &= SR_VALID;
  if ((m68kc.sr & SR_S) != (val & SR_S)) {
    if (m68kc.sr & SR_S) { m68kc.isp = m68kc.a[7]; m68kc.a[7] = m68kc.usp; }
    else                 { m68kc.usp = m68kc.a[7]; m68kc.a[7] = m68kc.isp; }
  }
  m68kc.sr = val;
}
static void set_ccr(uint16_t ccr){ m68kc.sr = (uint16_t)((m68kc.sr & 0xFF00) | (ccr & 0x1F)); }

/* ---- CCR helpers ---- */
static void setf(uint16_t f, int cond){ if (cond) m68kc.sr |= f; else m68kc.sr &= (uint16_t)~f; }
static void set_nz(uint32_t r, int size){
  r &= MASKSZ[size];
  setf(SR_N, (r & MSBSZ[size]) != 0);
  setf(SR_Z, r == 0);
}
static void fl_logic(uint32_t r, int size){ set_nz(r, size); m68kc.sr &= (uint16_t)~(SR_V | SR_C); }

static void fl_add(uint32_t s, uint32_t d, uint32_t r, int size){
  uint32_t m = MSBSZ[size];
  int Sm = (s & m) != 0, Dm = (d & m) != 0, Rm = (r & m) != 0;
  set_nz(r, size);
  setf(SR_V, (Sm && Dm && !Rm) || (!Sm && !Dm && Rm));
  setf(SR_C, (Sm && Dm) || (!Rm && Dm) || (Sm && !Rm));
  setf(SR_X, (m68kc.sr & SR_C) != 0);
}
static void fl_sub(uint32_t s, uint32_t d, uint32_t r, int size, int setx){
  uint32_t m = MSBSZ[size];
  int Sm = (s & m) != 0, Dm = (d & m) != 0, Rm = (r & m) != 0;
  set_nz(r, size);
  setf(SR_V, (!Sm && Dm && !Rm) || (Sm && !Dm && Rm));
  setf(SR_C, (Sm && !Dm) || (Rm && !Dm) || (Sm && Rm));
  if (setx) setf(SR_X, (m68kc.sr & SR_C) != 0);
}

/* ---- register access ---- */
static uint32_t dreg_r(int reg, int size){ return M68K_DREG(reg) & MASKSZ[size]; }
static void dreg_w(int reg, int size, uint32_t v){
  M68K_DREG(reg) = (M68K_DREG(reg) & ~MASKSZ[size]) | (v & MASKSZ[size]);
}

/* ---- effective address ---- */
static M68K_HOT int      ea_kind;   /* 0=mem 1=Dn 2=An 3=imm */
static M68K_HOT int      ea_reg;
static M68K_HOT uint32_t ea_ad;

static uint32_t ea_index_ext(uint32_t base){
  uint16_t ext = fetch16();
  int32_t disp = (int32_t)sxt8(ext);
  int ri = (ext >> 12) & 7;
  uint32_t idx = (ext & 0x8000) ? M68K_AREG(ri) : M68K_DREG(ri);
  if (!(ext & 0x0800)) idx = (uint32_t)(int32_t)(int16_t)idx;
  return base + (uint32_t)disp + idx;
}
static void ea_resolve(int mode, int reg, int size){
  uint32_t a;
  switch (mode) {
    case 0: ea_kind = 1; ea_reg = reg; return;
    case 1: ea_kind = 2; ea_reg = reg; return;
    case 2: ea_kind = 0; ea_ad = M68K_AREG(reg); return;
    case 3: ea_kind = 0; a = M68K_AREG(reg);
            M68K_AREG(reg) = a + ((reg == 7 && size == 1) ? 2 : size); ea_ad = a; return;
    case 4: ea_kind = 0; M68K_AREG(reg) -= (reg == 7 && size == 1) ? 2 : size; ea_ad = M68K_AREG(reg); return;
    case 5: ea_kind = 0; ea_ad = M68K_AREG(reg) + (uint32_t)(int32_t)(int16_t)fetch16(); return;
    case 6: ea_kind = 0; ea_ad = ea_index_ext(M68K_AREG(reg)); return;
    case 7:
      switch (reg) {
        case 0: ea_kind = 0; ea_ad = (uint32_t)(int32_t)(int16_t)fetch16(); return;
        case 1: ea_kind = 0; ea_ad = fetch32(); return;
        case 2: ea_kind = 0; a = m68kc.pc; ea_ad = a + (uint32_t)(int32_t)(int16_t)fetch16(); return;
        case 3: ea_kind = 0; ea_ad = ea_index_ext(m68kc.pc); return;
        default: ea_kind = 3; return;   /* #imm */
      }
  }
  ea_kind = 0; ea_ad = 0;
}
static uint32_t mem_r(uint32_t a, int size){
  if (size == 1) return M68K_RD8(a);
  if (size == 2) return M68K_RD16(a);
  return M68K_RD32(a);
}
static void mem_w(uint32_t a, int size, uint32_t v){
  if (size == 1) M68K_WR8(a, (uint8_t)v);
  else if (size == 2) M68K_WR16(a, (uint16_t)v);
  else M68K_WR32(a, v);
}
static uint32_t ea_val(int size){
  if (ea_kind == 1) return dreg_r(ea_reg, size);
  if (ea_kind == 2) return M68K_AREG(ea_reg) & MASKSZ[size];
  return mem_r(ea_ad, size);
}
static void ea_store(int size, uint32_t v){
  if (ea_kind == 1) dreg_w(ea_reg, size, v);
  else if (ea_kind == 2) M68K_AREG(ea_reg) = sext(v, size);
  else mem_w(ea_ad, size, v);
}
static uint32_t ea_imm(int size){
  if (size == 1) return fetch16() & 0xFF;
  if (size == 2) return fetch16();
  return fetch32();
}
/* resolve + read a source operand (handles immediate) */
static uint32_t src_read(int mode, int reg, int size){
  if (mode == 7 && reg == 4) { ea_kind = 3; return ea_imm(size); }
  ea_resolve(mode, reg, size);
  return ea_val(size);
}

/* ---- exceptions (group 1/2, 6-byte frame) ---- */
M68K_HOT uint32_t m68k_start_pc;       /* pc at the start of the current instruction
                                       ** (public: the asm inner loop stamps it) */
#define g_start_pc m68k_start_pc
int (*m68k_aline_hook)(uint16_t op) = 0;
static void exception(int vec, uint32_t push_pc){
  uint16_t oldsr = m68kc.sr;
  m68k_set_sr((uint16_t)((m68kc.sr | SR_S) & ~SR_T));
  m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], push_pc);
  m68kc.a[7] -= 2; M68K_WR16(m68kc.a[7], oldsr);
  m68kc.pc = M68K_RD32((uint32_t)vec * 4);
}
static int priv_ok(void){                     /* true if allowed (supervisor) */
  if (m68kc.sr & SR_S) return 1;
  exception(EXC_PRIV, g_start_pc);
  return 0;
}

static int cond_true(int c){
  uint16_t s = m68kc.sr;
  int N = (s & SR_N) != 0, Z = (s & SR_Z) != 0, V = (s & SR_V) != 0, C = (s & SR_C) != 0;
  switch (c) {
    case 0:  return 1;               case 1:  return 0;
    case 2:  return !C && !Z;        case 3:  return C || Z;
    case 4:  return !C;              case 5:  return C;
    case 6:  return !Z;              case 7:  return Z;
    case 8:  return !V;              case 9:  return V;
    case 10: return !N;              case 11: return N;
    case 12: return N == V;          case 13: return N != V;
    case 14: return !Z && (N == V);  case 15: return Z || (N != V);
  }
  return 0;
}

/* extended-precision add/sub with X and cumulative Z */
static void fl_addx(uint32_t s, uint32_t d, uint32_t r, int size){
  uint32_t m = MSBSZ[size];
  int Sm = (s & m) != 0, Dm = (d & m) != 0, Rm = (r & m) != 0;
  setf(SR_N, Rm);
  if ((r & MASKSZ[size]) != 0) m68kc.sr &= (uint16_t)~SR_Z;
  setf(SR_V, (Sm && Dm && !Rm) || (!Sm && !Dm && Rm));
  setf(SR_C, (Sm && Dm) || (!Rm && Dm) || (Sm && !Rm));
  setf(SR_X, (m68kc.sr & SR_C) != 0);
}
static void fl_subx(uint32_t s, uint32_t d, uint32_t r, int size){
  uint32_t m = MSBSZ[size];
  int Sm = (s & m) != 0, Dm = (d & m) != 0, Rm = (r & m) != 0;
  setf(SR_N, Rm);
  if ((r & MASKSZ[size]) != 0) m68kc.sr &= (uint16_t)~SR_Z;
  setf(SR_V, (!Sm && Dm && !Rm) || (Sm && !Dm && Rm));
  setf(SR_C, (Sm && !Dm) || (Rm && !Dm) || (Sm && Rm));
  setf(SR_X, (m68kc.sr & SR_C) != 0);
}

/* ============================ instruction groups ======================== */

static void op_move(uint16_t op, int size){          /* groups 1/2/3 */
  int dstreg = (op >> 9) & 7, dstmode = (op >> 6) & 7, srcmode = (op >> 3) & 7, srcreg = op & 7;
  uint32_t v = src_read(srcmode, srcreg, size);
  if (dstmode == 1) { M68K_AREG(dstreg) = sext(v, size); return; }   /* MOVEA */
  ea_resolve(dstmode, dstreg, size);
  ea_store(size, v);
  fl_logic(v, size);
}

static void op_alu_std(uint16_t op, int base){       /* 8,9,B,C,D common ALU */
  int dn = (op >> 9) & 7, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  int size = (opmode & 3) == 0 ? 1 : ((opmode & 3) == 1 ? 2 : 4);
  uint32_t s, d, r;

  if (opmode == 3 || opmode == 7) {                  /* <ea> op An / MUL / DIV */
    int asz = (opmode == 3) ? 2 : 4;
    if (base == 0x8 || base == 0xC) {                /* DIVU/DIVS / MULU/MULS */
      uint32_t ea = src_read(mode, reg, 2);
      if (base == 0xC) {                             /* MUL: opmode3=MULU, 7=MULS */
        uint32_t res;
        if (opmode == 3) res = (dreg_r(dn, 2)) * (ea & 0xFFFF);
        else res = (uint32_t)((int32_t)(int16_t)dreg_r(dn, 2) * (int32_t)(int16_t)ea);
        M68K_DREG(dn) = res; fl_logic(res, 4);
      } else {                                       /* DIV */
        uint32_t divisor = ea & 0xFFFF;
        if (divisor == 0) { exception(EXC_ZERODIV, m68kc.pc); return; }
        if (opmode == 3) {                           /* DIVU */
          uint32_t dd = M68K_DREG(dn);
          uint32_t q = dd / divisor, rem = dd % divisor;
          if (q > 0xFFFF) { setf(SR_V, 1); setf(SR_C, 0); return; }
          M68K_DREG(dn) = (rem << 16) | (q & 0xFFFF);
          set_nz(q, 2); setf(SR_V, 0); setf(SR_C, 0);
        } else {                                     /* DIVS */
          int32_t dd = (int32_t)M68K_DREG(dn), dv = (int32_t)(int16_t)divisor;
          int32_t q = dd / dv, rem = dd % dv;
          if (q > 32767 || q < -32768) { setf(SR_V, 1); setf(SR_C, 0); return; }
          M68K_DREG(dn) = ((uint32_t)(rem & 0xFFFF) << 16) | ((uint32_t)q & 0xFFFF);
          set_nz((uint32_t)q, 2); setf(SR_V, 0); setf(SR_C, 0);
        }
      }
      return;
    }
    /* ADDA / SUBA / CMPA */
    s = sext(src_read(mode, reg, asz), asz);
    d = M68K_AREG(dn);
    if (base == 0xD) M68K_AREG(dn) = d + s;
    else if (base == 0x9) M68K_AREG(dn) = d - s;
    else { r = d - s; fl_sub(s, d, r, 4, 0); }        /* CMPA: full 32-bit compare */
    return;
  }

  if (opmode < 3) {                                  /* <ea> op Dn -> Dn (or CMP/EOR read) */
    s = src_read(mode, reg, size);
    d = dreg_r(dn, size);
    switch (base) {
      case 0x8: r = d | s; dreg_w(dn, size, r); fl_logic(r, size); break;
      case 0xC: r = d & s; dreg_w(dn, size, r); fl_logic(r, size); break;
      case 0x9: r = d - s; fl_sub(s, d, r, size, 1); dreg_w(dn, size, r); break;
      case 0xD: r = d + s; fl_add(s, d, r, size);    dreg_w(dn, size, r); break;
      case 0xB: r = d - s; fl_sub(s, d, r, size, 0); break;   /* CMP */
    }
    return;
  }

  /* opmode 4/5/6: Dn op <ea> -> <ea>, or ADDX/SUBX, or EOR, or CMPM */
  if (base == 0xB) {                                 /* EOR or CMPM */
    if (mode == 1) {                                 /* CMPM (Ay)+,(Ax)+ (postincrement) */
      ea_resolve(3, reg, size); s = ea_val(size);
      ea_resolve(3, dn, size);  d = ea_val(size);
      r = d - s; fl_sub(s, d, r, size, 0);
    } else {                                         /* EOR Dn,<ea> */
      ea_resolve(mode, reg, size);
      d = ea_val(size); s = dreg_r(dn, size);
      r = d ^ s; ea_store(size, r); fl_logic(r, size);
    }
    return;
  }
  if ((base == 0xD || base == 0x9) && mode <= 1) {   /* ADDX / SUBX */
    if (mode == 0) { s = dreg_r(reg, size); d = dreg_r(dn, size); }
    else {
      ea_resolve(4, reg, size); s = ea_val(size);
      ea_resolve(4, dn, size);  d = ea_val(size);
    }
    { uint32_t x = (m68kc.sr & SR_X) ? 1 : 0;
      if (base == 0xD) { r = d + s + x; fl_addx(s, d, r, size); }
      else             { r = d - s - x; fl_subx(s, d, r, size); }
    }
    if (mode == 0) dreg_w(dn, size, r); else ea_store(size, r);
    return;
  }
  /* Dn op <ea> -> <ea> */
  ea_resolve(mode, reg, size);
  d = ea_val(size); s = dreg_r(dn, size);
  switch (base) {
    case 0x8: r = d | s; break;
    case 0xC: r = d & s; break;
    case 0x9: r = d - s; fl_sub(s, d, r, size, 1); ea_store(size, r); return;
    case 0xD: r = d + s; fl_add(s, d, r, size);    ea_store(size, r); return;
    default:  r = d; break;
  }
  ea_store(size, r); fl_logic(r, size);
}

/* type: 0=AS 1=LS 2=ROX(through X) 3=RO(plain) — matches the 68000 encoding
** (bits 4-3 for register shifts, 10-9 for memory shifts) */
static uint32_t do_shift(int type, int dir, uint32_t v, int cnt, int size){
  uint32_t m = MSBSZ[size], msk = MASKSZ[size];
  int lastc = 0, ovf = 0;
  int x = (m68kc.sr & SR_X) ? 1 : 0;
  int i;
  v &= msk;
  for (i = 0; i < cnt; i++) {
    if (dir) {                                       /* left */
      int top = (v & m) != 0;
      lastc = top;
      if (type == 0) { v = (v << 1) & msk; if (top != ((v & m) != 0)) ovf = 1; }        /* ASL */
      else if (type == 1) { v = (v << 1) & msk; }                                       /* LSL */
      else if (type == 2) { v = ((v << 1) | x) & msk; x = top; }                        /* ROXL */
      else { v = ((v << 1) | top) & msk; }                                              /* ROL */
    } else {                                         /* right */
      int bot = v & 1;
      lastc = bot;
      if (type == 0) { v = (v & m) ? ((v >> 1) | m) : (v >> 1); }                       /* ASR */
      else if (type == 1) { v = (v >> 1) & msk; }                                       /* LSR */
      else if (type == 2) { v = ((v >> 1) | (x ? m : 0)) & msk; x = bot; }              /* ROXR */
      else { v = ((v >> 1) | (bot ? m : 0)) & msk; }                                    /* ROR */
    }
  }
  set_nz(v, size);
  setf(SR_V, (type == 0) ? ovf : 0);                 /* only ASx sets V */
  if (cnt == 0) {
    setf(SR_C, (type == 2) ? ((m68kc.sr & SR_X) != 0) : 0);   /* ROXx: C=X; others C=0 */
  } else {
    setf(SR_C, lastc);
    if (type != 3) setf(SR_X, lastc);                /* AS/LS/ROX set X; RO leaves it */
  }
  return v;
}

static void op_shift(uint16_t op){                   /* group E */
  int size = (op >> 6) & 3;
  if (size == 3) {                                   /* memory shift by 1 */
    int type = (op >> 9) & 3, dir = (op >> 8) & 1, mode = (op >> 3) & 7, reg = op & 7;
    ea_resolve(mode, reg, 2);
    uint32_t v = ea_val(2);
    v = do_shift(type, dir, v, 1, 2);
    ea_store(2, v);
    return;
  }
  int sz = size == 0 ? 1 : (size == 1 ? 2 : 4);
  int dir = (op >> 8) & 1, reg = op & 7;
  int ir = (op >> 5) & 1, cr = (op >> 9) & 7, type = (op >> 3) & 3;
  int cnt = ir ? (M68K_DREG(cr) & 63) : (cr == 0 ? 8 : cr);
  uint32_t v = do_shift(type, dir, dreg_r(reg, sz), cnt, sz);
  dreg_w(reg, sz, v);
}

static void op_group4(uint16_t op){                  /* misc */
  int mode = (op >> 3) & 7, reg = op & 7;
  uint32_t r, d;

  /* Hot-first decode (docs/PERF.md profiler round, item 3): LEA / JSR / RTS
  ** dominate real 68000 code; the rare TRAP/LINK/UNLK/USP checks used to be
  ** walked first by every group-4 instruction. Reorder is safe: the hoisted
  ** patterns are disjoint from everything that now runs after them (the
  ** SWAP-before-PEA and EXT-before-MOVEM dependencies below are untouched). */
  if ((op & 0xF1C0) == 0x41C0) {                     /* LEA <ea>,An */
    int an = (op >> 9) & 7; ea_resolve(mode, reg, 4); M68K_AREG(an) = ea_ad; return;
  }
  if ((op & 0xFFC0) == 0x4E80) {                     /* JSR */
    ea_resolve(mode, reg, 4); m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], m68kc.pc); m68kc.pc = ea_ad; return;
  }
  switch (op) {
    case 0x4E75:                                     /* RTS (hot: first) */
      m68kc.pc = M68K_RD32(m68kc.a[7]); m68kc.a[7] += 4; return;
    case 0x4E71: return;                             /* NOP */
    case 0x4E70: if (priv_ok()) {} return;          /* RESET (no-op here) */
    case 0x4E73:                                     /* RTE */
      if (!priv_ok()) return;
      { uint16_t nsr = M68K_RD16(m68kc.a[7]); m68kc.a[7] += 2;
        uint32_t npc = M68K_RD32(m68kc.a[7]); m68kc.a[7] += 4;
        m68k_set_sr(nsr); m68kc.pc = npc; } return;
    case 0x4E77:                                     /* RTR */
      { uint16_t ccr = M68K_RD16(m68kc.a[7]); m68kc.a[7] += 2; set_ccr(ccr);
        m68kc.pc = M68K_RD32(m68kc.a[7]); m68kc.a[7] += 4; } return;
    case 0x4E76:                                     /* TRAPV */
      if (m68kc.sr & SR_V) exception(EXC_TRAPV, m68kc.pc); return;
    case 0x4E72:                                     /* STOP #imm */
      { uint16_t imm = fetch16(); if (priv_ok()) { m68k_set_sr(imm); m68kc.stopped = 1; } } return;
    case 0x4AFC: exception(EXC_ILLEGAL, g_start_pc); return;   /* ILLEGAL */
    case 0x4E7A: case 0x4E7B: fetch16(); return;     /* MOVEC (020+): swallow */
  }

  if ((op & 0xFFC0) == 0x4EC0) {                     /* JMP */
    ea_resolve(mode, reg, 4); m68kc.pc = ea_ad; return;
  }
  /* rare system ops, checked after the hot set */
  if ((op & 0xFFF0) == 0x4E40) { exception(EXC_TRAP0 + (op & 0xF), m68kc.pc); return; }  /* TRAP #n */
  if ((op & 0xFFF8) == 0x4E50) {                     /* LINK An,#d16 */
    int an = op & 7; int16_t disp = (int16_t)fetch16();
    m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], M68K_AREG(an));
    M68K_AREG(an) = m68kc.a[7]; m68kc.a[7] += (int32_t)disp; return;
  }
  if ((op & 0xFFF8) == 0x4E58) {                     /* UNLK An (An==A7: load overwrites the +4) */
    int an = op & 7; uint32_t tmp;
    m68kc.a[7] = M68K_AREG(an); tmp = M68K_RD32(m68kc.a[7]); m68kc.a[7] += 4; M68K_AREG(an) = tmp; return;
  }
  if ((op & 0xFFF8) == 0x4E60) {                     /* MOVE An,USP */
    if (priv_ok()) { if (m68kc.sr & SR_S) m68kc.usp = M68K_AREG(op & 7); else m68kc.a[7] = M68K_AREG(op & 7); } return;
  }
  if ((op & 0xFFF8) == 0x4E68) {                     /* MOVE USP,An */
    if (priv_ok()) M68K_AREG(op & 7) = (m68kc.sr & SR_S) ? m68kc.usp : m68kc.a[7]; return;
  }
  if ((op & 0xFFF8) == 0x4840) { int dn = op & 7;    /* SWAP (mode 0) — before PEA */
    uint32_t v = M68K_DREG(dn); v = (v >> 16) | (v << 16); M68K_DREG(dn) = v; fl_logic(v, 4); return; }
  if ((op & 0xFFC0) == 0x4840) {                     /* PEA <ea> */
    ea_resolve(mode, reg, 4); m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], ea_ad); return;
  }
  if ((op & 0xFFB8) == 0x4880) {                     /* EXT */
    int dn = op & 7;
    if (op & 0x40) { M68K_DREG(dn) = (uint32_t)(int32_t)(int16_t)M68K_DREG(dn); fl_logic(M68K_DREG(dn), 4); }
    else { uint32_t v = sxt8(M68K_DREG(dn)); dreg_w(dn, 2, v); fl_logic(v, 2); }
    return;
  }
  if ((op & 0xF1C0) == 0x4180) {                     /* CHK.w */
    int dn = (op >> 9) & 7; int16_t bound = (int16_t)src_read(mode, reg, 2);
    int16_t v = (int16_t)dreg_r(dn, 2);
    if (v < 0 || v > bound) { setf(SR_N, v < 0); exception(EXC_CHK, m68kc.pc); }
    return;
  }
  { int sub = (op >> 8) & 0xF;                       /* NEGX/CLR/NEG/NOT/TST/TAS/MOVEfromSR/etc */
    int size = (op >> 6) & 3;
    int sz = size == 0 ? 1 : (size == 1 ? 2 : 4);
    if (op == 0x40C0 + (op & 0x3F) && (op & 0xFFC0) == 0x40C0) { /* MOVE SR,<ea> */
      ea_resolve(mode, reg, 2); ea_store(2, m68kc.sr); return;
    }
    if ((op & 0xFFC0) == 0x44C0) { set_ccr((uint16_t)src_read(mode, reg, 2)); return; } /* MOVE <ea>,CCR (incl #imm) */
    if ((op & 0xFFC0) == 0x46C0) { uint16_t v = (uint16_t)src_read(mode, reg, 2); if (priv_ok()) m68k_set_sr(v); return; } /* MOVE <ea>,SR */
    if ((op & 0xFFC0) == 0x4AC0) {                   /* TAS */
      ea_resolve(mode, reg, 1); d = ea_val(1); fl_logic(d, 1); ea_store(1, d | 0x80); return;
    }
    switch (sub) {
      case 0x0: ea_resolve(mode, reg, sz); d = ea_val(sz);        /* NEGX */
                r = 0 - d - ((m68kc.sr & SR_X) ? 1 : 0); fl_subx(d, 0, r, sz); ea_store(sz, r); return;
      case 0x2: ea_resolve(mode, reg, sz); ea_store(sz, 0);       /* CLR */
                m68kc.sr &= (uint16_t)~(SR_N | SR_V | SR_C); m68kc.sr |= SR_Z; return;
      case 0x4: ea_resolve(mode, reg, sz); d = ea_val(sz);        /* NEG */
                r = 0 - d; fl_sub(d, 0, r, sz, 1); ea_store(sz, r); return;
      case 0x6: ea_resolve(mode, reg, sz); d = ea_val(sz);        /* NOT */
                r = ~d; ea_store(sz, r); fl_logic(r, sz); return;
      case 0xA: if ((op & 0xFFC0) == 0x4AC0) return;              /* (TAS handled) */
                ea_resolve(mode, reg, sz); d = ea_val(sz); fl_logic(d, sz); return; /* TST */
    }
    if ((op & 0xFF80) == 0x4880 || (op & 0xFF80) == 0x4C80) {     /* MOVEM */
      int dir = (op >> 10) & 1;                       /* 0=reg->mem, 1=mem->reg */
      int wsz = (op & 0x40) ? 4 : 2;
      uint16_t list = fetch16();
      if (dir == 0 && mode == 4) {                    /* predecrement store: A7..D0 */
        uint32_t a = M68K_AREG(reg);
        for (int i = 0; i < 16; i++) if (list & (1 << i)) {
          uint32_t v = (i < 8) ? M68K_AREG(7 - i) : M68K_DREG(15 - i);   /* bit0=A7 .. bit15=D0 */
          a -= wsz; mem_w(a, wsz, v);
        }
        M68K_AREG(reg) = a;
      } else if (dir == 1 && mode == 3) {             /* postincrement load: D0..A7 */
        uint32_t a = M68K_AREG(reg);
        for (int i = 0; i < 16; i++) if (list & (1 << i)) {
          uint32_t v = sext(mem_r(a, wsz), wsz);
          if (i < 8) M68K_DREG(i) = v; else M68K_AREG(i - 8) = v;
          a += wsz;
        }
        M68K_AREG(reg) = a;
      } else {                                        /* control mode */
        ea_resolve(mode, reg, wsz); uint32_t a = ea_ad;
        for (int i = 0; i < 16; i++) if (list & (1 << i)) {
          if (dir == 0) { uint32_t v = (i < 8) ? M68K_DREG(i) : M68K_AREG(i - 8); mem_w(a, wsz, v); }
          else { uint32_t v = sext(mem_r(a, wsz), wsz); if (i < 8) M68K_DREG(i) = v; else M68K_AREG(i - 8) = v; }
          a += wsz;
        }
      }
      return;
    }
    if ((op & 0xFFC0) == 0x4800) {                    /* NBCD = 0 - <ea> - X (BCD) */
      ea_resolve(mode, reg, 1);
      { uint32_t s = ea_val(1); int x = (m68kc.sr & SR_X) ? 1 : 0;
        int lo = 0 - (int)(s & 0xF) - x, hi = 0 - (int)(s >> 4), bcd = 0, c = 0;
        if (lo < 0) { lo += 10; bcd = 1; }
        hi -= bcd; if (hi < 0) { hi += 10; c = 1; }
        { uint32_t res = (uint32_t)(((hi & 0xF) << 4) | (lo & 0xF));
          ea_store(1, res & 0xFF);
          if ((res & 0xFF) != 0) m68kc.sr &= (uint16_t)~SR_Z;
          setf(SR_C, c); setf(SR_X, c); } }
      return;
    }
  }
  exception(EXC_ILLEGAL, g_start_pc);
}

static void op_group0(uint16_t op){                  /* immediate ALU + bit ops + MOVEP */
  int mode = (op >> 3) & 7, reg = op & 7;
  int size = (op >> 6) & 3;
  int sz = size == 0 ? 1 : (size == 1 ? 2 : 4);
  uint32_t s, d, r;

  if ((op & 0x0138) == 0x0108) {                     /* MOVEP */
    int dn = (op >> 9) & 7, an = op & 7, dir = (op >> 7) & 1, lng = (op >> 6) & 1;
    uint32_t a = M68K_AREG(an) + (uint32_t)(int32_t)(int16_t)fetch16();
    if (dir) {                                       /* reg -> mem */
      if (lng) { M68K_WR8(a, M68K_DREG(dn) >> 24); M68K_WR8(a + 2, M68K_DREG(dn) >> 16);
                 M68K_WR8(a + 4, M68K_DREG(dn) >> 8); M68K_WR8(a + 6, M68K_DREG(dn)); }
      else { M68K_WR8(a, M68K_DREG(dn) >> 8); M68K_WR8(a + 2, M68K_DREG(dn)); }
    } else {                                         /* mem -> reg */
      if (lng) { uint32_t v = ((uint32_t)M68K_RD8(a) << 24) | ((uint32_t)M68K_RD8(a + 2) << 16)
                 | ((uint32_t)M68K_RD8(a + 4) << 8) | M68K_RD8(a + 6); M68K_DREG(dn) = v; }
      else { uint32_t v = ((uint32_t)M68K_RD8(a) << 8) | M68K_RD8(a + 2); dreg_w(dn, 2, v); }
    }
    return;
  }

  if ((op & 0x0100) || (op & 0x0F00) == 0x0800) {    /* bit ops (dynamic Dn / static #imm) */
    int bit; int stat = ((op & 0x0F00) == 0x0800);
    if (stat) bit = fetch16() & 0xFF; else bit = M68K_DREG((op >> 9) & 7) & 0xFF;
    int op2 = (op >> 6) & 3;                          /* 0 BTST 1 BCHG 2 BCLR 3 BSET */
    if (mode == 0) {                                 /* Dn: 32-bit */
      bit &= 31; uint32_t m = 1UL << bit; d = M68K_DREG(reg);
      setf(SR_Z, (d & m) == 0);
      if (op2 == 1) M68K_DREG(reg) = d ^ m;
      else if (op2 == 2) M68K_DREG(reg) = d & ~m;
      else if (op2 == 3) M68K_DREG(reg) = d | m;
    } else {                                         /* memory (8-bit), or immediate (BTST only) */
      bit &= 7; uint32_t m = 1UL << bit;
      if (mode == 7 && reg == 4) { d = ea_imm(1); setf(SR_Z, (d & m) == 0); }   /* BTST #imm */
      else {
        ea_resolve(mode, reg, 1); d = ea_val(1);
        setf(SR_Z, (d & m) == 0);
        if (op2 == 1) ea_store(1, d ^ m);
        else if (op2 == 2) ea_store(1, d & ~m);
        else if (op2 == 3) ea_store(1, d | m);
      }
    }
    return;
  }

  /* immediate ALU: ORI/ANDI/SUBI/ADDI/EORI/CMPI + to CCR/SR */
  int kind = (op >> 9) & 7;                          /* 0 OR 1 AND 2 SUB 3 ADD 5 EOR 6 CMP */
  if ((op & 0xFF) == 0x3C && (kind == 0 || kind == 1 || kind == 5)) {   /* xxI to CCR */
    uint16_t imm = fetch16() & 0x1F;
    if (kind == 0) set_ccr((m68kc.sr | imm)); else if (kind == 1) set_ccr((m68kc.sr & (0xE0 | imm))); else set_ccr((m68kc.sr ^ imm));
    return;
  }
  if ((op & 0xFF) == 0x7C && (kind == 0 || kind == 1 || kind == 5)) {   /* xxI to SR */
    uint16_t imm = fetch16();
    if (!priv_ok()) return;
    if (kind == 0) m68k_set_sr(m68kc.sr | imm); else if (kind == 1) m68k_set_sr(m68kc.sr & imm); else m68k_set_sr(m68kc.sr ^ imm);
    return;
  }
  s = ea_imm(sz);
  if (kind == 6) { ea_resolve(mode, reg, sz); d = ea_val(sz); r = d - s; fl_sub(s, d, r, sz, 0); return; }   /* CMPI */
  ea_resolve(mode, reg, sz); d = ea_val(sz);
  switch (kind) {
    case 0: r = d | s; ea_store(sz, r); fl_logic(r, sz); break;
    case 1: r = d & s; ea_store(sz, r); fl_logic(r, sz); break;
    case 2: r = d - s; fl_sub(s, d, r, sz, 1); ea_store(sz, r); break;
    case 3: r = d + s; fl_add(s, d, r, sz);    ea_store(sz, r); break;
    case 5: r = d ^ s; ea_store(sz, r); fl_logic(r, sz); break;
  }
}

static void op_bcd(uint16_t op, int sub){            /* ABCD (C) / SBCD (8) — documented divergence */
  int mode = (op >> 3) & 1, rx = (op >> 9) & 7, ry = op & 7;
  uint32_t s, d;
  if (mode == 0) { s = M68K_DREG(ry) & 0xFF; d = M68K_DREG(rx) & 0xFF; }
  else { ea_resolve(4, ry, 1); s = ea_val(1); ea_resolve(4, rx, 1); d = ea_val(1); }
  uint32_t x = (m68kc.sr & SR_X) ? 1 : 0, r;
  if (sub) {                                          /* SBCD */
    int lo = (int)(d & 0xF) - (int)(s & 0xF) - (int)x;
    int hi = (int)(d >> 4) - (int)(s >> 4);
    int bcd = 0; if (lo < 0) { lo += 10; bcd = 1; }
    hi -= bcd; int c = 0; if (hi < 0) { hi += 10; c = 1; }
    r = (uint32_t)(((hi & 0xF) << 4) | (lo & 0xF));
    setf(SR_C, c); setf(SR_X, c);
  } else {                                            /* ABCD */
    int lo = (int)(d & 0xF) + (int)(s & 0xF) + (int)x;
    int hi = (int)(d >> 4) + (int)(s >> 4);
    if (lo > 9) { lo -= 10; hi += 1; }
    int c = 0; if (hi > 9) { hi -= 10; c = 1; }
    r = (uint32_t)(((hi & 0xF) << 4) | (lo & 0xF));
    setf(SR_C, c); setf(SR_X, c);
  }
  if ((r & 0xFF) != 0) m68kc.sr &= (uint16_t)~SR_Z;
  if (mode == 0) dreg_w(rx, 1, r); else ea_store(1, r);
}

/* Take an autovectored interrupt at level `lvl` (vector 24+lvl). */
static void take_interrupt(int lvl){
  uint16_t oldsr = m68kc.sr;
  m68k_set_sr((uint16_t)((oldsr & ~(SR_T | SR_I)) | SR_S | ((uint16_t)lvl << 8)));
  m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], m68kc.pc);
  m68kc.a[7] -= 2; M68K_WR16(m68kc.a[7], oldsr);
  m68kc.pc = M68K_RD32((uint32_t)(24 + lvl) * 4);
  m68kc.stopped = 0;
}

/* Decode + execute one already-fetched instruction. PC is past the opcode
** word; g_start_pc holds its address (exceptions push it). Public so the
** hand-asm inner loop (snes/calypsi/m68krun.s) can fetch inline and call
** straight in (docs/PERF.md item 5). */
void m68k_exec(uint16_t op){
  switch (op >> 12) {
    case 0x0: op_group0(op); break;
    case 0x1: op_move(op, 1); break;
    case 0x2: op_move(op, 4); break;
    case 0x3: op_move(op, 2); break;
    case 0x4: op_group4(op); break;
    case 0x5:                                        /* ADDQ/SUBQ/Scc/DBcc */
      if (((op >> 6) & 3) == 3) {                    /* Scc / DBcc */
        int cond = (op >> 8) & 0xF, mode = (op >> 3) & 7, reg = op & 7;
        if (mode == 1) {                             /* DBcc */
          int16_t disp = (int16_t)fetch16(); uint32_t base = m68kc.pc - 2;
          if (!cond_true(cond)) {
            uint16_t c = (uint16_t)(M68K_DREG(reg) & 0xFFFF) - 1; dreg_w(reg, 2, c);
            if (c != 0xFFFF) m68kc.pc = base + disp;
          }
        } else {                                     /* Scc */
          ea_resolve(mode, reg, 1); ea_store(1, cond_true(cond) ? 0xFF : 0x00);
        }
      } else {                                       /* ADDQ/SUBQ */
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        int sub = (op >> 8) & 1, mode = (op >> 3) & 7, reg = op & 7;
        int size = (op >> 6) & 3; int sz = size == 0 ? 1 : (size == 1 ? 2 : 4);
        if (mode == 1) { M68K_AREG(reg) += sub ? (uint32_t)-data : (uint32_t)data; }   /* An: no flags, full 32 */
        else {
          ea_resolve(mode, reg, sz); uint32_t d = ea_val(sz), r;
          if (sub) { r = d - data; fl_sub(data, d, r, sz, 1); } else { r = d + data; fl_add(data, d, r, sz); }
          ea_store(sz, r);
        }
      }
      break;
    case 0x6: {                                      /* Bcc/BRA/BSR */
      int cond = (op >> 8) & 0xF; int32_t disp = (int32_t)sxt8(op); uint32_t base = m68kc.pc;
      if ((op & 0xFF) == 0) disp = (int16_t)fetch16();   /* 68000: 0x00 => 16-bit disp; 0xFF is just -1 */
      if (cond == 1) { m68kc.a[7] -= 4; M68K_WR32(m68kc.a[7], m68kc.pc); m68kc.pc = base + disp; }  /* BSR */
      else if (cond_true(cond)) m68kc.pc = base + disp;
      break;
    }
    case 0x7: {                                      /* MOVEQ */
      int reg = (op >> 9) & 7; uint32_t v = sxt8(op);
      M68K_DREG(reg) = v; fl_logic(v, 4); break;
    }
    case 0x8: if (((op >> 4) & 0x1F) == 0x10 && ((op >> 6) & 3) == 0) op_bcd(op, 1); else op_alu_std(op, 0x8); break;
    case 0x9: op_alu_std(op, 0x9); break;
    case 0xA:
      if (!m68k_aline_hook || !m68k_aline_hook(op)) exception(EXC_LINEA, g_start_pc);
      break;
    case 0xB: op_alu_std(op, 0xB); break;
    case 0xC:
      if (((op >> 4) & 0x1F) == 0x10 && ((op >> 6) & 3) == 0) { op_bcd(op, 0); break; }
      if ((op & 0x0130) == 0x0100 && ((op >> 6) & 7) != 3 && ((op >> 6) & 7) != 7) {  /* EXG */
        int rx = (op >> 9) & 7, ry = op & 7, m2 = (op >> 3) & 0x1F; uint32_t t;
        if (m2 == 0x08) { t = M68K_DREG(rx); M68K_DREG(rx) = M68K_DREG(ry); M68K_DREG(ry) = t; }
        else if (m2 == 0x09) { t = M68K_AREG(rx); M68K_AREG(rx) = M68K_AREG(ry); M68K_AREG(ry) = t; }
        else if (m2 == 0x11) { t = M68K_DREG(rx); M68K_DREG(rx) = M68K_AREG(ry); M68K_AREG(ry) = t; }
        else op_alu_std(op, 0xC);
        break;
      }
      op_alu_std(op, 0xC); break;
    case 0xD: op_alu_std(op, 0xD); break;
    case 0xE: op_shift(op); break;
    case 0xF: exception(EXC_LINEF, g_start_pc); break;
  }
}

void m68k_step(void){
  int lvl = m68kc.irq & 7;
  if (lvl && (lvl == 7 || lvl > (int)((m68kc.sr >> 8) & 7))) { take_interrupt(lvl); return; }
  if (m68kc.stopped) return;
  g_start_pc = m68kc.pc;
  m68k_exec(fetch16());
}
