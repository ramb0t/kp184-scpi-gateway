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
/* eth485: network gateway for a Kunkin KP184 electronic load on RS485, grown
 * from WCH's ETH_UART example.
 *
 * The board takes its address by DHCP and listens on two TCP ports, one
 * client each:
 *   SCPI_PORT  a SCPI command interpreter that talks to the load itself
 *              (scpi_server.c, kp184.c)
 *   RAW_PORT   bytes passed straight between the socket and the RS485 line,
 *              for software that speaks the load's own protocol (raw_port.c)
 *
 * This file starts everything, hands each new connection to its port and
 * runs the main loop. */
#include <string.h>
#include "eth_driver.h"
#include "failsafe.h"
#include "netsock.h"
#include "raw_port.h"
#include "scpi_server.h"
#include "tick.h"
#include "uart.h"
#include "wdg.h"

#define SCPI_PORT       5025                /* IANA "scpi-raw" */
#define RAW_PORT        4001
#define DHCP_HOSTNAME   "eth485"

static uint8_t mac[6];
static uint8_t ip[4], gateway[4], netmask[4];   /* filled in by DHCP */
static uint8_t listening;                       /* both listening sockets exist */
static uint8_t readdressed;                     /* DHCP changed the address: clients of the old one are gone */

/* --- clients ------------------------------------------------------------------- */

static uint8_t listen_on(uint16_t port)
{
    SOCK_INF listener = {0};
    uint8_t id;

    listener.SourPort = port;
    listener.ProtoType = PROTO_TYPE_TCP;
    if(WCHNET_SocketCreat(&id, &listener) != WCHNET_ERR_SUCCESS) return 0;
    return WCHNET_SocketListen(id) == WCHNET_ERR_SUCCESS;
}

/* A client connected; id is its own socket, and the local port tells which
 * listener took it. False when it was turned away. */
static uint8_t admit(uint8_t id)
{
    uint16_t port = (uint16_t)SocketInf[id].SourPort;

    /* A session whose slot was just handed to this client is over, and the
     * stack wiped its disconnect flag when it reused the slot. */
    Raw_Check();
    Scpi_Check();

    if(port == RAW_PORT)
    {
        Raw_Connect(id);
    }
    else if((port == SCPI_PORT) && !Scpi_IsConnected())
    {
        Scpi_Connect(id);
    }
    else
    {
        WCHNET_SocketClose(id, TCP_CLOSE_RST);      /* a second SCPI client */
        return 0;
    }
    return 1;
}

/* The two clients' data is read by Raw_Poll() and Scpi_Poll(). Anything else
 * is dropped, but only from a socket that is still open. */
static void drop_unclaimed(uint8_t id)
{
    uint32_t len;

    if(Raw_Has(id) || Scpi_Has(id) || (SocketInf[id].SockStatus != SOCK_STAT_OPEN)) return;
    len = WCHNET_SocketRecvLen(id, NULL);
    if(len) WCHNET_SocketRecv(id, NULL, &len);
}

static void on_socket_event(uint8_t id, uint8_t events)
{
    /* Connect comes first: it can be flagged together with receive. */
    if((events & SINT_STAT_CONNECT) && !admit(id)) return;
    if(events & SINT_STAT_RECV) drop_unclaimed(id);
    if(events & (SINT_STAT_DISCONNECT | SINT_STAT_TIM_OUT))
    {
        /* Asked separately: after a slot was reused, both could name it. */
        if(Raw_Has(id)) Raw_Disconnect();
        if(Scpi_Has(id)) Scpi_Disconnect();
    }
}

/* --- network ------------------------------------------------------------------- */

static void on_network_event(void)
{
    uint8_t events = WCHNET_GetGlobalInt();
    uint8_t id, socket_events;

    if(events & GINT_STAT_UNREACH) printf("destination unreachable\r\n");
    if(events & GINT_STAT_IP_CONFLI) printf("IP address conflict\r\n");
    if((events & GINT_STAT_PHY_CHANGE) && (WCHNET_GetPHYStatus() & PHY_Linked_Status)) printf("link up\r\n");
    if(events & GINT_STAT_SOCKET)
    {
        for(id = 0; id < WCHNET_MAX_SOCKET_NUM; id++)
        {
            socket_events = WCHNET_GetSocketInt(id);
            if(socket_events) on_socket_event(id, socket_events);
        }
    }
}

/* Called on the first lease and again on renewals; lease holds the address,
 * the gateway and the netmask, four bytes each. The stack calls this from
 * inside WCHNET_MainTask(), possibly in the middle of a command that is
 * waiting on the load, so the clients are dropped later, by the main loop. */
static uint8_t on_dhcp(uint8_t status, void *lease)
{
    const uint8_t *p = lease;

    if(status || !p)
    {
        printf("DHCP failed: %02x\r\n", status);
        return NoREADY;
    }
    if(memcmp(ip, p, 4))
    {
        readdressed = 1;
        memcpy(ip, p, 4);
        memcpy(gateway, p + 4, 4);
        memcpy(netmask, p + 8, 4);
        printf("address %d.%d.%d.%d\r\n", ip[0], ip[1], ip[2], ip[3]);
    }
    if(!listening) listening = listen_on(SCPI_PORT) & listen_on(RAW_PORT);
    return READY;
}

static void network_start(void)
{
    /* Each port takes one client, so one that vanished must be noticed quickly
     * or it blocks the next: probe after 10 s idle, give up after 3 x 5 s. */
    struct _KEEP_CFG keepalive = {.KLIdle = 10000, .KLIntvl = 5000, .KLCount = 3};

    if(WCHNET_GetVer() != WCHNET_LIB_VER) printf("network library is not the version its header describes\r\n");
    WCHNET_GetMacAddr(mac);
    printf("mac %02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    WCHNET_DHCPSetHostname(DHCP_HOSTNAME);
    if(ETH_LibInit(ip, gateway, netmask, mac) != WCHNET_ERR_SUCCESS)
    {
        printf("network library failed to start, halted\r\n");
        for(;;);                            /* the watchdog is not running yet, so this stays */
    }
    WCHNET_ConfigKeepLive(&keepalive);
    WCHNET_DHCPStart(on_dhcp);              /* the first lease opens the listening sockets */
}

/* --- start and main loop ------------------------------------------------------- */

int main(void)
{
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);
    printf("eth485\r\n");

    Tick_Init();
    Uart_Init();
    Scpi_Init();
    network_start();
    Wdg_Init();
    printf("reset cause: %s\r\n", Wdg_ResetCause());

    /* Whatever the board was doing before this start, the load's input goes
     * off: after a hang, a power cut or a reflash nobody may be watching it. */
    Failsafe_Trip();

    for(;;)
    {
        Net_Service();
        if(WCHNET_QueryGlobalInt()) on_network_event();
        if(readdressed)
        {
            readdressed = 0;
            Raw_Close();
            Scpi_Close();
        }
        Raw_Poll();
        Scpi_Poll();
    }
}
