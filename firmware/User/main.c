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
eth485: network gateway for a Kunkin KP184 electronic load on RS485, grown from WCH's ETH_UART example.
The board takes its address by DHCP and listens on two TCP ports, one client each:
  SCPI_PORT  a SCPI command interpreter that talks to the load itself (scpi_server.c, kp184.c)
  RAW_PORT   bytes passed straight between the socket and USART2 (PA2 TX, PA3 RX) at BAUD_RATE,
             for software that speaks the load's own protocol
While a raw client is connected it owns the RS485 port and SCPI commands that need the load fail.
*/
#include "string.h"
#include "eth_driver.h"
#include "bsp_uart.h"
#include "kp184.h"
#include "scpi_server.h"
#include "netsock.h"
#include "tick.h"

#define SCPI_PORT      5025                                 //IANA "scpi-raw"
#define RAW_PORT       4001                                 //transparent TCP-to-serial port
#define DHCP_HOSTNAME  "eth485"                             //name offered to the DHCP server

u8 MACAddr[6];                                              //MAC address
u8 IPAddr[4]   = {0, 0, 0, 0};                              //IP address, filled in by DHCP
u8 GWIPAddr[4] = {0, 0, 0, 0};                              //Gateway IP address, filled in by DHCP
u8 IPMask[4]   = {0, 0, 0, 0};                              //subnet mask, filled in by DHCP

u8 ListenValid = 0;                                         //1 once the listening sockets exist
net_session_t Raw;                                          //the raw client, if Raw.active
u8 RawRecvBuf[RECE_BUF_LEN];                                //receive buffer of the raw connection
u8 ScpiRecvBuf[SCPI_RECV_BUF_LEN];                          //receive buffer of the SCPI connection
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
 * @brief   Create a TCP socket that listens on the given port
 *
 * @return  1 on success
 */
u8 WCHNET_CreateTcpSocketListen(u16 port)
{
    u8 i, id;
    SOCK_INF TmpSocketInf;

    memset((void *)&TmpSocketInf,0,sizeof(SOCK_INF));
    TmpSocketInf.SourPort = port;
    TmpSocketInf.ProtoType = PROTO_TYPE_TCP;
    i = WCHNET_SocketCreat(&id,&TmpSocketInf);
    mStopIfError(i);
    if(i != WCHNET_ERR_SUCCESS){
        return 0;
    }
    printf("listen %d on socket %d\r\n",port,id);
    i = WCHNET_SocketListen(id);
    mStopIfError(i);
    return (i == WCHNET_ERR_SUCCESS);
}

/*********************************************************************
 * @fn      RawEnd
 *
 * @brief   The raw client is gone: give the RS485 port back to SCPI once
 *          the load has finished answering it.
 *
 * @return  none
 */
void RawEnd(void)
{
    Raw.active = 0;
    KP184_BusRelease();
    printf("raw client gone\r\n");
}

/*********************************************************************
 * @fn      RawPoll
 *
 * @brief   Raw port, called from the main loop. Bytes from the client go to
 *          a uart TX buffer only when one is free, so a fast client is held
 *          back by TCP flow control instead of losing data. Bytes from the
 *          uart go to the client; only the bytes the stack accepted are
 *          consumed, so a send that could not be queued is retried.
 *
 * @return  none
 */
void RawPoll(void)
{
    u8 *p;
    uint32_t len;

    if(!Raw.active) return;
    if(!Net_SessionOwnsSlot(&Raw))                  //the slot went to another client
    {
        RawEnd();
        return;
    }
    if(uart_data_t.tx_remainBuffNum > 0)
    {
        len = Net_SessionRead(&Raw, uart_data_t.TX_buffer[uart_data_t.tx_write], ETH_RECEIVE_SIZE);
        if(len)
        {
            __disable_irq();
            uart_data_t.TX_data_length[uart_data_t.tx_write] = len;
            uart_data_t.tx_write = (uart_data_t.tx_write + 1) % UART_TX_BUF_NUM;
            uart_data_t.tx_remainBuffNum--;
            __enable_irq();
        }
    }
    Uart_TxKick();
    if(!Net_SessionOpen(&Raw))
    {
        if(Net_SessionPending(&Raw) == 0) RawEnd();  //closed, and everything it sent is queued
        return;
    }
    len = Uart_RxPeek(&p);
    if(len && (WCHNET_SocketSend(Raw.id, p, &len) == WCHNET_ERR_SUCCESS) && len)
    {
        Uart_RxConsume(len);
    }
}

