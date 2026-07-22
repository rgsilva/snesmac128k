#!/usr/bin/env python3
"""Generate benchrom.bin — snesmac's own 64 KB Macintosh boot ROM.

100% original 68000 code (no Apple bytes), so unlike the real ROM it is
committed and the whole bench pipeline runs anywhere, CI included. It boots
exactly like the real one (reset SP/PC vectors, then drives the VIA to drop
the ROM overlay) and then runs a fixed, fully deterministic series of
workload phases, each exercising one cost centre of the emulator:

  1 nop     fetch + dispatch, nothing else
  2 alu     register-only ADD/EOR/ADDQ/CMP (long)
  3 ram.b   byte writes + reads through the RAM window path
  4 ram.w   word writes + reads (the aligned-word fast-path target)
  5 ram.l   long writes + reads
  6 shift   LSL/LSR/ROL/ROR long (the 32-bit shift helpers)
  7 muldiv  MULU/DIVU (the runtime mul/div helpers)
  8 copy    MOVE.L (A0)+,(A1)+ ROM->RAM (the _BlockMove shape)
  9 fbfill  paints the whole 512x342 framebuffer (visible on the SNES!)
 10 crc     folds regs + RAM samples into D7, stores it, STOPs

Before each phase the ROM writes the phase number to RAM $0100 (word); the
runners watch that word — the host counts instructions per phase, the Mesen
Lua harness counts frames per phase, and frames/instr is the metric that
every optimization in docs/PERF.md gets judged by. The final CRC lands in
RAM $0104 (long) and must match host <-> SNES exactly: the semantic gate.

The emitter below is not a general assembler — just the encodings this
program needs, checked against the same decoder the emulator uses (the
vector-validated core is the reference; if the encoding were wrong, the
host run would veer off and the CRC/phase markers would say so).
"""
import sys

ROM_BASE = 0x400000          # the ROM's runtime address (overlay off)
PHASE_W  = 0x0100            # RAM word: current phase
CRC_L    = 0x0104            # RAM long: final CRC
DONE     = 0xB0CA            # phase-word value when finished
VIA_DDRA = 0xEFE7FE          # VIA reg 3
VIA_ORA  = 0xEFFFFE          # VIA reg 15 — bit 4 low drops the overlay


