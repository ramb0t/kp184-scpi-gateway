/* The raw port: bytes pass unchanged between one TCP client and the RS485
 * line, for software that speaks the load's own protocol.
 *
 * While a client is connected it owns the bus, and SCPI commands that need
 * the load fail. The newest client wins: connecting takes the port over. */
#ifndef RAW_PORT_H
#define RAW_PORT_H

#include <stdint.h>

void    Raw_Connect(uint8_t socketid);  /* a client was accepted on this socket */
void    Raw_Disconnect(void);           /* the client has gone; what it sent still goes out */
void    Raw_Close(void);                /* drop the client from this end */
void    Raw_Check(void);                /* end the session if its slot went to another client */
uint8_t Raw_Has(uint8_t socketid);      /* the client is on this socket */
void    Raw_Poll(void);                 /* call from the main loop: moves the bytes */

#endif
