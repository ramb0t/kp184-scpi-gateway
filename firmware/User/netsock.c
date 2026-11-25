#include <string.h>
#include "netsock.h"
#include "eth_driver.h"
#include "tick.h"
#include "wdg.h"

void Net_Service(void)
{
    WCHNET_MainTask();
    Wdg_Feed();
}

void Net_ServiceFor(uint16_t ticks)
{
    uint16_t t0 = Tick_Now();

    while(Tick_Since(t0) < ticks) Net_Service();
}

void Net_SessionStart(net_session_t *s, uint8_t id, uint8_t *recv_buffer, uint32_t size)
{
    s->id = id;
    memcpy(s->ip, SocketInf[id].IPAddr, 4);
    s->port = (uint16_t)SocketInf[id].DesPort;
    s->active = 1;
    WCHNET_SocketSetKeepLive(id, ENABLE);
    WCHNET_ModifyRecvBuf(id, (uint32_t)recv_buffer, size);
}

uint8_t Net_SessionOwnsSlot(const net_session_t *s)
{
    const SOCK_INF *si = &SocketInf[s->id];

    return s->active && (si->ProtoType == PROTO_TYPE_TCP) && (si->DesPort == s->port) &&
           (memcmp(si->IPAddr, s->ip, 4) == 0);
}

uint8_t Net_SessionOpen(const net_session_t *s)
{
    return Net_SessionOwnsSlot(s) && (SocketInf[s->id].SockStatus == SOCK_STAT_OPEN);
}

/* A reset, not a close: it frees the connection at once. After a plain close
 * the client could go on sending for a while, into a receive buffer that by
 * then belongs to its successor. */
void Net_SessionDrop(const net_session_t *s)
{
    if(Net_SessionOpen(s)) WCHNET_SocketClose(s->id, TCP_CLOSE_RST);
}

uint32_t Net_SessionPending(const net_session_t *s)
{
    return Net_SessionOwnsSlot(s) ? SocketInf[s->id].RecvRemLen : 0;
}

uint32_t Net_SessionRead(net_session_t *s, uint8_t *dst, uint32_t max)
{
    SOCK_INF *si = &SocketInf[s->id];
    uint32_t len, n, rd, start, end;

    if(!Net_SessionOwnsSlot(s)) return 0;
    if(si->SockStatus == SOCK_STAT_OPEN)
    {
        len = WCHNET_SocketRecvLen(s->id, NULL);
        if(len == 0) return 0;
        if(len > max) len = max;
        if(WCHNET_SocketRecv(s->id, dst, &len) != WCHNET_ERR_SUCCESS) return 0;
        return len;
    }
    /* Closed: copy what is left by hand, from the circular receive buffer. */
    start = si->RecvStartPoint;
    end = start + si->RecvBufLen;
    rd = si->RecvReadPoint;
    len = si->RecvRemLen;
    if((si->RecvBufLen == 0) || (rd < start) || (rd >= end) || (len > si->RecvBufLen)) return 0;
    if(len > max) len = max;
    for(n = 0; n < len; n++)
    {
        dst[n] = *(const uint8_t *)rd;
        if(++rd == end) rd = start;
    }
    si->RecvReadPoint = rd;
    si->RecvRemLen -= len;
    return len;
}
