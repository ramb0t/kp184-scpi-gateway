# eth485

Ethernet-to-RS485 bridge that puts a KKP184 electronic load on the LAN, built on a WCH CH32V208 Ethernet dev board.
So far it is a working TCP-to-UART bridge at 3.3 V logic levels; the RS485 transceiver, wiring and the load's protocol notes are still to come.

## What the firmware does

The board takes an address by DHCP under the hostname `eth485` and listens on TCP port 4001. One client at a time connects, for example with `nc eth485 4001`, and bytes pass transparently between that connection and USART2 (PA2 TX, PA3 RX, the board's TXD/RXD header) at 115200 8N1.

`firmware/` is WCH's ETH_UART example with those changes. The port and hostname are constants at the top of `firmware/User/main.c`, the baud rate is in `firmware/User/bsp_uart.h`.

While one client is connected a second connection attempt goes unanswered. TCP keepalive is set to drop a client that vanishes without closing after about 25 seconds; that has not been tested. As in WCH's example, TCP data that arrives faster than the UART can send it is dropped once three 2920-byte buffers are full.

## Build and flash

`make sdk` fetches the pinned WCH SDK subset into `third_party/`, `make` builds the bridge, and `make flash` programs it over a WCH-Link with `wlink`, compares a readback and resets the board.

`make EXAMPLE=DHCP` or `EXAMPLE=ETH_UART` builds one of WCH's unmodified examples instead. The DHCP example prints its console on the TXD pin at 115200 baud, which makes it the quickest check that a board and its Ethernet link work.

The build needs WCH's GCC, because the SDK uses `WCH-Interrupt-fast` and the `xw` extension. The Makefile expects it at `~/toolchains/wch-riscv-gcc12`; `TOOLCHAIN=` overrides that.
