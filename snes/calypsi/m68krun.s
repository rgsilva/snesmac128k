;;; The 68000 interpreter's inner loop + native hot-op handlers, in hand
;;; 65816 (docs/PERF.md items 5c/5d).
;;;
;;; void m68k_run_hot(uint16_t n): per instruction — stopped/irq gate, stamp
;;; m68k_start_pc, fetch-cache hit (tag compare + one [dp],y read + xba), PC
;;; bump, then dispatch on the opcode's top byte through a range-test compare
;;; chain. A matched top byte runs a native handler (an exact flag-for-flag
;;; mirror of the C semantics); everything else `punt`s: jsl into the C
;;; decoder m68k_exec with the opcode in A. The punt-before-mutate rule:
;;; a handler may only jump to `punt` while machine state is exactly
;;; "PC past the opcode word, nothing else touched" — which is precisely
;;; the state m68k_exec expects.
;;;
;;; Native coverage: NOP; MOVEQ (all 16 top bytes — the C decodes bit 8 as
;;; don't-care, mirrored here); the full Bcc/BRA set (all 14 conditions +
;;; BRA; BSR punts); ADDQ/SUBQ #d,An (no flags); DBRA (DBF); and the long
;;; register-direct ALU — ADD/SUB/CMP/AND/OR.L Dn,Dn and ADDQ/SUBQ.L #d,Dn,
;;; with N/V/Z/C/X taken from the 65816 ALU (item 5e). Slow path (pending irq,
;;; STOP, fetch-cache miss, extension word beyond the cached window): the full
;;; C m68k_step / m68k_exec. Still punting — next candidates: byte/word ALU,
;;; the memory-EA forms (MOVE/CMPI/...), and RTS/JSR.
;;;
;;; Everything lives in the direct page (item 5a): m68kc (pc +64, sr +76,
;;; stopped|irq word +78; sizeof==80 packed, verified), the fetch cache,
;;; the timers. No private machine state -> C fallbacks need no sync.
;;; This section (asmhot) is pinned to the single-bank HiROM-c0 memory.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern m68kc, m68k_start_pc
              .extern mac_fetch_tag, mac_fetch_base
              .extern mac_via_t1, mac_via_t2, mac_vbl_timer
              .extern m68k_exec, m68k_step, mac_vbl_fire

PC_LO         .equ    64              ; m68k_t offsets (packed, verified)
PC_HI         .equ    66
SR_OFF        .equ    76
STOPIRQ       .equ    78              ; stopped byte | irq byte, one 16-bit load

              .section asmhot

              .public m68k_run_hot
m68k_run_hot: php
              rep     #0x30           ; 16-bit A/X/Y throughout
              sta     dp:.tiny run_n
step_loop:    lda     dp:.tiny (m68kc+STOPIRQ)
              beq     fast            ; stopped, or an irq is pending: C decides
              jmp     long:slow
fast:
              ;; m68k_start_pc = pc  (exceptions inside m68k_exec push it)
              lda     dp:.tiny (m68kc+PC_LO)
              sta     dp:.tiny m68k_start_pc
              lda     dp:.tiny (m68kc+PC_HI)
              sta     dp:.tiny (m68k_start_pc+2)
              ;; fetch-cache hit test: (pc & 0xFFFFE000) == mac_fetch_tag
              cmp     dp:.tiny (mac_fetch_tag+2)      ; A is still pc-hi
              bne     slow_j                          ; miss: C refills
              lda     dp:.tiny (m68kc+PC_LO)
              and     ##0xe000
              cmp     dp:.tiny mac_fetch_tag
              beq     fetch_ok
slow_j:       jmp     long:slow                       ; (slow is now >127 away)
fetch_ok:     ;; opcode = big-endian word at fetch_base + (pc & 0x1FFF)
              lda     dp:.tiny (m68kc+PC_LO)
              and     ##0x1fff
              tay
              lda     [.tiny mac_fetch_base],y
              xba                     ; 68000 words are big-endian
              sta     dp:.tiny cur_op
              ;; pc += 2
              lda     dp:.tiny (m68kc+PC_LO)
              clc
              adc     ##2
              sta     dp:.tiny (m68kc+PC_LO)
              bcc     pc_ok
              inc     dp:.tiny (m68kc+PC_HI)
