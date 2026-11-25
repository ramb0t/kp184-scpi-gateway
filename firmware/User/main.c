/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2022/05/10
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
/*
 *@Note
eth485: WCH's ETH_UART example changed to take its address by DHCP and to listen as a TCP server.
One client at a time connects to LISTEN_PORT; bytes pass transparently between that socket and USART2
(PA2 TX, PA3 RX) at BAUD_RATE (bsp_uart.h).
For details on the selection of engineering chips,
please refer to the "CH32V20x Evaluation Board Manual" under the CH32V20xEVT\EVT\PUB folder.
*/
#include "string.h"
#include "eth_driver.h"
#include "bsp_uart.h"

#define LISTEN_PORT    4001                                 //TCP port the bridge listens on
#define DHCP_HOSTNAME  "eth485"                             //name offered to the DHCP server

u8 MACAddr[6];                                              //MAC address
u8 IPAddr[4]   = {0, 0, 0, 0};                              //IP address, filled in by DHCP
u8 GWIPAddr[4] = {0, 0, 0, 0};                              //Gateway IP address, filled in by DHCP
u8 IPMask[4]   = {0, 0, 0, 0};                              //subnet mask, filled in by DHCP

u8 SocketIdForListen;                                       //listening socket id
u8 ListenValid = 0;                                         //1 once the listening socket exists
u8 SocketId;                                                //socket id of the connected client
u8 SocketRecvBuf[RECE_BUF_LEN];                             //socket data buff

u8 TCPConnValid = 0;                                        //0 disconnected  1 connect

u32 ethRxDropCnt = 0;                                   //frames dropped because no TX buffer was free
/*********************************************************************
 * @fn      mStopIfError
 *
 * @brief   check if error.
 *
 * @param   iError - error constants.
 *
 * @return  none
 */
void mStopIfError(u8 iError)
{
    if (iError == WCHNET_ERR_SUCCESS) return;
    printf("Error: %02X\r\n", (u16)iError);
}

/*********************************************************************
 * @fn      TIM2_Init
 *
 * @brief   Initializes TIM2.
 *
 * @return  none
 */
void TIM2_Init( void )
{
    TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure={0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    TIM_TimeBaseStructure.TIM_Period = SystemCoreClock / 1000000;
    TIM_TimeBaseStructure.TIM_Prescaler = WCHNETTIMERPERIOD * 1000 - 1;
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);
    TIM_ITConfig(TIM2, TIM_IT_Update ,ENABLE);

    TIM_Cmd(TIM2, ENABLE);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update );
    NVIC_EnableIRQ(TIM2_IRQn);
}

/*********************************************************************
 * @fn      WCHNET_CreateTcpSocketListen
 *
 * @brief   Create the TCP socket that listens on LISTEN_PORT
 *
 * @return  none
 */
void WCHNET_CreateTcpSocketListen(void)
{
    u8 i;
    SOCK_INF TmpSocketInf;

    memset((void *)&TmpSocketInf,0,sizeof(SOCK_INF));
    TmpSocketInf.SourPort = LISTEN_PORT;
    TmpSocketInf.ProtoType = PROTO_TYPE_TCP;
    i = WCHNET_SocketCreat(&SocketIdForListen,&TmpSocketInf);
    mStopIfError(i);
    if(i != WCHNET_ERR_SUCCESS){
        return;
    }
    printf("SocketIdForListen %d\r\n",SocketIdForListen);
    i = WCHNET_SocketListen(SocketIdForListen);
    mStopIfError(i);
    if(i == WCHNET_ERR_SUCCESS){
        ListenValid = 1;
    }
}


/*********************************************************************
 * @fn      uartRxAndSendDataToETH
 *
 * @brief   uart receive data, and send these data to eth. The send length
 *          is in/out: only the bytes actually handed to the stack are
 *          consumed, so a send that could not be queued is retried by the
 *          next call instead of being dropped.
 *
 * @return  none
 */
void uartRxAndSendDataToETH(void)
{
    u8 *p;
    u8 ret;
    uint32_t temp = 0;
    uint32_t len = 0;
    uint32_t readindex = 0;

    temp = DMA1_Channel6->CNTR;

    /* uart rx dma CNTR not equal to last, indicating that data is received*/
    if(temp != uart_data_t.last_RX_DMA_length)
    {
        if(temp < uart_data_t.last_RX_DMA_length)
        {
            len = uart_data_t.last_RX_DMA_length - temp;
        }
        else {
            len = uart_data_t.last_RX_DMA_length;
        }
        readindex = UART_RX_DMA_SIZE - uart_data_t.last_RX_DMA_length;
        p = &uart_data_t.RX_buffer[readindex];
        ret = WCHNET_SocketSend(SocketId, p, &len);
        if((ret != WCHNET_ERR_SUCCESS) || (len == 0))
        {
            return;
        }
        uart_data_t.last_RX_DMA_length -= len;
        if(uart_data_t.last_RX_DMA_length == 0)
            uart_data_t.last_RX_DMA_length = UART_RX_DMA_SIZE;
    }
}

