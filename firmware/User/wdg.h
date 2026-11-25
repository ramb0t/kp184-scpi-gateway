/* Independent watchdog and boot diagnostics.
 *
 * The watchdog resets the board if it is not fed for about 4 s (LSI 40 kHz
 * nominal; the LSI's spread makes it roughly 2.7 to 5.3 s). It is fed by the
 * main loop and by Net_Service() in every bounded wait, so only a real hang
 * trips it. It keeps running while a debugger halts the core, so a halt of
 * more than a few seconds resets the board; make flash resets and halts the
 * chip before programming, so the watchdog is not running then. */
#ifndef WDG_H
#define WDG_H

#include <stdint.h>

void Wdg_Init(void);
void Wdg_Feed(void);
const char *Wdg_ResetCause(void);       /* why the board last started: POWER, PIN, WATCHDOG, SOFTWARE */

extern volatile uint32_t Uptime_ms;     /* advanced by the TIM2 interrupt */

#endif