pc_ok:        ;; dispatch on the opcode's top byte — a chain of range tests
              ;; (a jmp (abs,X) table needs in-bank 16-bit entries the linker
              ;; can't relocate from 24-bit symbols; at this size the chain
              ;; costs the same). Ordered by hotness in real 68000 code.
              lda     dp:.tiny cur_op
              xba
              and     ##0x00ff
              cmp     ##0x60          ; $60-$6F: Bcc / BRA / BSR
              bcc     d1
              cmp     ##0x70
              bcs     d1
              jmp     long:h_bcc
d1:           cmp     ##0x70          ; $70-$7F: MOVEQ
              bcc     d2
              cmp     ##0x80
              bcs     d2
              jmp     long:h_moveq
d2:           cmp     ##0x50          ; $50-$5F: ADDQ/SUBQ/Scc/DBcc
              bcc     d3
              cmp     ##0x60
              bcs     d3
              jmp     long:h_5x
d3:           cmp     ##0x4e          ; $4E: NOP (RTS/JSR/... punt)
              bne     d4
              jmp     long:h_4e
d4:           cmp     ##0x80          ; $80-$DF: register-direct ALU
              bcc     punt            ; (h_alu validates: opmode 2 long, mode 0 Dn)
              cmp     ##0xe0
              bcs     punt
              jmp     long:h_alu

punt:         lda     dp:.tiny cur_op
              jsl     long:m68k_exec  ; the C decoder (op in A)
tick:         dec     dp:.tiny mac_via_t1
              dec     dp:.tiny mac_via_t2
              dec     dp:.tiny mac_vbl_timer
              bne     next
              jsl     long:mac_vbl_fire
next:         dec     dp:.tiny run_n
              beq     done
              jmp     long:step_loop
done:         plp
              rtl
slow:         jsl     long:m68k_step  ; full C step: irq entry / STOP / refill
              bra     tick

;;; ---- native handlers (each an exact mirror of the C semantics) ----

;; 0x4E: NOP (0x4E71) native; everything else (RTS/JSR/RTE/...) punts.
h_4e:         lda     dp:.tiny cur_op
              cmp     ##0x4e71
              beq     nj1
              jmp     long:punt
nj1:
              jmp     long:tick

;; 0x70-0x7F: MOVEQ #imm8,Dn — reg = bits 9-11 (bit 8 is don't-care in the
;; C decode, mirrored). D[reg] = sext8(imm); N/Z from the value, V=C=0.
h_moveq:      lda     dp:.tiny cur_op
              xba
              and     ##0x000e        ; (top byte >> 1) & 7, times 2
              asl     a               ; reg * 4 = D-reg offset
              tax
              lda     dp:.tiny cur_op
              and     ##0x00ff
              bit     ##0x0080
              bne     mq_neg
              sta     dp:.tiny (m68kc+0),x        ; D[reg] lo
              stz     dp:.tiny (m68kc+2),x        ; D[reg] hi
              ;; flags: V=C=0; N=0; Z = (byte == 0)
              tay
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0xfff0
              cpy     ##0
              bne     mq_wr
              ora     ##0x0004        ; Z
mq_wr:        sta     dp:.tiny (m68kc+SR_OFF)
              jmp     long:tick
mq_neg:       ora     ##0xff00        ; sign-extend
              sta     dp:.tiny (m68kc+0),x
              lda     ##0xffff
              sta     dp:.tiny (m68kc+2),x
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0xfff0
              ora     ##0x0008        ; N (value is negative, never zero)
              sta     dp:.tiny (m68kc+SR_OFF)
              jmp     long:tick

;; $60-$6F: Bcc / BRA (cond 0, always) / BSR (cond 1 -> punt: it pushes a
;; return address). cond = op bits 11-8 = the top byte's low nibble. A byte
;; displacement of 0 selects the 16-bit form (punt: the ext word is read in C;
;; an untaken 16-bit form must still swallow it). Every taken branch shares
;; br_take: pc += sext8(disp), base = pc past the opcode. The per-condition
;; tests mirror cond_true() in m68k.c exactly. SR bits: C=1 V=2 Z=4 N=8.
;; Each ends by jmp long to bcc_take / bcc_nt (far); short local trampolines
;; (_t / _n) bridge the in-section +-127 branch range.
h_bcc:        lda     dp:.tiny cur_op
              xba
              and     ##0x000f        ; A = cond (0..15)
              bne     bcc_c1
              jmp     long:bcc_take   ; 0  T : BRA (always)
