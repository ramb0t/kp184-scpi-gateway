#include "debug.h"
#include "failsafe.h"
#include "kp184.h"

#define TRIES   3

static uint8_t tries_left;

void Failsafe_Trip(void)
{
    tries_left = TRIES;
}

void Failsafe_Poll(void)
{
    kp184_err_t r;

    if(!tries_left) return;
    r = KP184_SetInput(0);
    if(r == KP184_ERR_BUSY) return;             /* a raw client holds the bus: wait for it to leave */
    tries_left = (r == KP184_OK) ? 0 : tries_left - 1;
    if(!tries_left) printf("fail-safe input off: %s\r\n", (r == KP184_OK) ? "done" : "load not answering");
}
