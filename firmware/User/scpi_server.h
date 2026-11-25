/* SCPI command interpreter for the load, fed from one TCP connection. */
#ifndef SCPI_SERVER_H
#define SCPI_SERVER_H

#include <stdint.h>

#define SCPI_RECV_BUF_LEN   512     /* socket receive buffer for the SCPI connection */

void Scpi_Init(void);
void Scpi_Connect(uint8_t socketid);
void Scpi_Disconnect(void);
uint8_t Scpi_IsConnected(void);
uint8_t Scpi_Socket(void);
void Scpi_Poll(void);               /* call from the main loop: runs any complete command lines */

#endif