/*********************************************************************
 * @fn      uartTx
 *
 * @brief   send data from TX_buffer by uart tx dma
 *
 * @return  none
 */
void uartTx(void)
{
    if(uart_data_t.uart_tx_dma_state == IDLE)
    {
        /* eth has received data  */
        if(uart_data_t.tx_remainBuffNum < UART_TX_BUF_NUM)
        {
            DMA1_Channel7->MADDR = (uint32_t) &uart_data_t.TX_buffer[uart_data_t.tx_read];
            DMA1_Channel7->CNTR = uart_data_t.TX_data_length[uart_data_t.tx_read];
            uart_data_t.uart_tx_dma_state = BUSY;
            DMA_Cmd(DMA1_Channel7,ENABLE);
        }
    }
}

/*********************************************************************
 * @fn      ETHRx
 *
 * @brief   ETH receive data and save it to buff.
 *
 * @param   socketid - socket id.
 *
 * @return  none
 */
void ETHRx(u8 socketid)
{
    u32 len;
    int8_t receive_state = -1;

    len = WCHNET_SocketRecvLen(socketid, NULL);                               /* query length */
    if(len > ETH_RECEIVE_SIZE)
        len = ETH_RECEIVE_SIZE;

    if( uart_data_t.tx_remainBuffNum > 0)
    {
        /* socket receive */
        receive_state = WCHNET_SocketRecv(socketid, uart_data_t.TX_buffer[uart_data_t.tx_write], &len);
        if(receive_state == WCHNET_ERR_SUCCESS)
        {
            __disable_irq();
            uart_data_t.TX_data_length[uart_data_t.tx_write] = len;

            uart_data_t.tx_write++;
            /* Prevent access from out-of-bounds */
            uart_data_t.tx_write = (uart_data_t.tx_write)%UART_TX_BUF_NUM;

            uart_data_t.tx_remainBuffNum--;
            __enable_irq();
        }
    }
    else
    {
        WCHNET_SocketRecv(socketid, NULL, &len);
        ethRxDropCnt++;
        printf("eth receive buff busy, drop count: %u\r\n", (unsigned)ethRxDropCnt);
    }
    uartTx();
}

/*********************************************************************
 * @fn      WCHNET_HandleSockInt
 *
 * @brief   Socket Interrupt Handle
 *
 * @param   socketid - socket id.
 *          intstat - interrupt status
 *
 * @return  none
 */
void WCHNET_HandleSockInt(u8 socketid,u8 intstat)
{
    if(intstat & SINT_STAT_RECV)                        //receive data
    {
        ETHRx(socketid);
    }
    if(intstat & SINT_STAT_CONNECT)                     //a client connected; socketid is the accepted socket
    {
        SocketId = socketid;
        WCHNET_SocketSetKeepLive(socketid, ENABLE);
        WCHNET_ModifyRecvBuf(socketid, (u32)SocketRecvBuf, RECE_BUF_LEN);
        /* Forget UART bytes that arrived while nobody was connected. */
        uart_data_t.last_RX_DMA_length = DMA1_Channel6->CNTR;
        TCPConnValid = 1;
        printf("TCP Connect Success\r\n");
    }
    if(intstat & SINT_STAT_DISCONNECT)                  //disconnect; the listening socket stays open
    {
        if(socketid == SocketId) TCPConnValid = 0;
        printf("TCP Disconnect\r\n");
    }
    if(intstat & SINT_STAT_TIM_OUT)                     //timeout disconnect
    {
        if(socketid == SocketId) TCPConnValid = 0;
        printf("TCP Timeout\r\n");
    }
}

/*********************************************************************
 * @fn      WCHNET_HandleGlobalInt
 *
 * @brief   Global Interrupt Handle
 *
 * @return  none
 */
