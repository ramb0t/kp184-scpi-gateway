/********************************** (C) COPYRIGHT *******************************
* File Name          : uart.c (from WCH's bsp_uart.c)
* Author             : WCH
* Version            : V1.0
* Date               : 2022/05/10
* Description        : uart cfg.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v20x.h"
#include "uart.h"
#include "tick.h"
#include "netsock.h"

#define RX_DMA          DMA1_Channel6
#define TX_DMA          DMA1_Channel7

#define RX_SIZE         RECE_BUF_LEN
#define TX_BLOCKS       3

/* Measured on the KP184 through an HW-0519 module: a request that starts
 * within about 1.5 ms of the end of a reply loses its first bytes on the bus,
 * so the Modbus 3.5-character gap (0.6 ms at 57600 baud) is not enough. */
#define TX_GUARD        TICK_MS(5)

static uint8_t  rx_buffer[RX_SIZE];
static uint16_t rx_cursor = RX_SIZE;        /* the receive DMA's countdown at the first unread byte */

static uint8_t  tx_block[TX_BLOCKS][UART_TX_BLOCK_SIZE];
static uint16_t tx_length[TX_BLOCKS];
static uint8_t  tx_fill;                    /* the block the next Uart_TxQueue() takes */
static volatile uint8_t tx_send;            /* the block the DMA sends next */
static volatile uint8_t tx_free = TX_BLOCKS;
static volatile uint8_t tx_running;         /* a DMA transfer is under way */

void DMA1_Channel7_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* --- setup --------------------------------------------------------------------- */

static void pins_init(void)
{
    GPIO_InitTypeDef pin = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    pin.GPIO_Pin = GPIO_Pin_2;
    pin.GPIO_Speed = GPIO_Speed_50MHz;
    pin.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &pin);
    pin.GPIO_Pin = GPIO_Pin_3;
    pin.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &pin);
}

static void usart_init(void)
{
    USART_InitTypeDef usart = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    usart.USART_BaudRate = UART_BAUD;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART2, &usart);
    USART_Cmd(USART2, ENABLE);
}

static void dma_init(void)
{
    DMA_InitTypeDef dma = {0};
    NVIC_InitTypeDef irq = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    /* Transmit: one block per transfer. Uart_TxKick() sets the address and
     * length of each, so no length is configured here. */
    DMA_DeInit(TX_DMA);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)tx_block;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(TX_DMA, &dma);

    /* Receive: around the buffer for ever. */
    DMA_DeInit(RX_DMA);
    dma.DMA_MemoryBaseAddr = (uint32_t)rx_buffer;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = RX_SIZE;
    dma.DMA_Mode = DMA_Mode_Circular;
    DMA_Init(RX_DMA, &dma);

    DMA_ClearFlag(DMA1_IT_TC7);
    DMA_Cmd(RX_DMA, ENABLE);
    USART_DMACmd(USART2, USART_DMAReq_Rx | USART_DMAReq_Tx, ENABLE);

    DMA_ITConfig(TX_DMA, DMA_IT_TC, ENABLE);
    irq.NVIC_IRQChannel = DMA1_Channel7_IRQn;
    irq.NVIC_IRQChannelPreemptionPriority = 1;
    irq.NVIC_IRQChannelSubPriority = 2;
    irq.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&irq);
}

void Uart_Init(void)
{
    pins_init();
    usart_init();
    dma_init();
}

/* --- receive ------------------------------------------------------------------- */

/* The receive DMA counts down from RX_SIZE as bytes arrive and starts again at zero. */
static uint16_t rx_countdown(void)
{
    uint16_t left = RX_DMA->CNTR;

    return left ? left : RX_SIZE;
}

/* After the DMA has wrapped, the bytes up to the end of the buffer are
 * returned first and the rest on the next call. */
