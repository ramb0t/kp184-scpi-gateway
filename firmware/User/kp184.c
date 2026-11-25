#include <string.h>
#include "kp184.h"
#include "bsp_uart.h"
#include "tick.h"
#include "netsock.h"
#include "eth_driver.h"

/* The load starts answering 5 to 8 ms after a request. */
#define REPLY_TIMEOUT   TICK_MS(300)
/* A reply ends when the line has been quiet this long. The load's own byte
 * count field cannot be used: the status block says 0x1e in a 23-byte frame. */
#define FRAME_GAP       TICK_MS(10)
/* After our last stop bit, wait this long and drop what was received, so that
 * an RS485 module which hears its own transmission does no harm. Two
 * character times at 57600 baud; the reply cannot start this early. An echo
 * that arrives later than that is recognised by its content instead. */
#define ECHO_GUARD      4
#define RETRY_PAUSE     TICK_MS(20)
/* The longest reply, 23 bytes, takes 4 ms; give up collecting after this. */
#define COLLECT_LIMIT   TICK_MS(100)
/* After the raw client: how long the bus must stay silent before a request,
 * longer than the load's reply time plus its longest reply. */
#define SETTLE_QUIET    TICK_MS(30)
#define SETTLE_LIMIT    TICK_MS(500)
#define MAX_FRAME       48
/* A transceiver switching direction can leave a stray byte before or after the reply. */
#define MAX_LEADING     3

volatile uint8_t KP184_BusLocked = 0;
static uint8_t settle;

void KP184_BusRelease(void)
{
    settle = 1;
    KP184_BusLocked = 0;
}

/* Wait until the raw client's queued bytes have gone out and the bus has been
 * silent for SETTLE_QUIET, so its last reply cannot be taken for ours. */
static void settle_bus(void)
{
    uint16_t t0 = Tick_Now(), quiet = Tick_Now();
    uint8_t *p;

    while(Tick_Since(t0) < SETTLE_LIMIT)
    {
        Uart_TxKick();
        Net_Service();
        if(!Uart_TxIdle() || Uart_RxPeek(&p))
        {
            Uart_RxFlush();
            quiet = Tick_Now();
        }
        else if(Tick_Since(quiet) >= SETTLE_QUIET)
        {
            break;
        }
    }
    Uart_RxFlush();
    settle = 0;
}

