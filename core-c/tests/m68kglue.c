/* Isolated access to Musashi internals. m68kcpu.h drags in a pile of internal
** macros, so it gets its own translation unit with no other identifiers to
** collide with. */
#include "m68kcpu.h"

/* Clear the stopped/halted state a STOP instruction or a double bus fault
** leaves behind, so the next test steps normally without a full pulse_reset
** (which would burn an execute on the reset sequence). */
void tm_clear_stopped(void){ m68ki_cpu.stopped = 0; }