/*********************************************************************
 * @fn      CloseClients
 *
 * @brief   Drop both client connections; the listening sockets stay.
 *
 * @return  none
 */
void CloseClients(void)
{
    if(Raw.active)
    {
        if(Net_SessionOpen(&Raw)) WCHNET_SocketClose(Raw.id, TCP_CLOSE_NORMAL);
        RawEnd();
    }
    Scpi_Close();
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
    u32 len;

    /* Connect is handled before receive: both can be flagged at once. */
    if(intstat & SINT_STAT_CONNECT)                     //a client connected; socketid is the accepted socket
    {
        u16 port = SocketInf[socketid].SourPort;        //the local port tells which listener accepted it

        /* A session whose slot was just handed to this client is over, and
         * the stack wiped its disconnect flag when it reused the slot. */
        if(Raw.active && !Net_SessionOwnsSlot(&Raw)) RawEnd();
        Scpi_Check();

        if((port == RAW_PORT) && !Raw.active)
        {
            Net_SessionStart(&Raw, socketid);
            WCHNET_SocketSetKeepLive(socketid, ENABLE);
            WCHNET_ModifyRecvBuf(socketid, (u32)RawRecvBuf, RECE_BUF_LEN);
            Uart_RxFlush();                             //forget bytes that arrived while nobody was connected
            KP184_BusLocked = 1;
            printf("raw client connected\r\n");
        }
        else if((port == SCPI_PORT) && !Scpi_IsConnected())
        {
            WCHNET_SocketSetKeepLive(socketid, ENABLE);
            WCHNET_ModifyRecvBuf(socketid, (u32)ScpiRecvBuf, SCPI_RECV_BUF_LEN);
            Scpi_Connect(socketid);
            printf("SCPI client connected\r\n");
        }
        else
        {
            /* A second client on a port that already has one. */
            WCHNET_SocketClose(socketid, TCP_CLOSE_RST);
            return;
        }
    }
    if(intstat & SINT_STAT_RECV)                        //receive data
    {
        /* The two clients' data is read by RawPoll() and Scpi_Poll(). Anything
         * else is dropped, but only from a socket that is still open. */
        if(!(Raw.active && (socketid == Raw.id)) && !(Scpi_IsConnected() && (socketid == Scpi_Socket())) &&
           (SocketInf[socketid].SockStatus == SOCK_STAT_OPEN))
        {
            len = WCHNET_SocketRecvLen(socketid, NULL);
            if(len) WCHNET_SocketRecv(socketid, NULL, &len);
        }
    }
    if(intstat & (SINT_STAT_DISCONNECT | SINT_STAT_TIM_OUT))    //closed or timed out; the listening sockets stay open
    {
        /* Checked separately: after a slot was reused, both could name it. */
        if(Raw.active && (socketid == Raw.id))
        {
            RawPoll();                                  //queue what the client sent before it closed
            if(Raw.active) RawEnd();
        }
        if(Scpi_IsConnected() && (socketid == Scpi_Socket()))
        {
            Scpi_Disconnect();
            printf("SCPI client gone\r\n");
        }
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
        /* New or changed address: clients connected to the old one are gone. */
        CloseClients();
        memcpy(IPAddr, p, 4);
        memcpy(GWIPAddr, &p[4], 4);
        memcpy(IPMask, &p[8], 4);
        printf("IPAddr: %d.%d.%d.%d \r\n", (u16)IPAddr[0], (u16)IPAddr[1],
               (u16)IPAddr[2], (u16)IPAddr[3]);
    }
    if(!ListenValid)
    {
        ListenValid = WCHNET_CreateTcpSocketListen(SCPI_PORT) &
                      WCHNET_CreateTcpSocketListen(RAW_PORT);
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
    printf("eth485\r\n");
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
    Tick_Init();
    TIM2_Init();
    BSP_Uart_Init(BAUD_RATE);                                               //USART2 initialize
    Scpi_Init();
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

    /* Each port takes one client, so one that vanished must be noticed quickly
     * or it blocks the next: probe after 10 s idle, give up after 3 x 5 s. */
    {
        struct _KEEP_CFG cfg;

        cfg.KLIdle = 10000;
        cfg.KLIntvl = 5000;
        cfg.KLCount = 3;
        WCHNET_ConfigKeepLive(&cfg);
    }
    WCHNET_DHCPStart(WCHNET_DHCPCallBack);                                  //Start DHCP; the callback opens the listening sockets

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
        RawPoll();
        Uart_TxKick();
        Scpi_Poll();
    }
}