void WCHNET_HandleGlobalInt(void)
{
    u8 intstat;
    u8 socketint;
    u16 i;

    intstat = WCHNET_GetGlobalInt();                                           //get global interrupt flag
    if(intstat & GINT_STAT_UNREACH)                                            //Unreachable interrupt
    {
        printf("GINT_STAT_UNREACH\r\n");
    }
    if(intstat & GINT_STAT_IP_CONFLI)                                          //IP conflict
    {
        printf("GINT_STAT_IP_CONFLI\r\n");
    }
    if(intstat & GINT_STAT_PHY_CHANGE)                                         //PHY status change
    {
        i = WCHNET_GetPHYStatus();                                             //socket related interrupt
        if(i&PHY_Linked_Status)
            printf("PHY Link Success\r\n");
    }
    if(intstat & GINT_STAT_SOCKET)
    {
        for(i = 0; i < WCHNET_MAX_SOCKET_NUM; i ++)
        {
            socketint = WCHNET_GetSocketInt(i);
            if(socketint)WCHNET_HandleSockInt(i,socketint);
        }
    }
}

/*********************************************************************
 * @fn      WCHNET_DHCPCallBack
 *
 * @brief   DHCPCallBack. Called on the first lease and again on renewals.
 *
 * @param   status - status returned by DHCP
 *                   0x00 - Success
 *                   0x01 - Failure
 *          arg - Data returned by DHCP
 *
 * @return  DHCP status
 */
u8 WCHNET_DHCPCallBack(u8 status, void *arg)
{
    u8 *p = arg;

    if(status || (p == NULL))
    {
        printf("DHCP Fail %02x \r\n", status);
        return NoREADY;
    }
    printf("DHCP Success\r\n");
    if(memcmp(IPAddr, p, sizeof(IPAddr)))
    {
        /* New or changed address: a client connected to the old one is gone. */
        if(TCPConnValid)
        {
            TCPConnValid = 0;
            WCHNET_SocketClose(SocketId, TCP_CLOSE_NORMAL);
        }
        memcpy(IPAddr, p, 4);
        memcpy(GWIPAddr, &p[4], 4);
        memcpy(IPMask, &p[8], 4);
        printf("IPAddr: %d.%d.%d.%d \r\n", (u16)IPAddr[0], (u16)IPAddr[1],
               (u16)IPAddr[2], (u16)IPAddr[3]);
    }
    if(!ListenValid)
    {
        WCHNET_CreateTcpSocketListen();
    }
    return READY;
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program
 *
 * @return  none
 */
int main(void)
{
    u8 i;
    
    SystemCoreClockUpdate();
	Delay_Init();
	USART_Printf_Init(115200);                                              //USART initialize
	printf("ETH_UART\r\n");	
    if((SystemCoreClock == 60000000) || (SystemCoreClock == 120000000))
        printf("SystemClk:%lu\r\n", (unsigned long)SystemCoreClock);
    else
        printf("Error: Please choose 60MHz and 120MHz clock when using Ethernet!\r\n");
    printf("net version:%x\r\n",(unsigned)WCHNET_GetVer());
    if(WCHNET_LIB_VER != WCHNET_GetVer()){
        printf("version error.\r\n");
    }
    WCHNET_GetMacAddr(MACAddr);                                             //get the chip MAC address
    printf("mac addr:");
    for(i = 0; i < 6; i++) 
        printf("%x ", (unsigned)MACAddr[i]);
    printf("\r\n");
    TIM2_Init();
    BSP_Uart_Init(BAUD_RATE);                                               //USART2 initialize
    WCHNET_DHCPSetHostname(DHCP_HOSTNAME);                                  //Configure DHCP host name
    i = ETH_LibInit(IPAddr,GWIPAddr,IPMask,MACAddr);                        //Ethernet library initialize
    mStopIfError(i);
    if(i == WCHNET_ERR_SUCCESS) {
        printf("WCHNET_LibInit Success\r\n");
    }
    else {
        printf("WCHNET_LibInit failed, halted\r\n");
        while(1);
    }

    /* There is one TCP slot, so a client that vanished must be noticed quickly
     * or it blocks the next one: probe after 10 s idle, give up after 3 x 5 s. */
    {
        struct _KEEP_CFG cfg;

        cfg.KLIdle = 10000;
        cfg.KLIntvl = 5000;
        cfg.KLCount = 3;
        WCHNET_ConfigKeepLive(&cfg);
    }
    WCHNET_DHCPStart(WCHNET_DHCPCallBack);                                  //Start DHCP; the callback opens the listening socket

	while(1)
	{
        /*Ethernet library main task function,
         * which needs to be called cyclically*/
	    WCHNET_MainTask();
        /*Query the Ethernet global interrupt,
         * if there is an interrupt, call the global interrupt handler*/
        if(WCHNET_QueryGlobalInt())
        {
            WCHNET_HandleGlobalInt();
        }
        if(TCPConnValid){
            uartTx();
            uartRxAndSendDataToETH();
        }
    }
}
