/* SCPI command interpreter for the load, fed from one TCP connection. */
#ifndef SCPI_SERVER_H
#define SCPI_SERVER_H

#include <stdint.h>

#define SCPI_RECV_BUF_LEN   512     /* socket receive buffer for the SCPI connection */

void Scpi_Init(void);
void Scpi_Connect(uint8_t socketid);
void Scpi_Disconnect(void);         /* the client has gone; its complete lines are still run */
void Scpi_Close(void);              /* close the client's connection from this end */
void Scpi_Check(void);              /* end the session if its slot went to another client */
uint8_t Scpi_IsConnected(void);
uint8_t Scpi_Socket(void);
void Scpi_Poll(void);               /* call from the main loop: runs the next received command lines */

#endif
