#include "tick.h"

void Tick_Init(void)
{
    TIM_TimeBaseInitTypeDef tb = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    tb.TIM_Period = 0xFFFF;
    tb.TIM_Prescaler = SystemCoreClock / TICK_HZ - 1;   /* same timer clock assumption as WCH's TIM2 setup */
    tb.TIM_ClockDivision = 0;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tb);
    TIM_Cmd(TIM3, ENABLE);
}
