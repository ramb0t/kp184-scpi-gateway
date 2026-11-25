#include <string.h>
#include "kp184.h"
#include "bsp_uart.h"
#include "tick.h"
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
#define MAX_FRAME       48
/* A transceiver switching direction can leave a stray byte before the reply. */
#define MAX_LEADING     3

volatile uint8_t KP184_BusLocked = 0;

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

/* One request and its reply. On success frame/len hold the reply from its
 * address byte to its CRC. The network stack keeps running while we wait. */
static kp184_err_t xfer_once(const uint8_t *req, uint8_t reqlen, uint8_t *frame, uint8_t *len)
{
    uint8_t rx[MAX_FRAME + 16];
    uint8_t n = 0, off, start = 0, *p;
    uint16_t avail, take, t0;

    Uart_SendBlocking(req, reqlen);
    t0 = Tick_Now();
    while(Tick_Since(t0) < ECHO_GUARD);
    Uart_RxFlush();

    t0 = Tick_Now();
    while(Uart_RxPeek(&p) == 0)
    {
        if(Tick_Since(t0) > REPLY_TIMEOUT) return KP184_ERR_TIMEOUT;
        WCHNET_MainTask();
    }
    t0 = Tick_Now();
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
    }

    /* A copy of the request is a late echo, not a reply: skip it, and if
     * nothing follows, the load did not answer. */
    if((n >= reqlen) && (memcmp(rx, req, reqlen) == 0))
    {
        if(n == reqlen) return KP184_ERR_TIMEOUT;
        start = reqlen;
    }

    for(off = start; off <= start + MAX_LEADING; off++)
    {
        if(n < off + 5) break;
        if((rx[off] == req[0]) && (rx[off + 1] == req[1]) &&
           (crc16(rx + off, n - off - 2) == (rx[n - 2] | ((uint16_t)rx[n - 1] << 8))))
        {
            *len = n - off;
            memcpy(frame, rx + off, *len);
            return KP184_OK;
        }
    }
    return KP184_ERR_FRAME;
}

static kp184_err_t xfer(const uint8_t *req, uint8_t reqlen, uint8_t *frame, uint8_t *len)
{
    kp184_err_t r;
    uint16_t t0;

    if(KP184_BusLocked) return KP184_ERR_BUSY;
    r = xfer_once(req, reqlen, frame, len);
    if(r != KP184_OK)
    {
        /* Every request only reads or sets a value, so repeating one is harmless. */
        t0 = Tick_Now();
        while(Tick_Since(t0) < RETRY_PAUSE) WCHNET_MainTask();
        r = xfer_once(req, reqlen, frame, len);
    }
    return r;
}

kp184_err_t KP184_Read(uint16_t reg, uint32_t *value)
{
    uint8_t req[8] = {KP184_ADDR, 0x03, reg >> 8, reg & 0xFF, 0x00, 0x04};
    uint8_t frame[MAX_FRAME], len;
    kp184_err_t r = xfer(req, finish(req, 6), frame, &len);

    if(r != KP184_OK) return r;
    if((len != 9) || (frame[2] != 4)) return KP184_ERR_FRAME;
    *value = be(frame + 3, 4);
    if((reg == KP184_REG_OUTPUT) || (reg == KP184_REG_MODE)) *value &= 0xFFFF;
    return KP184_OK;
}

kp184_err_t KP184_Write(uint16_t reg, uint32_t value)
{
    uint8_t req[13] = {KP184_ADDR, 0x06, reg >> 8, reg & 0xFF, 0x00, 0x01, 0x04,
                       value >> 24, value >> 16, value >> 8, value};
    uint8_t frame[MAX_FRAME], len;
    kp184_err_t r = xfer(req, finish(req, 11), frame, &len);

    if(r != KP184_OK) return r;
    /* Firmware 2128 acknowledges with address, function, register and the low
     * 16 bits of the value. The manual describes an echo of the whole request
     * instead; that form is not accepted, because it cannot be told apart
     * from an RS485 module echoing the request to a load that stays silent. */
    if((len == 8) && (be(frame + 2, 2) == reg) && (be(frame + 4, 2) == (value & 0xFFFF)))
        return KP184_OK;
    return KP184_ERR_FRAME;
}

kp184_err_t KP184_Status(kp184_status_t *st)
{
    uint8_t req[8] = {KP184_ADDR, 0x03, KP184_REG_STATUS >> 8, KP184_REG_STATUS & 0xFF, 0x00, 0x00};
    uint8_t frame[MAX_FRAME], len;
    kp184_err_t r = xfer(req, finish(req, 6), frame, &len);

    if(r != KP184_OK) return r;
    if(len < 13) return KP184_ERR_FRAME;
    st->output_on = frame[3] & 1;
    st->voltage_mv = be(frame + 5, 3);
    st->current_ma = be(frame + 8, 3);
    return KP184_OK;
}
