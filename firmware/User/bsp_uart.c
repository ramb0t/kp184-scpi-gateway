/********************************** (C) COPYRIGHT *******************************
* File Name          : bsp_uart.c
* Author             : WCH
* Version            : V1.0
* Date               : 2022/05/10
* Description        : uart cfg.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "bsp_uart.h"
#include "tick.h"
#include "netsock.h"
#include "eth_driver.h"

/* Global Variable */

struct uart_data  uart_data_t;
/* UART2 */
u8  UART2_RX_DMA_DataBuf[UART_RX_DMA_SIZE]= {0};
u8  UART2_TX_DMA_DataBuf[UART_TX_BUF_NUM][ETH_RECEIVE_SIZE] = {0};
u16 UART2_TX_Buf_Length[UART_TX_BUF_NUM] = {0};

/**********************************************************************
 *  @fn      UART_GPIO_Init
 *
 *  @brief   initialize uart2 GPIO
 *
 *  @return  none
 * */
void UART_GPIO_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure={0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /* UART2 TX-->PA2   RX-->PA3 */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**********************************************************************
 *  @fn      UART_DMA_Init
 *
 *  @brief   initialize uart2 DMA
 *
 *  @return  none
 * */
void UART_DMA_Init(void)
{
    DMA_InitTypeDef DMA_InitStructure = {0};
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    /* UART2  TX-->DMA1_Channel7   RX-->DMA1_Channel6 */
    DMA_DeInit(DMA1_Channel7);
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)(&USART2->DATAR);
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)uart_data_t.TX_buffer;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    /* The TX DMA transfer length is deliberately not configured here: uartTx()
     * points MADDR at the buffer that is to be sent and writes CNTR right
     * before enabling the channel, so the length is set per transfer. */
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel7, &DMA_InitStructure);

    DMA_DeInit(DMA1_Channel6);
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)(&USART2->DATAR);
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)uart_data_t.RX_buffer;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = (uint32_t)UART_RX_DMA_SIZE;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_Init(DMA1_Channel6, &DMA_InitStructure);

    DMA_ClearFlag(DMA1_IT_TC7);

    DMA_Cmd(DMA1_Channel6, ENABLE);
    USART_DMACmd(USART2,USART_DMAReq_Rx|USART_DMAReq_Tx,ENABLE);
}

/**********************************************************************
 *  @fn      UART_Interrupt_Init
 *
 *  @brief   initialize uart2 interrupt
 *
 *  @return  none
 * */