class Emit:
    """68000 word emitter with a per-phase dynamic-instruction ledger."""
    def __init__(self):
        self.words = []
        self.counts = {}          # phase name -> dynamic instructions
        self.phase = "init"

    def here(self):               # current runtime address
        return ROM_BASE + 0x10 + 2 * len(self.words)

    def w(self, *ws):
        for x in ws:
            self.words.append(x & 0xFFFF)

    def n(self, dyn=1):           # count dyn dynamic instructions
        self.counts[self.phase] = self.counts.get(self.phase, 0) + dyn

    # ---- instructions (each emits + counts one) ----
    def nop(self):                self.w(0x4E71); self.n()
    def moveq(self, imm, dn):     self.w(0x7000 | dn << 9 | (imm & 0xFF)); self.n()
    def move_l_imm_d(self, imm, dn):
        self.w(0x203C | dn << 9, imm >> 16, imm); self.n()
    def movea_l_imm(self, imm, an):
        self.w(0x207C | an << 9, imm >> 16, imm); self.n()
    def move_b_imm_absl(self, imm, addr):
        self.w(0x13FC, imm & 0xFF, addr >> 16, addr); self.n()
    def move_w_imm_absl(self, imm, addr):
        self.w(0x33FC, imm, addr >> 16, addr); self.n()
    def move_l_d_absl(self, dn, addr):
        self.w(0x23C0 | dn, addr >> 16, addr); self.n()
    def move_b_d_ainc(self, dn, an): self.w(0x10C0 | an << 9 | dn); self.n()
    def move_b_ainc_d(self, an, dn): self.w(0x1018 | dn << 9 | an); self.n()
    def move_w_d_ainc(self, dn, an): self.w(0x30C0 | an << 9 | dn); self.n()
    def move_w_ainc_d(self, an, dn): self.w(0x3018 | dn << 9 | an); self.n()
    def move_l_d_ainc(self, dn, an): self.w(0x20C0 | an << 9 | dn); self.n()
    def move_l_ainc_d(self, an, dn): self.w(0x2018 | dn << 9 | an); self.n()
    def move_l_ainc_ainc(self, src, dst): self.w(0x20D8 | dst << 9 | src); self.n()
    def move_w_imm_d(self, imm, dn): self.w(0x303C | dn << 9, imm); self.n()
    def add_l_d_d(self, src, dst):   self.w(0xD080 | dst << 9 | src); self.n()
    def eor_l_d_d(self, src, dst):   self.w(0xB180 | src << 9 | dst); self.n()
    def addq_l(self, imm, dn):       self.w(0x5080 | ((imm & 7) << 9) | dn); self.n()
    def cmp_l_d_d(self, src, dst):   self.w(0xB080 | dst << 9 | src); self.n()
    def lsl_l_imm(self, cnt, dn):    self.w(0xE188 | ((cnt & 7) << 9) | dn); self.n()
    def lsr_l_imm(self, cnt, dn):    self.w(0xE088 | ((cnt & 7) << 9) | dn); self.n()
    def rol_l_imm(self, cnt, dn):    self.w(0xE198 | ((cnt & 7) << 9) | dn); self.n()
    def ror_l_imm(self, cnt, dn):    self.w(0xE098 | ((cnt & 7) << 9) | dn); self.n()
    def mulu(self, src, dst):        self.w(0xC0C0 | dst << 9 | src); self.n()
    def divu(self, src, dst):        self.w(0x80C0 | dst << 9 | src); self.n()
    def stop(self, imm):             self.w(0x4E72, imm); self.n()
    def rte(self):                   self.w(0x4E73); self.n()
    def aline(self, op):             self.w(0xA000 | (op & 0xFFF)); self.n()
    def addq_l_sp2(self):            self.w(0x54AF, 0x0002); self.n()  # ADDQ.L #2,2(A7)
    def move_l_imm_absl(self, imm, addr):
        self.w(0x23FC, imm >> 16, imm, addr >> 16, addr); self.n()

    def label(self):                 return self.here()

    def dbra(self, dn, target):      # count added by the caller (loop-aware)
        # disp is relative to the extension word = opcode address + 2
        self.w(0x51C8 | dn, (target - (self.here() + 2)) & 0xFFFF)

    def bra_s(self, target):         # disp8 relative to opcode address + 2
        self.w(0x6000 | ((target - (self.here() + 2)) & 0xFF))

    # ---- structured helpers ----
    def mark(self, num, name):       # end phase, start the next
        self.phase = name
        self.move_w_imm_absl(num, PHASE_W)

    def loop(self, iters, body_fn, body_count, dn=6):
        """MOVE.W #iters-1,Dn; L: body; DBRA Dn,L  — ledger-aware."""
        self.move_w_imm_d(iters - 1, dn)
        top = self.label()
        body_fn()                                   # emits body once (counts 1x)
        self.dbra(dn, top)
        # body ran `iters` times + `iters` DBRAs; body_fn counted itself once
        self.n((iters - 1) * body_count + iters)