bcc_c1:       cmp     ##1
              bne     bcc_c2
              jmp     long:punt       ; 1  F : BSR -> C
bcc_c2:       cmp     ##2             ; 2  HI: !C && !Z
              bne     bcc_c3
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0005
              beq     bcc_c2t
              jmp     long:bcc_nt
bcc_c2t:      jmp     long:bcc_take
bcc_c3:       cmp     ##3             ; 3  LS: C || Z
              bne     bcc_c4
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0005
              bne     bcc_c3t
              jmp     long:bcc_nt
bcc_c3t:      jmp     long:bcc_take
bcc_c4:       cmp     ##4             ; 4  CC: !C
              bne     bcc_c5
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0001
              beq     bcc_c4t
              jmp     long:bcc_nt
bcc_c4t:      jmp     long:bcc_take
bcc_c5:       cmp     ##5             ; 5  CS: C
              bne     bcc_c6
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0001
              bne     bcc_c5t
              jmp     long:bcc_nt
bcc_c5t:      jmp     long:bcc_take
bcc_c6:       cmp     ##6             ; 6  NE: !Z
              bne     bcc_c7
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0004
              beq     bcc_c6t
              jmp     long:bcc_nt
bcc_c6t:      jmp     long:bcc_take
bcc_c7:       cmp     ##7             ; 7  EQ: Z
              bne     bcc_c8
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0004
              bne     bcc_c7t
              jmp     long:bcc_nt
bcc_c7t:      jmp     long:bcc_take
bcc_c8:       cmp     ##8             ; 8  VC: !V
              bne     bcc_c9
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0002
              beq     bcc_c8t
              jmp     long:bcc_nt
bcc_c8t:      jmp     long:bcc_take
bcc_c9:       cmp     ##9             ; 9  VS: V
              bne     bcc_c10
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0002
              bne     bcc_c9t
              jmp     long:bcc_nt
bcc_c9t:      jmp     long:bcc_take
bcc_c10:      cmp     ##10            ; 10 PL: !N
              bne     bcc_c11
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0008
              beq     bcc_c10t
              jmp     long:bcc_nt
bcc_c10t:     jmp     long:bcc_take
bcc_c11:      cmp     ##11            ; 11 MI: N
              bne     bcc_c12
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x0008
              bne     bcc_c11t
              jmp     long:bcc_nt
bcc_c11t:     jmp     long:bcc_take
bcc_c12:      cmp     ##12            ; 12 GE: N == V
              bne     bcc_c13
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x000a
              beq     bcc_c12t        ; both clear
              cmp     ##0x000a
              beq     bcc_c12t        ; both set
              jmp     long:bcc_nt     ; differ
bcc_c12t:     jmp     long:bcc_take
bcc_c13:      cmp     ##13            ; 13 LT: N != V
              bne     bcc_c14
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0x000a
              beq     bcc_c13n        ; both clear -> N==V
              cmp     ##0x000a
              beq     bcc_c13n        ; both set  -> N==V
              jmp     long:bcc_take   ; differ -> take
bcc_c13n:     jmp     long:bcc_nt
bcc_c14:      cmp     ##14            ; 14 GT: !Z && (N == V)
              bne     bcc_c15
              lda     dp:.tiny (m68kc+SR_OFF)
              bit     ##0x0004
              bne     bcc_c14n        ; Z set -> not taken
              and     ##0x000a
              beq     bcc_c14t        ; N==V (both clear)
              cmp     ##0x000a
              beq     bcc_c14t        ; N==V (both set)
bcc_c14n:     jmp     long:bcc_nt
bcc_c14t:     jmp     long:bcc_take
bcc_c15:      ;; 15 LE: Z || (N != V)   (cond is 15 here by elimination)
              lda     dp:.tiny (m68kc+SR_OFF)
              bit     ##0x0004
              bne     bcc_c15t        ; Z set -> take
              and     ##0x000a
              beq     bcc_c15n        ; N==V -> not taken
              cmp     ##0x000a
              beq     bcc_c15n