uint16_t Uart_RxPeek(uint8_t **p)
{
    uint16_t left = rx_countdown();

    if(left == rx_cursor) return 0;
    *p = &rx_buffer[RX_SIZE - rx_cursor];
    return (left < rx_cursor) ? (rx_cursor - left) : rx_cursor;
}

void Uart_RxConsume(uint16_t n)
{
    rx_cursor -= n;
    if(rx_cursor == 0) rx_cursor = RX_SIZE;
}

void Uart_RxFlush(void)
{
    rx_cursor = rx_countdown();
}

/* Call it often: it watches the receive DMA's counter, so it notices bytes
 * only when it is called. */
uint8_t Uart_BusQuiet(void)
{
    static uint16_t seen, since;
    static uint8_t quiet = 1;
    uint16_t countdown = RX_DMA->CNTR;

    if(countdown != seen)
    {
        seen = countdown;
        since = Tick_Now();
        quiet = 0;
    }
    else if(!quiet && (Tick_Since(since) >= TX_GUARD))
    {
        quiet = 1;
    }
    return quiet;
}

/* --- transmit: the queue ------------------------------------------------------- */

uint8_t *Uart_TxBlock(void)
{
    return tx_free ? tx_block[tx_fill] : NULL;
}

void Uart_TxQueue(uint16_t len)
{
    __disable_irq();
    tx_length[tx_fill] = len;
    tx_fill = (tx_fill + 1) % TX_BLOCKS;
    tx_free--;
    __enable_irq();
}

void Uart_TxKick(void)
{
    if(!Uart_BusQuiet() || tx_running || (tx_free == TX_BLOCKS)) return;
    TX_DMA->MADDR = (uint32_t)tx_block[tx_send];
    TX_DMA->CNTR = tx_length[tx_send];
    tx_running = 1;
    DMA_Cmd(TX_DMA, ENABLE);
}

/* A block has gone out. */
void DMA1_Channel7_IRQHandler(void)
{
    if(DMA_GetITStatus(DMA1_IT_TC7))
    {
        DMA_Cmd(TX_DMA, DISABLE);
        tx_running = 0;
        tx_send = (tx_send + 1) % TX_BLOCKS;
        tx_free++;
        DMA_ClearITPendingBit(DMA1_IT_TC7);
    }
}

static uint8_t tx_queue_empty(void)
{
    return !tx_running && (tx_free == TX_BLOCKS);
}

static uint8_t last_bit_left(void)
{
    return USART_GetFlagStatus(USART2, USART_FLAG_TC) != RESET;
}

uint8_t Uart_TxIdle(void)
{
    return tx_queue_empty() && last_bit_left();
}

/* Abandon whatever is still queued. */
static void tx_abort(void)
{
    DMA_Cmd(TX_DMA, DISABLE);
    __disable_irq();
    tx_send = 0;
    tx_fill = 0;
    tx_free = TX_BLOCKS;
    tx_running = 0;
    __enable_irq();
    while(!last_bit_left());
}

/* --- transmit: blocking -------------------------------------------------------- */

/* Sends a short frame by polling, so the caller knows when the line is quiet
 * again. What the raw port still has queued goes out first. */
void Uart_SendBlocking(const uint8_t *buf, uint16_t len)
{
    uint16_t t0 = Tick_Now();

    /* Three full blocks take 1.5 s at 57600 baud. After 3 s, drop what is
     * left rather than write into USART2 alongside the DMA. */
    while(!tx_queue_empty())
    {
        Uart_TxKick();
        Net_Service();
        if(Tick_Since(t0) > TICK_MS(3000))
        {
            tx_abort();
            break;
        }
    }
    /* A bus that never falls silent must not hang the gateway: send anyway after 100 ms. */
    t0 = Tick_Now();
    while(!Uart_BusQuiet())
    {
        Net_Service();
        if(Tick_Since(t0) > TICK_MS(100)) break;
    }
    while(len--)
    {
        while(USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
        USART_SendData(USART2, *buf++);
    }
    while(!last_bit_left());
}
