;;; Hot copy loops for the Mac video path + RAM init, in hand 65816 —
;;; docs/PERF.md: the C refresh() compiled to ~45 instructions per byte
;;; (measured: ~25 frames = 0.4 s per redraw); this is ~11 clocks per byte.
;;;
;;; tile_rows     — rebuild the 32x28 tile buffer from the Mac framebuffer:
;;;                 for each of 224 fb rows, copy 32 bytes from WRAM bank $7F
;;;                 (row offset $A700 + (tile_pan_y + row)*64 + tile_pan_xb)
;;;                 into the SRAM tile buffer with stride 16 (plane 0 of the
;;;                 2bpp tiles). Tiles 0-511 -> $B0:6000, 512-895 -> $B1:6000.
;;;                 Fully unrolled inner copy: Y walks the fb (lda $A700+k,y
;;;                 with DBR=$7F), X is the tile-row base (sta long:TB+16k,x —
;;;                 long-indexed is X-only on the 65816, hence the role split).
;;; mac_ram_clear_all — zero all 128 KB of Mac RAM: seed two bytes, then let
;;;                 an overlapping ascending MVN propagate them (7 clk/byte).
;;; mac_fastboot_load — copy the 128 KB warm-state image (cart $D8:0000-
;;;                 $D9:FFFF, linear) into Mac RAM with three MVNs.
;;;
;;; No arguments (Calypsi cc dodged): pan comes from two C globals. Uses
;;; A/X/Y + DBR only (saved); direct page untouched; callers run with
;;; interrupts off (these ROMs never enable NMI/IRQ). MVN operands are
;;; .byte-encoded (0x54, dst, src) to dodge assembler-dialect ambiguity.

              .rtmodel version, "1"
              .rtmodel core, "*"


              .section farcode

;;; --------------------------------------------------------------- tile_rows
              .public tile_rows
tile_rows:    phb
              php
              rep     #0x30           ; 16-bit A/X/Y
              lda     ##0x7f7f
              pha
              plb                     ; DBR = $7F (the framebuffer bank)
              plb
              lda     ##0
              sta     long:trow_cur
row_loop:     ;; Y = fb offset above $A700: (tile_pan_y + row) * 64 + tile_pan_xb
              lda     long:trow_cur
              clc
              adc     long:tile_pan_y
              asl     a
              asl     a
              asl     a
              asl     a
              asl     a
              asl     a
              clc
              adc     long:tile_pan_xb
              tay
              ;; X = tile-row base within the half: (trow & 15)*512 + r*2
              lda     long:trow_cur
              and     ##7
              asl     a
              sta     long:dst_tmp
              lda     long:trow_cur
              lsr     a
              lsr     a
              lsr     a               ; tile row 0-27
              cmp     ##16
              bcs     second_half
              xba                     ; <<9: byte swap then <<1 (A < 16)
              asl     a
              clc
              adc     long:dst_tmp
              tax
              jsl     copy32_tb0
              bra     row_next
second_half:  sec
              sbc     ##16
              xba
              asl     a
              clc
              adc     long:dst_tmp
              tax
              jsl     copy32_tb1
row_next:     lda     long:trow_cur
              inc     a
              sta     long:trow_cur
              cmp     ##224
              bne     row_loop
              plp
              plb
              rtl

copy32_tb0:
              sep     #0x20
              lda     0xa700,y
              sta     long:0xb06000,x
              lda     0xa701,y
              sta     long:0xb06010,x
              lda     0xa702,y
              sta     long:0xb06020,x
              lda     0xa703,y
              sta     long:0xb06030,x
              lda     0xa704,y
              sta     long:0xb06040,x
              lda     0xa705,y
              sta     long:0xb06050,x
              lda     0xa706,y
              sta     long:0xb06060,x
              lda     0xa707,y
              sta     long:0xb06070,x
              lda     0xa708,y
              sta     long:0xb06080,x
              lda     0xa709,y
              sta     long:0xb06090,x
              lda     0xa70a,y
              sta     long:0xb060a0,x
              lda     0xa70b,y
              sta     long:0xb060b0,x
              lda     0xa70c,y
              sta     long:0xb060c0,x
              lda     0xa70d,y
              sta     long:0xb060d0,x
              lda     0xa70e,y
              sta     long:0xb060e0,x
              lda     0xa70f,y
              sta     long:0xb060f0,x
              lda     0xa710,y
              sta     long:0xb06100,x
              lda     0xa711,y
              sta     long:0xb06110,x
              lda     0xa712,y
              sta     long:0xb06120,x
              lda     0xa713,y
              sta     long:0xb06130,x
              lda     0xa714,y
              sta     long:0xb06140,x
              lda     0xa715,y
              sta     long:0xb06150,x
              lda     0xa716,y
              sta     long:0xb06160,x
              lda     0xa717,y
              sta     long:0xb06170,x
              lda     0xa718,y
              sta     long:0xb06180,x
              lda     0xa719,y
              sta     long:0xb06190,x
              lda     0xa71a,y
              sta     long:0xb061a0,x
              lda     0xa71b,y
              sta     long:0xb061b0,x
              lda     0xa71c,y
              sta     long:0xb061c0,x
              lda     0xa71d,y
              sta     long:0xb061d0,x
              lda     0xa71e,y
              sta     long:0xb061e0,x
              lda     0xa71f,y
              sta     long:0xb061f0,x
              rep     #0x20
              rtl
