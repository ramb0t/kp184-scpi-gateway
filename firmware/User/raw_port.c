#include "debug.h"
#include "eth_driver.h"
#include "raw_port.h"
#include "netsock.h"
#include "kp184.h"
#include "uart.h"

static net_session_t session;           /* the client, while session.active */
static uint8_t recv_buffer[RECE_BUF_LEN];

/* The bus goes back to SCPI once the load has finished answering this client. */
static void end(void)
{
    session.active = 0;
    KP184_BusRelease();
    printf("raw client gone\r\n");
}

/* A block is taken from the socket only when the uart has room for it, so a
 * fast client is held back by TCP flow control instead of losing data. */
static void client_to_bus(void)
{
    uint8_t *block = Uart_TxBlock();
    uint32_t len;

    if(!block) return;
    len = Net_SessionRead(&session, block, UART_TX_BLOCK_SIZE);
    if(len) Uart_TxQueue((uint16_t)len);
}

/* Only the bytes the stack accepted are consumed, so a send that could not
 * be queued is tried again. */
static void bus_to_client(void)
{
    uint8_t *p;
    uint32_t len = Uart_RxPeek(&p);

    if(len && (WCHNET_SocketSend(session.id, p, &len) == WCHNET_ERR_SUCCESS) && len)
        Uart_RxConsume((uint16_t)len);
}

/* The newest client wins, as with ser2net's kickolduser: a client that lost
 * its connection without closing it would otherwise lock everyone out until
 * keepalive notices, 25 s later. */
void Raw_Connect(uint8_t socketid)
{
    Raw_Close();
    Net_SessionStart(&session, socketid, recv_buffer, sizeof(recv_buffer));
    KP184_BusClaim();                   /* waits out the previous client's last exchange */
    Uart_RxFlush();                     /* forget bytes that arrived while nobody was connected */
    printf("raw client connected\r\n");
}

void Raw_Disconnect(void)
{
    if(!session.active) return;
    client_to_bus();
    end();
}

void Raw_Close(void)
{
    if(!session.active) return;
    Net_SessionDrop(&session);
    end();
}

void Raw_Check(void)
{
    if(session.active && !Net_SessionOwnsSlot(&session)) end();
}

uint8_t Raw_Has(uint8_t socketid)
{
    return session.active && (session.id == socketid);
}

void Raw_Poll(void)
{
    Raw_Check();
    if(session.active)
    {
        client_to_bus();
        if(Net_SessionOpen(&session)) bus_to_client();
        else if(!Net_SessionPending(&session)) end();   /* closed, and everything it sent is queued */
    }
    Uart_TxKick();                      /* what a client queued still goes out after it has left */
}
