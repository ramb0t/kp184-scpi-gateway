/* Build options for the SCPI parser library (j123b567/scpi-parser). */
#ifndef SCPI_USER_CONFIG_H
#define SCPI_USER_CONFIG_H

#define SCPI_LINE_ENDING                        "\n"
#define USE_FULL_ERROR_LIST                     1
#define USE_DEVICE_DEPENDENT_ERROR_INFORMATION  0
#define USE_CUSTOM_DTOSTRE                      1   /* newlib-nano's printf has no floating point */

/* Device-specific errors are positive numbers in SCPI. */
#define USE_USER_ERROR_LIST                     1
#define LIST_OF_USER_ERRORS \
    X(SCPI_ERROR_BUS_IN_USE, 100, "RS485 port in use by a raw TCP client")

#endif
