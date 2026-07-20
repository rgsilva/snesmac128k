;;; SNES cartridge header and interrupt vectors for snesmac.
;;; Cloned from snespico8/snes/calypsi/header.s (which is based on Calypsi's
;;; stock snes-HiROM rules). ROM+RAM+battery, 256 KB SRAM requested — the Mac's
;;; 128 KB RAM will live there (Mesen backs it; a real cart needs the SRAM).

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; Extended header $FFB0-$FFBF (licensee byte $33 => present)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "SMAC"         ; game code
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $FFC0-$FFDF
              .section snesheader, root
              .ascii  "SNESMAC M1           "   ; 21-byte title
              .byte   0x21           ; map mode: HiROM, slow
              .byte   0x02           ; cartridge type: ROM+RAM+battery
              .byte   0x09           ; ROM size: 512 KB
              .byte   0x08           ; RAM size: 256 KB requested
              .byte   0x01           ; country: USA (NTSC)
              .byte   0x33           ; licensee: extended header present
              .byte   0x00           ; version
              .word   0xaaaa         ; checksum complement (dummy, patched by raw2sfc.py)
              .word   0x5555         ; checksum (dummy)

;;; Native + emulation vectors.
              .section snesvec1, root
              .word   0, 0                  ; $FFE0,$FFE2 unused
              .word   __vector_stub         ; $FFE4 native COP
              .word   __vector_stub         ; $FFE6 native BRK
              .word   __vector_stub         ; $FFE8 native ABORT
              .word   __vector_stub         ; $FFEA native NMI
              .word   0                     ; $FFEC unused
              .word   __vector_stub         ; $FFEE native IRQ
              .word   0, 0                  ; $FFF0,$FFF2 unused
              .word   __vector_stub         ; $FFF4 emulation COP
              .word   0                     ; $FFF6 unused
              .word   __vector_stub         ; $FFF8 emulation ABORT
              .word   __vector_stub         ; $FFFA emulation NMI

              .section snesvec2, root
              .word   __vector_stub         ; $FFFE emulation IRQ/BRK

              .section code, root
              .public __vector_stub
__vector_stub:
              rti
