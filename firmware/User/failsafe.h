/* The load's input must not stay on with nobody watching it.
 *
 * It is switched off whenever the gateway starts, whatever made it restart,
 * and when an SCPI client that switched it on goes away without switching it
 * off (scpi_server.c decides that). A raw client holding the bus delays the
 * switch-off until it has left; a load that does not answer gets three tries. */
#ifndef FAILSAFE_H
#define FAILSAFE_H

void Failsafe_Trip(void);       /* switch the input off as soon as the bus allows */
void Failsafe_Poll(void);       /* carries that out; call before running any client's commands */

#endif