static uint16_t crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    uint8_t i;

    while(n--)
    {
        crc ^= *p++;
        for(i = 0; i < 8; i++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

static uint8_t finish(uint8_t *req, uint8_t n)
{
    uint16_t crc = crc16(req, n);

    req[n] = (uint8_t)crc;              /* low byte first, as standard Modbus */
    req[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

static uint32_t be(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0;

    while(n--) v = (v << 8) | *p++;
    return v;
}

/* One request and its reply of known length. On success frame holds the
 * reply from its address byte to its CRC. The network stack keeps running
 * while we wait. */
static kp184_err_t xfer_once(const uint8_t *req, uint8_t reqlen, uint8_t *frame, uint8_t replylen)
{
    uint8_t rx[MAX_FRAME + 16];
    uint8_t n = 0, off, start = 0, *p;
    uint16_t avail, take, t0, first;

    Uart_SendBlocking(req, reqlen);
    t0 = Tick_Now();
    while(Tick_Since(t0) < ECHO_GUARD);
    Uart_RxFlush();

    t0 = Tick_Now();
    while(Uart_RxPeek(&p) == 0)
    {
        if(Tick_Since(t0) > REPLY_TIMEOUT) return KP184_ERR_TIMEOUT;
        Net_Service();
    }
    t0 = first = Tick_Now();
    for(;;)
    {
        avail = Uart_RxPeek(&p);
        if(avail)
        {
            take = avail;
            if(take > sizeof(rx) - n) take = sizeof(rx) - n;
            memcpy(rx + n, p, take);
            n += take;
            Uart_RxConsume(avail);
            t0 = Tick_Now();
        }
        else if(Tick_Since(t0) > FRAME_GAP) break;
        if(Tick_Since(first) > COLLECT_LIMIT) break;    /* a line that never falls silent */
    }

    /* A copy of the request is a late echo, not a reply: skip it, and if
     * nothing follows, the load did not answer. */
    if((n >= reqlen) && (memcmp(rx, req, reqlen) == 0))
    {
        if(n == reqlen) return KP184_ERR_TIMEOUT;
        start = reqlen;
    }

    /* Find the reply by its length, address, function and CRC, allowing a
     * few stray bytes before and after it. */
    for(off = start; (off <= start + MAX_LEADING) && (off + replylen <= n); off++)
    {
        if((rx[off] == req[0]) && (rx[off + 1] == req[1]) &&
           (crc16(rx + off, replylen - 2) == (rx[off + replylen - 2] | ((uint16_t)rx[off + replylen - 1] << 8))))
        {
            memcpy(frame, rx + off, replylen);
            return KP184_OK;
        }
    }
    return KP184_ERR_FRAME;
}

static kp184_err_t xfer(const uint8_t *req, uint8_t reqlen, uint8_t *frame, uint8_t replylen)
{
    kp184_err_t r;
    uint16_t t0;

    if(KP184_BusLocked) return KP184_ERR_BUSY;
    if(settle) settle_bus();
    r = xfer_once(req, reqlen, frame, replylen);
    if(r != KP184_OK)
    {
        /* Every request only reads or sets a value, so repeating one is harmless. */
        t0 = Tick_Now();
        while(Tick_Since(t0) < RETRY_PAUSE) Net_Service();
        r = xfer_once(req, reqlen, frame, replylen);
    }
    return r;
}

kp184_err_t KP184_Read(uint16_t reg, uint32_t *value)
{
    uint8_t req[8] = {KP184_ADDR, 0x03, reg >> 8, reg & 0xFF, 0x00, 0x04};
    uint8_t frame[MAX_FRAME];
    kp184_err_t r = xfer(req, finish(req, 6), frame, 9);

    if(r != KP184_OK) return r;
    if(frame[2] != 4) return KP184_ERR_FRAME;
    *value = be(frame + 3, 4);
    if((reg == KP184_REG_OUTPUT) || (reg == KP184_REG_MODE)) *value &= 0xFFFF;
    return KP184_OK;
}

kp184_err_t KP184_Write(uint16_t reg, uint32_t value)
{
    uint8_t req[13] = {KP184_ADDR, 0x06, reg >> 8, reg & 0xFF, 0x00, 0x01, 0x04,
                       value >> 24, value >> 16, value >> 8, value};
    uint8_t frame[MAX_FRAME];
    /* Firmware 2128 acknowledges with address, function, register and the low
     * 16 bits of the value. The manual describes an echo of the whole request
     * instead; that form is not accepted, because it cannot be told apart
     * from an RS485 module echoing the request to a load that stays silent. */
    kp184_err_t r = xfer(req, finish(req, 11), frame, 8);

    if(r != KP184_OK) return r;
    if((be(frame + 2, 2) == reg) && (be(frame + 4, 2) == (value & 0xFFFF)))
        return KP184_OK;
    return KP184_ERR_FRAME;
}

kp184_err_t KP184_Status(kp184_status_t *st)
{
    uint8_t req[8] = {KP184_ADDR, 0x03, KP184_REG_STATUS >> 8, KP184_REG_STATUS & 0xFF, 0x00, 0x00};
    uint8_t frame[MAX_FRAME];
    kp184_err_t r = xfer(req, finish(req, 6), frame, 23);   /* the short status block */

    if(r != KP184_OK) return r;
    st->output_on = frame[3] & 1;
    st->voltage_mv = be(frame + 5, 3);
    st->current_ma = be(frame + 8, 3);
    return KP184_OK;
}
