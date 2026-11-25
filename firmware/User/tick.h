/* Free-running 0.1 ms time base on TIM3, read without interrupts.
 * The 16-bit counter wraps every 6.5 s, so only intervals shorter than that
 * can be measured, and only by subtraction through Tick_Since(). */
#ifndef TICK_H
#define TICK_H

#include "ch32v20x.h"

#define TICK_HZ         10000
#define TICK_MS(ms)     ((uint16_t)((ms) * (TICK_HZ / 1000)))

void Tick_Init(void);

static inline uint16_t Tick_Now(void)
{
    return (uint16_t)TIM3->CNT;
}

static inline uint16_t Tick_Since(uint16_t t0)
{
    return (uint16_t)(Tick_Now() - t0);
}

#endif
