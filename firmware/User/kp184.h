/* Kunkin KP184 electronic load: its serial protocol, spoken over USART2.
 *
 * The protocol looks like Modbus RTU but is not: function 0x03 reads one
 * 4-byte register, function 0x06 writes one, and 0x0300 returns a status
 * block. Frame layouts and quirks follow replies captured from a unit with
 * firmware 2128 (see tools/kp184_sim.py). */
#ifndef KP184_H
#define KP184_H

#include <stdint.h>

#define KP184_ADDR          1

#define KP184_REG_MODEL     0x0001      /* 1840 for a KP184 */
#define KP184_REG_OUTPUT    0x010E      /* 0 off, 1 on; only the low 16 bits of the reply are valid */
#define KP184_REG_MODE      0x0110      /* KP184_MODE_x; only the low 16 bits of the reply are valid */
#define KP184_REG_CV        0x0112      /* mV */
#define KP184_REG_CC        0x0116      /* mA */
#define KP184_REG_CR        0x011A      /* 0.1 ohm per count on firmware 2128, not 1 ohm as the manual says */
#define KP184_REG_CW        0x011E      /* 0.01 W per count on firmware 2128, not 0.1 W as the manual says */
#define KP184_REG_U         0x0122      /* mV */
#define KP184_REG_I         0x0126      /* mA */
#define KP184_REG_STATUS    0x0300      /* status block */

#define KP184_MODEL_KP184   1840

enum { KP184_MODE_CV = 0, KP184_MODE_CC = 1, KP184_MODE_CR = 2, KP184_MODE_CW = 3 };

typedef enum {
    KP184_OK = 0,
    KP184_ERR_TIMEOUT,      /* no reply */
    KP184_ERR_FRAME,        /* a reply that fails the CRC or does not match the request */
    KP184_ERR_BUSY          /* the RS485 port is held by a raw TCP client */
} kp184_err_t;

typedef struct {
    uint8_t  output_on;
    uint32_t voltage_mv;
    uint32_t current_ma;
} kp184_status_t;

/* Set while a raw TCP client owns the RS485 port; every call then returns KP184_ERR_BUSY. */
extern volatile uint8_t KP184_BusLocked;

kp184_err_t KP184_Read(uint16_t reg, uint32_t *value);
kp184_err_t KP184_Write(uint16_t reg, uint32_t value);
kp184_err_t KP184_Status(kp184_status_t *st);

#endif
