# eth485

Network gateway that puts a Kunkin KP184 electronic load on the LAN, built on a WCH CH32V208 Ethernet board wired to the load's RS485 port.
It answers SCPI on TCP port 5025, so the load can be driven like any LAN bench instrument, and also passes raw bytes on port 4001 for software that speaks the load's own protocol.

## Using it

The board takes its address by DHCP under the hostname `eth485`. Each port accepts one client at a time.

SCPI, port 5025, lines ending in LF, replies ending in LF:

```
tools/scpi.py eth485 '*IDN?' 'MEAS:VOLT?' 'MEAS:CURR?'
```

| Command | Meaning |
|---|---|
| `FUNC {CURR\|VOLT\|RES\|POW}`, `FUNC?` | mode: CC, CV, CR, CP |
| `CURR <A>`, `VOLT <V>`, `RES <ohm>`, `POW <W>`, each with `?` | setpoint of each mode; also `MIN`, `MAX`, and suffixes such as `500mA` |
| `INP {ON\|OFF}`, `INP?` | input on or off |
| `MEAS:VOLT?`, `MEAS:CURR?`, `MEAS:POW?`, `MEAS:RES?` | readings; `FETC:` works the same |
| `*IDN?`, `*RST`, `*TST?`, `SYST:ERR?` | identity, input off, load reachable (0 = yes), next error |

The long forms (`:SOURce:CURRent:LEVel:IMMediate`, `:SOURce:INPut:STATe`, `MEASure:VOLTage:DC?`) work too. This is the command set shared by Rigol DL3000, Siglent SDL1000X, ITECH IT8500+ and BK 8600 loads. From PyVISA the resource is `TCPIP::eth485::5025::SOCKET` with `read_termination = write_termination = '\n'`.

A command that fails returns nothing and queues an error for `SYST:ERR?`: `-365` when the load does not answer, `-360` for a garbled reply, `-221` when the load refuses a mode change, `100` while a raw client holds the RS485 port.

Raw, port 4001: bytes pass unchanged to and from the load at 57600 8N1. The pyserial URL `socket://eth485:4001` works with `kp184.py` from lab-tools, and Interflexo can use it through its TCP client setting. While a raw client is connected, SCPI commands that need the load fail with error 100.

Nothing switches the load's input off when a client disconnects or the network drops; `*RST` or `INP OFF` does.

## Wiring

The board's TXD/RXD header is USART2 (PA2 TX, PA3 RX) at 3.3 V logic. An auto-direction RS485 module goes between it and the load's A/B terminals. The module's receiver output must not put more than 3.3 V on PA3: use a 3.3 V module powered from the board's 3V3 pin, or divide a 5 V module's output down. PA3 is not marked 5 V tolerant in the CH32V30x pin table, and the CH32V208's own datasheet has not been checked.

On the load: RS485, 57600 baud, address 1, ONLI set to slave.

## Build and flash

`make sdk` fetches the pinned WCH SDK subset and the SCPI parser library into `third_party/`, `make` builds the gateway, and `make flash` programs it over a WCH-Link with `wlink`, compares a readback and resets the board.

`make EXAMPLE=DHCP` or `EXAMPLE=ETH_UART` builds one of WCH's unmodified examples instead. The DHCP example prints its console on the TXD pin at 115200 baud, which makes it the quickest check that a board and its Ethernet link work.

The build needs WCH's GCC, because the SDK uses `WCH-Interrupt-fast` and the `xw` extension. The Makefile expects it at `~/toolchains/wch-riscv-gcc12`; `TOOLCHAIN=` overrides that.

## Testing without the load

`tools/kp184_sim.py` answers the load's protocol on a serial port and reproduces replies captured from a real KP184 (`--selftest`). With a USB serial adapter wired to the board's TXD/RXD header in place of the RS485 module, `tools/gateway_test.py HOST SERIAL_PORT` runs the simulator and checks both ports end to end. Never run it with a real load attached: it switches the input on.

## Layout

`firmware/User/` holds the sources: `main.c` (network, ports), `scpi_server.c` (commands), `kp184.c` (the load's protocol), `bsp_uart.c` (USART2). It started as WCH's ETH_UART example, and the tags `dhcp-demo-working` and `eth-uart-bridge-working` mark the two earlier stages.