def build():
    a = Emit()

    # ---- boot: drop the overlay the way the real ROM does ----
    a.move_b_imm_absl(0x10, VIA_DDRA)      # DDRA bit 4 = output
    a.move_b_imm_absl(0x00, VIA_ORA)       # drive it low -> RAM at $0
    a.moveq(0, 7)                          # D7 = CRC accumulator
    a.move_l_imm_d(0x12345678, 1)
    a.move_l_imm_d(0x9ABCDEF0, 2)
    a.move_l_imm_d(0x0F0F0F0F, 3)
    a.move_l_imm_d(0x00001000, 5)          # MULU/DIVU operand (word $1000)

    a.mark(1, "nop")
    a.loop(64, lambda: [a.nop() for _ in range(16)], 16)

    a.mark(2, "alu")
    def alu_body():
        for _ in range(4):
            a.add_l_d_d(1, 2); a.eor_l_d_d(2, 3); a.addq_l(1, 1); a.cmp_l_d_d(1, 3)
    a.loop(64, alu_body, 16)

    a.mark(3, "ram.b")
    a.movea_l_imm(0x200, 0); a.movea_l_imm(0x200, 1)
    def ramb_body():
        for _ in range(8): a.move_b_d_ainc(1, 0)
        for _ in range(8): a.move_b_ainc_d(1, 2)
    a.loop(64, ramb_body, 16)              # walks $200-$3FF

    a.mark(4, "ram.w")
    a.movea_l_imm(0x200, 0); a.movea_l_imm(0x200, 1)
    def ramw_body():
        for _ in range(8): a.move_w_d_ainc(1, 0)
        for _ in range(8): a.move_w_ainc_d(1, 2)
    a.loop(64, ramw_body, 16)              # walks $200-$5FF

    a.mark(5, "ram.l")
    a.movea_l_imm(0x200, 0); a.movea_l_imm(0x200, 1)
    def raml_body():
        for _ in range(8): a.move_l_d_ainc(1, 0)
        for _ in range(8): a.move_l_ainc_d(1, 2)
    a.loop(64, raml_body, 16)              # walks $200-$9FF

    a.mark(6, "shift")
    def shift_body():
        for _ in range(4):
            a.lsl_l_imm(1, 1); a.lsr_l_imm(1, 2); a.rol_l_imm(0, 3); a.ror_l_imm(2, 3)
    a.loop(64, shift_body, 16)             # rol cnt 0 encodes 8

    a.mark(7, "muldiv")
    def muldiv_body():
        for _ in range(4):
            a.move_l_imm_d(0x00120034, 0); a.mulu(5, 0)
            a.move_l_imm_d(0x00654321, 2); a.divu(5, 2)
    a.loop(32, muldiv_body, 16)

    a.mark(8, "copy")
    a.movea_l_imm(ROM_BASE + 0x4000, 0); a.movea_l_imm(0x800, 1)
    def copy_body():
        for _ in range(8): a.move_l_ainc_ainc(0, 1)
    a.loop(64, copy_body, 8)               # 2 KB ROM->RAM

    a.mark(9, "fbfill")
    a.movea_l_imm(0x1A700, 0)              # the Mac framebuffer
    a.move_l_imm_d(0xAAAAAAAA, 0)
    def fill_body():
        for _ in range(8): a.move_l_d_ainc(0, 0)
    a.loop(684, fill_body, 8)              # 684*32 = 21888 bytes: whole screen

    a.mark(10, "crc")
    a.movea_l_imm(0x200, 0)                # 16 longs of the RAM buffer
    def crc16l():
        a.move_l_ainc_d(0, 0); a.eor_l_d_d(0, 7); a.rol_l_imm(1, 7)
    a.loop(16, crc16l, 3, dn=6)
    a.movea_l_imm(0x1A700, 0)              # 16 longs of the framebuffer
    a.loop(16, crc16l, 3, dn=6)
    for dn in (1, 2, 3, 4, 5):             # fold the registers in
        a.eor_l_d_d(dn, 7); a.rol_l_imm(1, 7)
    a.move_l_d_absl(7, CRC_L)

    # A-line traps: the hottest special case in a real boot (every OS/Toolbox
    # call). $A123 is declined by the .Sony HLE hook -> line-A exception ->
    # our handler bumps the stacked PC past the trap word and RTEs. Runs
    # AFTER the CRC so the golden value is untouched by the stack traffic.
    a.mark(11, "trap")
    handler = a.here() + 2                 # the RTE island right after the BRA
    a.bra_s(handler + 6)                   # skip it (ADDQ+ext+RTE = 6 bytes)
    a.addq_l_sp2()                         # 68000 pushes the A-line's own PC:
    a.rte()                                #   step past it, return
    assert a.here() == handler + 6
    a.move_l_imm_absl(handler, 0x28)       # vector 10 (line A) at $28
    a.loop(16, lambda: [a.aline(0x123) for _ in range(4)], 12)
    a.n(6)                                 # ledger: handler runs (2x) per A-line
                                           # (emit-time counted once; see loop())

    a.phase = "done"
    a.move_w_imm_absl(DONE, PHASE_W)
    a.stop(0x2700)
    a.bra_s(a.here())                      # safety net: spin, never fall off

    # ---- image ----
    img = bytearray(0x10000)
    img[0:4] = (0x00017F00).to_bytes(4, "big")            # initial SP
    img[4:8] = (ROM_BASE + 0x10).to_bytes(4, "big")       # initial PC
    code = b"".join(w.to_bytes(2, "big") for w in a.words)
    assert 0x10 + len(code) <= 0x4000, "code ran into the copy-source block"
    img[0x10:0x10 + len(code)] = code
    for i in range(0x4000, 0x4800):                        # copy-phase source
        img[i] = (i * 7 + 3) & 0xFF
    return bytes(img), a.counts


if __name__ == "__main__":
    img, counts = build()
    with open(sys.argv[1], "wb") as f:
        f.write(img)
    total = sum(counts.values())
    print(f"benchrom.bin: {len(img)} bytes, ~{total} dynamic instructions")
    for name, c in counts.items():
        print(f"  {name:8s} {c:6d}")
