/* SCPI command interpreter for the load, fed from one TCP connection. */
#ifndef SCPI_SERVER_H
#define SCPI_SERVER_H

#include <stdint.h>

void    Scpi_Init(void);
void    Scpi_Connect(uint8_t socketid); /* a client was accepted on this socket */
void    Scpi_Disconnect(void);          /* the client has gone; its complete lines are still run */
void    Scpi_Close(void);               /* drop the client from this end */
void    Scpi_Check(void);               /* end the session if its slot went to another client */
uint8_t Scpi_IsConnected(void);
uint8_t Scpi_Has(uint8_t socketid);     /* the client is on this socket */
void    Scpi_Poll(void);                /* call from the main loop: a pending fail-safe, then the next command lines */

#endif