bcc_c15t:     jmp     long:bcc_take   ; N!=V -> take
bcc_c15n:     jmp     long:bcc_nt

;; taken: disp8 must be nonzero (0 => 16-bit form -> punt). Fall into br_take.
bcc_take:     lda     dp:.tiny cur_op
              and     ##0x00ff
              beq     bcc_punt
br_take:      bit     ##0x0080        ; A = disp8 (nonzero); sign-extend, add pc
              bne     br_neg
              clc
              adc     dp:.tiny (m68kc+PC_LO)
              sta     dp:.tiny (m68kc+PC_LO)
              bcc     br_done
              inc     dp:.tiny (m68kc+PC_HI)
br_done:      jmp     long:tick
br_neg:       ora     ##0xff00
              clc
              adc     dp:.tiny (m68kc+PC_LO)
              sta     dp:.tiny (m68kc+PC_LO)
              bcs     br_done         ; carry set: no borrow into the hi word
              dec     dp:.tiny (m68kc+PC_HI)
              jmp     long:tick
bcc_punt:     jmp     long:punt
;; not taken: byte form does nothing; the 16-bit form (disp8==0) still has to
;; swallow its extension word -> punt to C.
bcc_nt:       lda     dp:.tiny cur_op
              and     ##0x00ff
              beq     bcc_punt
              jmp     long:tick

;; 0x51 with low byte C8-CF: DBRA (DBF) Dn,disp16. Anything else punts.
;; D[reg].w -= 1; if it didn't wrap to FFFF, pc = (address of the extension
;; word) + disp — i.e. current pc + disp; else pc += 2 (skip the extension).
h_51:         lda     dp:.tiny cur_op
              and     ##0x00f8
              cmp     ##0x00c8
              beq     nj5
              jmp     long:punt
nj5:
              ;; the extension word must be inside the cached window
              lda     dp:.tiny (m68kc+PC_LO)
              and     ##0x1fff
              bne     nj102
              jmp     long:punt
nj102:
              tay
              lda     dp:.tiny cur_op
              and     ##0x0007
              asl     a
              asl     a
              tax                     ; D-reg offset
              lda     dp:.tiny (m68kc+0),x
              dec     a
              sta     dp:.tiny (m68kc+0),x        ; word write, hi untouched
              cmp     ##0xffff
              beq     db_exit         ; counter expired: fall through
              lda     [.tiny mac_fetch_base],y    ; disp16, big-endian
              xba
              bit     ##0x8000
              bne     db_neg
              clc
              adc     dp:.tiny (m68kc+PC_LO)
              sta     dp:.tiny (m68kc+PC_LO)
              bcc     db_done
              inc     dp:.tiny (m68kc+PC_HI)
db_done:      jmp     long:tick
db_neg:       clc
              adc     dp:.tiny (m68kc+PC_LO)
              sta     dp:.tiny (m68kc+PC_LO)
              bcs     db_done
              dec     dp:.tiny (m68kc+PC_HI)
              jmp     long:tick
db_exit:      lda     dp:.tiny (m68kc+PC_LO)      ; skip the extension word
              clc
              adc     ##2
              sta     dp:.tiny (m68kc+PC_LO)
              bcc     db_done
              inc     dp:.tiny (m68kc+PC_HI)
              jmp     long:tick

