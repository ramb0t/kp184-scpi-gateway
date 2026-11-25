#include "tick.h"
#include "eth_driver.h"

static volatile uint32_t uptime_s;

void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* Both timers are clocked at SystemCoreClock, as WCH's examples assume. */
static void timer_start(TIM_TypeDef *timer, uint16_t prescaler, uint16_t period)
{
    TIM_TimeBaseInitTypeDef base = {0};

    base.TIM_Prescaler = prescaler;
    base.TIM_Period = period;
    base.TIM_ClockDivision = 0;
    base.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(timer, &base);
    TIM_Cmd(timer, ENABLE);
}

void Tick_Init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 | RCC_APB1Periph_TIM3, ENABLE);

    timer_start(TIM3, SystemCoreClock / TICK_HZ - 1, 0xFFFF);

    timer_start(TIM2, WCHNETTIMERPERIOD * 1000 - 1, SystemCoreClock / 1000000 - 1);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);
    NVIC_EnableIRQ(TIM2_IRQn);
}

void TIM2_IRQHandler(void)
{
    static uint16_t ms;

    WCHNET_TimeIsr(WCHNETTIMERPERIOD);
    ms += WCHNETTIMERPERIOD;
    if(ms >= 1000)
    {
        ms -= 1000;
        uptime_s++;
    }
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}

uint32_t Tick_UptimeSeconds(void)
{
    return uptime_s;
}
