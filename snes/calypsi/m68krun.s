;;; The 68000 interpreter's inner loop + native hot-op handlers, in hand
;;; 65816 (docs/PERF.md items 5c/5d).
;;;
;;; void m68k_run_hot(uint16_t n): per instruction — stopped/irq gate, stamp
;;; m68k_start_pc, fetch-cache hit (tag compare + one [dp],y read + xba), PC
;;; bump, then dispatch on the opcode's top byte through a 256-entry
;;; jmp (abs,X) table. Entries either run a native handler (an exact
;;; flag-for-flag mirror of the C semantics) or `punt`: jsl into the C
;;; decoder m68k_exec with the opcode in A. The punt-before-mutate rule:
;;; a handler may only jump to `punt` while machine state is exactly
;;; "PC past the opcode word, nothing else touched" — which is precisely
;;; the state m68k_exec expects.
;;;
;;; v1 native coverage: NOP, MOVEQ (all 16 top bytes — the C decodes bit 8
;;; as don't-care, mirrored here), BRA.b, BNE.b, BEQ.b, DBRA (DBF). Slow
;;; path (pending irq, STOP, fetch-cache miss, extension word beyond the
;;; cached window): the full C m68k_step / m68k_exec.
;;;
;;; Everything lives in the direct page (item 5a): m68kc (pc +64, sr +76,
;;; stopped|irq word +78; sizeof==80 packed, verified), the fetch cache,
;;; the timers. No private machine state -> C fallbacks need no sync.
;;; This section (asmhot) is pinned to the single-bank HiROM-c0 memory:
;;; the jump table holds 16-bit in-bank addresses for jmp (abs,X).

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
              bne     slow                            ; miss: C refills
              lda     dp:.tiny (m68kc+PC_LO)
              and     ##0xe000
              cmp     dp:.tiny mac_fetch_tag
              bne     slow
              ;; opcode = big-endian word at fetch_base + (pc & 0x1FFF)
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
pc_ok:        ;; dispatch on the top byte — a compare chain (6 native handler
              ;; groups; a jmp (abs,X) table needs in-bank 16-bit entries the
              ;; linker can't relocate from 24-bit symbols, and at this size
              ;; the chain costs the same)
              lda     dp:.tiny cur_op
              xba
              and     ##0x00ff
              cmp     ##0x60          ; branches first: hottest in real code
              bne     c1
              jmp     long:h_bra
c1:           cmp     ##0x66
              bne     c2
              jmp     long:h_bne
c2:           cmp     ##0x67
              bne     c3
              jmp     long:h_beq
c3:           cmp     ##0x70          ; 0x70-0x7F: all MOVEQ
              bcc     c4
              cmp     ##0x80
              bcs     c4
              jmp     long:h_moveq
c4:           cmp     ##0x4e
              bne     c5
              jmp     long:h_4e
c5:           cmp     ##0x51
              bne     c6
              jmp     long:h_51
c6:

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

;; 0x60: BRA.b — disp8 != 0 (0x00 selects the 16-bit form: punt).
;; pc += sext8(disp); base is the current pc (already past the opcode).
h_bra:        lda     dp:.tiny cur_op
              and     ##0x00ff
              bne     nj2
              jmp     long:punt
nj2:
br_take:      bit     ##0x0080
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

;; 0x66 BNE.b / 0x67 BEQ.b — gate on Z (SR bit 2), then share BRA's tail.
h_bne:        lda     dp:.tiny (m68kc+SR_OFF)
              bit     ##0x0004
              bne     tick2           ; Z set: not taken
              lda     dp:.tiny cur_op
              and     ##0x00ff
              bne     nj101
              jmp     long:punt
nj101:
              jmp     long:br_take
h_beq:        lda     dp:.tiny (m68kc+SR_OFF)
              bit     ##0x0004
              beq     tick2           ; Z clear: not taken
              lda     dp:.tiny cur_op
              and     ##0x00ff
              bne     nj3
              jmp     long:punt
nj3:
              jmp     long:br_take
tick2:        ;; untaken Bcc with the byte form: nothing to do. The 16-bit
              ;; form (low byte 0) must still swallow its extension word -> C.
              lda     dp:.tiny cur_op
              and     ##0x00ff
              bne     nj4
              jmp     long:punt
nj4:
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

;;; ---- the 256-entry top-byte dispatch table (16-bit in-bank addresses) ----
optab:

              .section ztiny, bss
run_n:        .space  2
cur_op:       .space  2
