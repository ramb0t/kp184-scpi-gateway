#include "ch32v20x.h"
#include "wdg.h"

volatile uint32_t Uptime_ms;
static const char *cause = "UNKNOWN";

void Wdg_Init(void)
{
    /* Record why we started before clearing the flags. A power-on also sets
     * PINRST, so POR is checked first. */
    if(RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) cause = "WATCHDOG";
    else if(RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET) cause = "SOFTWARE";
    else if(RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET) cause = "POWER";
    else if(RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET) cause = "PIN";
    RCC_ClearFlag();

    /* Not frozen under the debugger: writing DBGMCU_IWDG_STOP to CSR 0x7C0
     * from running code made the chip reset in a loop. make flash resets and
     * halts the chip first instead, before this has run. */
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);           /* 40 kHz / 64 = 625 Hz */
    IWDG_SetReload(2500);                           /* 2500 / 625 Hz = 4 s */
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void Wdg_Feed(void)
{
    IWDG_ReloadCounter();
}

const char *Wdg_ResetCause(void)
{
    return cause;
}
