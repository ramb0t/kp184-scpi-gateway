/********************************** (C) COPYRIGHT *******************************
* File Name          : uart.h (from WCH's bsp_uart.h)
* Author             : WCH
* Version            : V1.0.0
* Date               : 2022/05/10
* Description        : uart cfg.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
/* USART2 (PA2 TX, PA3 RX): the RS485 side, 8N1.
 *
 * Receive runs by circular DMA and is read through one cursor. Transmit has
 * two ways out: a queue of blocks sent by DMA, for the raw TCP port, and a
 * blocking send for the short frames of the load's protocol. Neither starts
 * until the bus has been quiet for the guard time. */
#ifndef UART_H
#define UART_H

#include <stdint.h>
#include "net_config.h"

#define UART_BAUD           57600           /* the KP184's RS485 rate */
#define UART_TX_BLOCK_SIZE  RECE_BUF_LEN    /* holds everything one read of a socket can return */

void     Uart_Init(void);

uint16_t Uart_RxPeek(uint8_t **p);          /* contiguous unread bytes at *p, 0 if none */
void     Uart_RxConsume(uint16_t n);        /* mark n of the peeked bytes as read */
void     Uart_RxFlush(void);                /* forget everything received so far */

uint8_t  Uart_BusQuiet(void);               /* 1 once nothing has been received for the guard time */

uint8_t *Uart_TxBlock(void);                /* a free block to fill, or NULL while the queue is full */
void     Uart_TxQueue(uint16_t len);        /* queue the first len bytes of that block */
void     Uart_TxKick(void);                 /* start sending the next queued block, if the bus allows */
uint8_t  Uart_TxIdle(void);                 /* nothing queued and the last stop bit has left */
void     Uart_SendBlocking(const uint8_t *buf, uint16_t len);  /* returns once the last bit has left */

#endif
