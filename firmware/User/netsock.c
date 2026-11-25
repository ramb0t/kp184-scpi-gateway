#include <string.h>
#include "netsock.h"
#include "eth_driver.h"

void Net_SessionStart(net_session_t *s, uint8_t id)
{
    s->id = id;
    memcpy(s->ip, SocketInf[id].IPAddr, 4);
    s->port = (uint16_t)SocketInf[id].DesPort;
    s->active = 1;
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