;;; ---- $50-$5F: ADDQ/SUBQ #d,<ea>  (and Scc/DBcc, size field == 3) ----
;; Native: ADDQ/SUBQ to An (mode 1: An += +-data, full 32-bit, NO flags — the
;; 68000 doesn't set CC for An dests) and DBRA (size==3, $51) via h_51. ADDQ/
;; SUBQ to Dn or memory need the ALU flag path, so they punt; Scc and every
;; DBcc other than DBRA punt. data = op bits 11-9 (0 encodes 8), sub = bit 8.
h_5x:         lda     dp:.tiny cur_op
              and     ##0x00c0        ; size field (bits 7-6)
              cmp     ##0x00c0
              bne     addq_go
              lda     dp:.tiny cur_op ; size==3: Scc/DBcc — only DBRA ($51) native
              xba
              and     ##0x00ff
              cmp     ##0x51
              beq     h5_dbra
              jmp     long:punt
h5_dbra:      jmp     long:h_51
addq_go:      lda     dp:.tiny cur_op
              and     ##0x0038        ; mode field (bits 5-3)
              bne     addq_go1        ; not mode 0
              jmp     long:addq_dn    ; mode 0 = Dn (flag-bearing: item 5e)
addq_go1:     cmp     ##0x0008        ; mode 1 = An (no flags)
              beq     addq_an
              jmp     long:punt       ; memory: to C
addq_an:      lda     dp:.tiny cur_op
              and     ##0x0007        ; An index
              asl     a
              asl     a
              clc
              adc     ##32            ; a[] file offset (a[0] at +32)
              tax
              lda     dp:.tiny cur_op
              xba
              and     ##0x000e        ; (top byte & 0x0E) = data<<1
              lsr     a               ; data (0..7)
              bne     addq_d1
              lda     ##8             ; 0 encodes 8
addq_d1:      sta     dp:.tiny addq_data
              lda     dp:.tiny cur_op
              and     ##0x0100        ; sub bit (opcode bit 8)
              bne     addq_sub
              lda     dp:.tiny addq_data          ; ADDQ: An += data (32-bit)
              clc
              adc     dp:.tiny (m68kc+0),x
              sta     dp:.tiny (m68kc+0),x
              bcc     addq_done
              inc     dp:.tiny (m68kc+2),x
addq_done:    jmp     long:tick
addq_sub:     lda     dp:.tiny (m68kc+0),x         ; SUBQ: An -= data (32-bit)
              sec
              sbc     dp:.tiny addq_data
              sta     dp:.tiny (m68kc+0),x
              bcs     addq_done
              dec     dp:.tiny (m68kc+2),x
              jmp     long:tick

;;; ---- $80-$DF: register-direct ALU, long (item 5e) ----
;; <op>.L D[reg],D[dn]  for opmode 2 (long, <ea> op Dn) and mode 0 (Dn source):
;; OR($8) SUB($9) CMP($B) AND($C) ADD($D). The 65816 ALU computes N/V/Z/C for
;; us (two 16-bit ADC/SBC == one 32-bit op); we translate to the 68000 SR. For
;; SUB/CMP the 68000 C is a borrow = !(65816 carry), captured by inverting the
;; branch. CMP writes no result and leaves X; AND/OR clear V/C and leave X; the
;; rest set X=C. Anything not opmode2/mode0 (MULU/DIVU/EXG/ABCD, memory or An
;; forms, byte/word sizes) punts to C. dp,X is X-only, so the source Dn is
;; staged to scratch first, then X switches to the destination Dn.
h_alu:        lda     dp:.tiny cur_op
              and     ##0x01c0        ; opmode (bits 8-6)
              cmp     ##0x0080        ; opmode 2 = long, <ea> op Dn
              beq     h_alu_l
              jmp     long:punt
h_alu_l:      lda     dp:.tiny cur_op
              and     ##0x0038        ; mode (bits 5-3)
              beq     h_alu_reg       ; mode 0 = Dn source
              jmp     long:punt
h_alu_reg:    lda     dp:.tiny cur_op ; stage src = D[op&7] -> scratch
              and     ##0x0007
              asl     a
              asl     a
              tax
              lda     dp:.tiny (m68kc+0),x
              sta     dp:.tiny s_lo
              lda     dp:.tiny (m68kc+2),x
              sta     dp:.tiny s_hi
              lda     dp:.tiny cur_op ; X = dest D[(op>>9)&7]
              xba
              and     ##0x000e
              asl     a
              tax
              lda     dp:.tiny cur_op ; dispatch on the op (top nibble)
              xba
              and     ##0x00f0
              cmp     ##0x00d0
              bne     alu_d1
              jmp     long:alu_add
alu_d1:       cmp     ##0x0090
              bne     alu_d2
              jmp     long:alu_sub
alu_d2:       cmp     ##0x00b0
              bne     alu_d3
              jmp     long:alu_cmp
alu_d3:       cmp     ##0x00c0
              bne     alu_d4
              jmp     long:alu_and
alu_d4:       cmp     ##0x0080
              bne     alu_d5
              jmp     long:alu_or
alu_d5:       jmp     long:punt       ; $A0-$AF etc.: not an ALU form
alu_add:      lda     dp:.tiny (m68kc+0),x
              clc
              adc     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              adc     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              stz     dp:.tiny cvflags
              bcc     aa_nc
              lda     dp:.tiny cvflags
              ora     ##0x0001
              sta     dp:.tiny cvflags
aa_nc:        bvc     aa_nv
              lda     dp:.tiny cvflags
              ora     ##0x0002
              sta     dp:.tiny cvflags
aa_nv:        jmp     long:commit_arith_l
alu_sub:      lda     dp:.tiny (m68kc+0),x
              sec
              sbc     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              sbc     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              stz     dp:.tiny cvflags
              bcs     as_nc           ; 65816 C set => no borrow => 68000 C=0
              lda     dp:.tiny cvflags
              ora     ##0x0001
              sta     dp:.tiny cvflags
as_nc:        bvc     as_nv
              lda     dp:.tiny cvflags
              ora     ##0x0002
              sta     dp:.tiny cvflags
as_nv:        jmp     long:commit_arith_l
alu_cmp:      lda     dp:.tiny (m68kc+0),x
              sec
              sbc     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              sbc     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              stz     dp:.tiny cvflags
              bcs     ac_nc
              lda     dp:.tiny cvflags
              ora     ##0x0001
              sta     dp:.tiny cvflags
ac_nc:        bvc     ac_nv
              lda     dp:.tiny cvflags
              ora     ##0x0002
              sta     dp:.tiny cvflags
ac_nv:        jmp     long:commit_cmp_l
alu_and:      lda     dp:.tiny (m68kc+0),x
              and     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              and     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              jmp     long:commit_logic_l
alu_or:       lda     dp:.tiny (m68kc+0),x
              ora     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              ora     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              jmp     long:commit_logic_l

;;; ---- ADDQ/SUBQ.L #d,Dn (item 5e; entered from h_5x addq_go) ----
addq_dn:      lda     dp:.tiny cur_op
              and     ##0x00c0        ; size (bits 7-6)
              cmp     ##0x0080        ; size 2 = long only (byte/word punt)
              beq     addq_dn_l
              jmp     long:punt
addq_dn_l:    lda     dp:.tiny cur_op ; X = dest D[op&7]
              and     ##0x0007
              asl     a
              asl     a
              tax
              lda     dp:.tiny cur_op ; data = (top byte & 0x0E)>>1, 0->8
              xba
              and     ##0x000e
              lsr     a
              bne     adn_d1
              lda     ##8
adn_d1:       sta     dp:.tiny s_lo
              stz     dp:.tiny s_hi
              lda     dp:.tiny cur_op
              and     ##0x0100        ; sub bit
              bne     subq_dn_go
              lda     dp:.tiny (m68kc+0),x   ; ADDQ.L
              clc
              adc     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              adc     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              stz     dp:.tiny cvflags
              bcc     adn_nc
              lda     dp:.tiny cvflags
              ora     ##0x0001
              sta     dp:.tiny cvflags
adn_nc:       bvc     adn_nv
              lda     dp:.tiny cvflags
              ora     ##0x0002
              sta     dp:.tiny cvflags
adn_nv:       jmp     long:commit_arith_l
subq_dn_go:   lda     dp:.tiny (m68kc+0),x   ; SUBQ.L
              sec
              sbc     dp:.tiny s_lo
              sta     dp:.tiny r_lo
              lda     dp:.tiny (m68kc+2),x
              sbc     dp:.tiny s_hi
              sta     dp:.tiny r_hi
              stz     dp:.tiny cvflags
              bcs     sdn_nc
              lda     dp:.tiny cvflags
              ora     ##0x0001
              sta     dp:.tiny cvflags
sdn_nc:       bvc     sdn_nv
              lda     dp:.tiny cvflags
              ora     ##0x0002
              sta     dp:.tiny cvflags
sdn_nv:       jmp     long:commit_arith_l

;;; ---- ALU flag-commit tails (long) — SR bits X=10 N=8 Z=4 V=2 C=1 ----
;; commit_arith_l: writeback r -> D[X]; N/Z from r, V/C from cvflags, X=C.
commit_arith_l: lda   dp:.tiny r_lo
              sta     dp:.tiny (m68kc+0),x
              lda     dp:.tiny r_hi
              sta     dp:.tiny (m68kc+2),x
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0xffe0        ; clear X,N,Z,V,C
              sta     dp:.tiny sr_tmp
              lda     dp:.tiny cvflags
              and     ##0x0001
              beq     ca_noc
              lda     dp:.tiny sr_tmp
              ora     ##0x0011        ; C + X
              sta     dp:.tiny sr_tmp
ca_noc:       lda     dp:.tiny cvflags
              and     ##0x0002
              beq     ca_nov
              lda     dp:.tiny sr_tmp
              ora     ##0x0002        ; V
              sta     dp:.tiny sr_tmp
ca_nov:       lda     dp:.tiny r_hi
              bpl     ca_non
              lda     dp:.tiny sr_tmp
              ora     ##0x0008        ; N (bit 31)
              sta     dp:.tiny sr_tmp
ca_non:       lda     dp:.tiny r_lo
              ora     dp:.tiny r_hi
              bne     ca_noz
              lda     dp:.tiny sr_tmp
              ora     ##0x0004        ; Z (all 32 bits)
              sta     dp:.tiny sr_tmp
ca_noz:       lda     dp:.tiny sr_tmp
              sta     dp:.tiny (m68kc+SR_OFF)
              jmp     long:tick

;; commit_cmp_l: N/Z from r, V/C from cvflags; NO writeback, X untouched.
commit_cmp_l: lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0xfff0        ; clear N,Z,V,C ; keep X
              sta     dp:.tiny sr_tmp
              lda     dp:.tiny cvflags
              and     ##0x0001
              beq     cc_noc
              lda     dp:.tiny sr_tmp
              ora     ##0x0001        ; C only (CMP leaves X)
              sta     dp:.tiny sr_tmp
cc_noc:       lda     dp:.tiny cvflags
              and     ##0x0002
              beq     cc_nov
              lda     dp:.tiny sr_tmp
              ora     ##0x0002
              sta     dp:.tiny sr_tmp
cc_nov:       lda     dp:.tiny r_hi
              bpl     cc_non
              lda     dp:.tiny sr_tmp
              ora     ##0x0008
              sta     dp:.tiny sr_tmp
cc_non:       lda     dp:.tiny r_lo
              ora     dp:.tiny r_hi
              bne     cc_noz
              lda     dp:.tiny sr_tmp
              ora     ##0x0004
              sta     dp:.tiny sr_tmp
cc_noz:       lda     dp:.tiny sr_tmp
              sta     dp:.tiny (m68kc+SR_OFF)
              jmp     long:tick

;; commit_logic_l: writeback r -> D[X]; N/Z from r, V=C=0, X untouched.
commit_logic_l: lda   dp:.tiny r_lo
              sta     dp:.tiny (m68kc+0),x
              lda     dp:.tiny r_hi
              sta     dp:.tiny (m68kc+2),x
              lda     dp:.tiny (m68kc+SR_OFF)
              and     ##0xfff0        ; clear N,Z,V,C ; keep X (V=C=0)
              sta     dp:.tiny sr_tmp
              lda     dp:.tiny r_hi
              bpl     cl_non
              lda     dp:.tiny sr_tmp
              ora     ##0x0008
              sta     dp:.tiny sr_tmp
cl_non:       lda     dp:.tiny r_lo
              ora     dp:.tiny r_hi
              bne     cl_noz
              lda     dp:.tiny sr_tmp
              ora     ##0x0004
              sta     dp:.tiny sr_tmp
cl_noz:       lda     dp:.tiny sr_tmp
              sta     dp:.tiny (m68kc+SR_OFF)
              jmp     long:tick

;;; ---- the 256-entry top-byte dispatch table (16-bit in-bank addresses) ----
optab:

              .section ztiny, bss
run_n:        .space  2
cur_op:       .space  2
addq_data:    .space  2
;; ALU (item 5e) scratch: source operand, 32-bit result, captured C/V, SR temp
s_lo:         .space  2
s_hi:         .space  2
r_lo:         .space  2
r_hi:         .space  2
cvflags:      .space  2       ; bit0 = 68000 C, bit1 = V (as captured)
sr_tmp:       .space  2
