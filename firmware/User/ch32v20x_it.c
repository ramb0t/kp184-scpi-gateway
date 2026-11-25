/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/06/06
 * Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
/* The interrupts that belong to no module of ours. TIM2's handler is in
 * tick.c and the uart's transmit DMA handler in uart.c. */
#include "ch32v20x_it.h"
#include "eth_driver.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void ETH_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void NMI_Handler(void)
{
}

/* Restart; the load's input goes off as the gateway starts again. */
void HardFault_Handler(void)
{
    NVIC_SystemReset();
    while(1);
}

void ETH_IRQHandler(void)
{
    WCHNET_ETHIsr();
}
