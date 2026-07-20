/* Macintosh 128K machine model around the lean 68000 core. Provides the m68k
** memory hooks (the real Mac address map: ROM-overlay at reset, RAM/ROM
** windows, VIA/IWM/SCC decode) and drives the 60 Hz VBL interrupt.
**
** Address map (from the Mac 128K/512K technical details + MAME mac128.cpp):
**   overlay ON (reset): $000000 ROM (mirror), $400000 ROM, $600000 RAM
**   overlay OFF:        $000000 RAM,          $400000 ROM
**   SCC rd $800000-$9FFFFF, wr $A00000-$BFFFFF, IWM $C00000-$DFFFFF,
**   VIA $E80000-$EFFFFF (reg N at $EFE1FE + N*$200; IFR $EFFBFE, IER $EFFDFE)
** Overlay = VIA port A bit 4 (cleared low by the boot ROM). */
#ifndef MAC_H
#define MAC_H

#include <stdint.h>
#include "m68k.h"

#define MAC_RAM_SIZE 0x20000u   /* 128 KB */
#define MAC_ROM_SIZE 0x10000u   /* 64 KB  */

void     mac_init(const uint8_t *rom);   /* load 64 KB ROM, reset the CPU */
void     mac_step(void);                 /* one CPU instruction + machine timers */
int      mac_overlay(void);
uint32_t mac_vbl_count(void);
uint8_t *mac_ram_ptr(void);              /* for the framebuffer / debugging */

#endif