void UART_Interrupt_Init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    /* UART2  TX-->DMA1_Channel7   RX-->DMA1_Channel6 */
    DMA_ITConfig(DMA1_Channel7,DMA_IT_TC,ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = DMA1_Channel7_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/**********************************************************************
 *  @fn      BSP_Uart_Init
 *
 *  @brief   initialize uart2
 *
 *  @param   baudrate - uart2 baud rate, BAUD_RATE is used by this example.
 *
 *  @return  none
 * */
void BSP_Uart_Init(uint32_t baudrate )
{
    USART_InitTypeDef USART_InitStructure = {0};

    UART_GPIO_Init();

    /* USART2 TX-->PA2   RX-->PA3 */
    uart_data_t.RX_buffer = UART2_RX_DMA_DataBuf;
    uart_data_t.TX_buffer = &UART2_TX_DMA_DataBuf[0];
    uart_data_t.TX_data_length = UART2_TX_Buf_Length;
    uart_data_t.rx_read = 0;
    uart_data_t.rx_write = 0;
    uart_data_t.last_RX_DMA_length = UART_RX_DMA_SIZE;
    uart_data_t.RX_data_length = 0;
    uart_data_t.tx_read = 0;
    uart_data_t.tx_write = 0;
    uart_data_t.tx_remainBuffNum = UART_TX_BUF_NUM;
    uart_data_t.uart_tx_dma_state = IDLE;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Init(USART2, &USART_InitStructure);
    USART_Cmd(USART2, ENABLE);

    UART_DMA_Init();
    UART_Interrupt_Init();
}

/**********************************************************************
 *  @fn      Uart_RxPeek
 *
 *  @brief   Look at the bytes received since the last consume. After the
 *           DMA has wrapped, the part up to the end of the buffer is
 *           returned first and the rest on the next call.
 *
 *  @param   p - set to the first unread byte
 *
 *  @return  number of contiguous unread bytes at *p
 * */
uint16_t Uart_RxPeek(uint8_t **p)
{
    uint16_t cntr = DMA1_Channel6->CNTR;            /* bytes left before the DMA wraps */
    uint16_t last = uart_data_t.last_RX_DMA_length;

    if(cntr == 0) cntr = UART_RX_DMA_SIZE;
    if(cntr == last) return 0;
    *p = &uart_data_t.RX_buffer[UART_RX_DMA_SIZE - last];
    return (cntr < last) ? (last - cntr) : last;
}

void Uart_RxConsume(uint16_t n)
{
    uart_data_t.last_RX_DMA_length -= n;
    if(uart_data_t.last_RX_DMA_length == 0)
        uart_data_t.last_RX_DMA_length = UART_RX_DMA_SIZE;
}

void Uart_RxFlush(void)
{
    uint16_t cntr = DMA1_Channel6->CNTR;

    uart_data_t.last_RX_DMA_length = cntr ? cntr : UART_RX_DMA_SIZE;
}

/**********************************************************************
 *  @fn      Uart_BusQuiet
 *
 *  @brief   Tell whether the line has been silent long enough to transmit.
 *           Measured on the KP184 through an HW-0519 module: a request that
 *           starts within about 1.5 ms of the end of a reply loses its first
 *           bytes on the bus, so the Modbus 3.5-character gap (0.6 ms at
 *           57600 baud) is not enough. Call it often; it watches the receive
 *           DMA counter and so only notices bytes when it is called.
 *
 *  @return  1 when nothing has been received for TX_GUARD
 * */
#define TX_GUARD    TICK_MS(5)

uint8_t Uart_BusQuiet(void)
{
    static uint16_t seen, since;
    static uint8_t quiet = 1;
    uint16_t cntr = DMA1_Channel6->CNTR;

    if(cntr != seen)
    {
        seen = cntr;
        since = Tick_Now();
        quiet = 0;
    }
    else if(!quiet && (Tick_Since(since) >= TX_GUARD))
    {
        quiet = 1;
    }
    return quiet;
}

/**********************************************************************
 *  @fn      Uart_TxKick
 *
 *  @brief   Send the next buffer queued by the raw TCP port, by DMA, once
 *           the bus is quiet.
 * */
void Uart_TxKick(void)
{
    if(!Uart_BusQuiet()) return;
    if(uart_data_t.uart_tx_dma_state == IDLE)
    {
        if(uart_data_t.tx_remainBuffNum < UART_TX_BUF_NUM)
        {
            DMA1_Channel7->MADDR = (uint32_t) &uart_data_t.TX_buffer[uart_data_t.tx_read];
            DMA1_Channel7->CNTR = uart_data_t.TX_data_length[uart_data_t.tx_read];
            uart_data_t.uart_tx_dma_state = BUSY;
            DMA_Cmd(DMA1_Channel7,ENABLE);
        }
    }
}

uint8_t Uart_TxIdle(void)
{
    return (uart_data_t.uart_tx_dma_state == IDLE) && (uart_data_t.tx_remainBuffNum == UART_TX_BUF_NUM) &&
           (USART_GetFlagStatus(USART2, USART_FLAG_TC) != RESET);
}

/* Abandon whatever the raw port still has queued. */
static void tx_abort(void)
{
    DMA_Cmd(DMA1_Channel7, DISABLE);
    __disable_irq();
    uart_data_t.tx_read = 0;
    uart_data_t.tx_write = 0;
    uart_data_t.tx_remainBuffNum = UART_TX_BUF_NUM;
    uart_data_t.uart_tx_dma_state = IDLE;
    __enable_irq();
    while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}

/**********************************************************************
 *  @fn      Uart_SendBlocking
 *
 *  @brief   Send a short frame by polling and return when its last stop
 *           bit has left, so the caller knows when the line is quiet.
 *           Anything still queued by the raw TCP port goes out first.
 * */
void Uart_SendBlocking(const uint8_t *buf, uint16_t len)
{
    uint16_t t0 = Tick_Now();

    /* Three full buffers take 1.5 s at 57600 baud. After 3 s, drop what is
     * left rather than write into USART2 alongside the DMA. */
    while((uart_data_t.uart_tx_dma_state != IDLE) || (uart_data_t.tx_remainBuffNum < UART_TX_BUF_NUM))
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
    while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}
