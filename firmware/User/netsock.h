/* TCP client sessions that survive the connection ending at any moment.
 *
 * The stack can free a connection inside any WCHNET_MainTask() call, and the
 * gateway calls MainTask while it waits on the load, so a socket id held by
 * the application can go stale between event polls. The next client can even
 * be accepted into the same slot, which also wipes the old slot's unread
 * event flags. A session therefore remembers its peer's address and port and
 * checks them before every use of the socket.
 *
 * WCHNET_SocketRecv must not be called once the connection is gone: it
 * dereferences the freed connection. Bytes left in a closed socket are copied
 * straight out of its receive buffer instead. */
#ifndef NETSOCK_H
#define NETSOCK_H

#include <stdint.h>

typedef struct {
    uint8_t  id;
    uint8_t  active;
    uint8_t  ip[4];
    uint16_t port;
} net_session_t;

void     Net_SessionStart(net_session_t *s, uint8_t id);
uint8_t  Net_SessionOwnsSlot(const net_session_t *s);  /* the slot still holds this client, open or closed */
uint8_t  Net_SessionOpen(const net_session_t *s);      /* ... and the connection is still open */
uint32_t Net_SessionPending(const net_session_t *s);   /* unread bytes */
uint32_t Net_SessionRead(net_session_t *s, uint8_t *dst, uint32_t max);

#endif