copy32_tb1:
              sep     #0x20
              lda     0xa700,y
              sta     long:0xb16000,x
              lda     0xa701,y
              sta     long:0xb16010,x
              lda     0xa702,y
              sta     long:0xb16020,x
              lda     0xa703,y
              sta     long:0xb16030,x
              lda     0xa704,y
              sta     long:0xb16040,x
              lda     0xa705,y
              sta     long:0xb16050,x
              lda     0xa706,y
              sta     long:0xb16060,x
              lda     0xa707,y
              sta     long:0xb16070,x
              lda     0xa708,y
              sta     long:0xb16080,x
              lda     0xa709,y
              sta     long:0xb16090,x
              lda     0xa70a,y
              sta     long:0xb160a0,x
              lda     0xa70b,y
              sta     long:0xb160b0,x
              lda     0xa70c,y
              sta     long:0xb160c0,x
              lda     0xa70d,y
              sta     long:0xb160d0,x
              lda     0xa70e,y
              sta     long:0xb160e0,x
              lda     0xa70f,y
              sta     long:0xb160f0,x
              lda     0xa710,y
              sta     long:0xb16100,x
              lda     0xa711,y
              sta     long:0xb16110,x
              lda     0xa712,y
              sta     long:0xb16120,x
              lda     0xa713,y
              sta     long:0xb16130,x
              lda     0xa714,y
              sta     long:0xb16140,x
              lda     0xa715,y
              sta     long:0xb16150,x
              lda     0xa716,y
              sta     long:0xb16160,x
              lda     0xa717,y
              sta     long:0xb16170,x
              lda     0xa718,y
              sta     long:0xb16180,x
              lda     0xa719,y
              sta     long:0xb16190,x
              lda     0xa71a,y
              sta     long:0xb161a0,x
              lda     0xa71b,y
              sta     long:0xb161b0,x
              lda     0xa71c,y
              sta     long:0xb161c0,x
              lda     0xa71d,y
              sta     long:0xb161d0,x
              lda     0xa71e,y
              sta     long:0xb161e0,x
              lda     0xa71f,y
              sta     long:0xb161f0,x
              rep     #0x20
              rtl

;;; ------------------------------------------------- mac_ram_clear_all
;;; Zero Mac RAM: $A0:6000-7FFF (Mac $0000-$1FFF) + $7E:2000-FFFF +
;;; $7F:0000-FFFF (Mac $2000-$1FFFF). Seed a zero word, then overlapping
;;; ascending MVN (dst = src+2) propagates it across the block.
              .public mac_ram_clear_all
mac_ram_clear_all:
              phb
              php
              rep     #0x30
              lda     ##0
              sta     long:0xa06000
              ldx     ##0x6000
              ldy     ##0x6002
              lda     ##0x1ffd
              .byte   0x54, 0xa0, 0xa0   ; mvn $A0 <- $A0
              lda     ##0
              sta     long:0x7e2000
              ldx     ##0x2000
              ldy     ##0x2002
              lda     ##0xdffd
              .byte   0x54, 0x7e, 0x7e   ; mvn $7E <- $7E
              lda     ##0
              sta     long:0x7f0000
              ldx     ##0
              ldy     ##2
              lda     ##0xfffd
              .byte   0x54, 0x7f, 0x7f   ; mvn $7F <- $7F
              plp
              plb
              rtl

;;; ------------------------------------------------- mac_fastboot_load
;;; Cart $D8:0000-$D9:FFFF (the warm-state RAM image) -> Mac RAM.
              .public mac_fastboot_load
mac_fastboot_load:
              phb
              php
              rep     #0x30
              ldx     ##0x0000        ; $D8:0000-1FFF -> $A0:6000 (Mac low 8 KB)
              ldy     ##0x6000
              lda     ##0x1fff
              .byte   0x54, 0xa0, 0xd8
              ldx     ##0x2000        ; $D8:2000-FFFF -> $7E:2000 (Mac $2000+)
              ldy     ##0x2000
              lda     ##0xdfff
              .byte   0x54, 0x7e, 0xd8
              ldx     ##0x0000        ; $D9:0000-FFFF -> $7F:0000 (Mac $10000+)
              ldy     ##0x0000
              lda     ##0xffff
              .byte   0x54, 0x7f, 0xd9
              plp
              plb
              rtl

              .section zfar, bss
              .public tile_pan_xb, tile_pan_y
tile_pan_xb:  .space  2
tile_pan_y:   .space  2
trow_cur:     .space  2
dst_tmp:      .space  2
