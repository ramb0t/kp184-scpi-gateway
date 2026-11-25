# eth485

Network gateway that puts a Kunkin KP184 electronic load on the LAN, built on a WCH CH32V208 Ethernet board wired to the load's RS485 port.
It answers SCPI on TCP port 5025, so the load can be driven like any LAN bench instrument, and also passes raw bytes on port 4001 for software that speaks the load's own protocol.

## Using it

The board takes its address by DHCP under the hostname `eth485`. Each port takes one client at a time: a second SCPI client is refused, while a new raw client takes over from the old one.

The load's input goes off whenever the gateway starts, including after a watchdog reset (the watchdog restarts a hung gateway within about 4 s), and when an SCPI client that switched the input on goes away without switching it off: closed, timed out or network gone. A client that wants the load to keep running after it disconnects sends `SYST:FAIL OFF` first; the setting lasts for that connection only. Clients that never switched the input on leave it alone, and raw-port clients are not covered. Complete command lines a client sent before closing are still run, so `INP OFF` followed by an immediate close works.

SCPI, port 5025, lines ending in LF, replies ending in LF:

```
tools/scpi.py eth485 '*IDN?' 'MEAS:VOLT?' 'MEAS:CURR?'
```

| Command | Meaning |
|---|---|
| `FUNC {CURR\|VOLT\|RES\|POW}`, `FUNC?` | mode: CC, CV, CR, CP; only with the input off |
| `CURR <A>`, `VOLT <V>`, `RES <ohm>`, `POW <W>`, each with `?` | setpoint of each mode; also `MIN`, `MAX`, and suffixes such as `500mA` |
| `INP {ON\|OFF}`, `INP?` | input on or off |
| `MEAS:VOLT?`, `MEAS:CURR?`, `MEAS:POW?`, `MEAS:RES?` | readings; `FETC:` works the same |
| `*IDN?`, `*RST`, `*TST?`, `SYST:ERR?` | identity, input off, load reachable (0 = yes), next error |
| `SYST:FAIL {ON\|OFF}`, `SYST:FAIL?` | fail-safe for this connection (default ON) |
| `DIAG:UPT?`, `DIAG:RES:CAUS?` | seconds since the gateway started; why it started: `POWER`, `PIN`, `WATCHDOG` or `SOFTWARE` |

The long forms (`:SOURce:CURRent:LEVel:IMMediate`, `:SOURce:INPut:STATe`, `MEASure:VOLTage:DC?`) work too. This is the command set shared by Rigol DL3000, Siglent SDL1000X, ITECH IT8500+ and BK 8600 loads.

A command that fails returns nothing and queues an error for `SYST:ERR?`: `-365` when the load does not answer, `-360` for a garbled reply, `-221` for a refused mode change, `100` while a raw client holds the RS485 port. In a compound query such as `MEAS:VOLT?;CURR?` a failed part leaves no field, so the fields after it shift; check `SYST:ERR?` before trusting one.

Two behaviours of the load (firmware 2128) differ from its manual. It never takes CV over the bus: it acknowledges `FUNC VOLT`, shows CV for a moment and goes back, so the gateway reports `-221`; set CV on the front panel. And it would change mode with the input on, jumping straight onto the other mode's setpoint, so the gateway refuses `FUNC` while the input is on.

Raw, port 4001: bytes pass unchanged to and from the load at 57600 8N1. While a raw client is connected, SCPI commands that need the load fail with error 100.

## Client software

- **PyVISA** (tested with pyvisa-py): resource `TCPIP::eth485::5025::SOCKET`, `read_termination = write_termination = '\n'`. A query the gateway does not support gets no reply, so it ends in a client timeout; `SYST:ERR?` then says why.
- **lab-tools `kp184.py`** (tested): pyserial URL `socket://eth485:4001`.
- **ngscopeclient** (not yet run): driver `siglent_load`, transport `lan`, address `eth485:5025`. Its driver does not check the identity and needs only commands the gateway has, including the range queries `SOUR:CURR:IRANG?` and `VRANG?`, which return the KP184's single range (40 A, 150 V). Choosing constant voltage in it shows CV while the load stays in its old mode, and picking a range switches the input off and on while changing nothing.
- **Interflexo KP184** (not yet run): its TCP client setting, the gateway's address and port 4001, with the load at 57600 baud. It speaks the load's own protocol over the raw port, so SCPI is unavailable while it is connected.

## Wiring

The board's TXD/RXD header is USART2 (PA2 TX, PA3 RX) at 3.3 V logic. An auto-direction RS485 module (an HW-0519 here) goes between it and the load's A/B terminals. Sellers label this module family's TXD and RXD pins both ways round, so if nothing reaches the load, swap the two data wires. The module's receiver output must not put more than 3.3 V on PA3: power it from the board's 3V3 pin, or divide a 5 V module's output down. PA3 is not marked 5 V tolerant in the CH32V30x pin table, and the CH32V208's own datasheet has not been checked.

On the load: RS485, 57600 baud, address 1, ONLI set to slave.

## Build and flash

`make sdk` fetches the pinned WCH SDK subset and the SCPI parser library into `third_party/`, `make` builds the gateway, and `make flash` programs it over a WCH-Link with `wlink`, compares a readback and resets the board.

`make EXAMPLE=DHCP` or `EXAMPLE=ETH_UART` builds one of WCH's unmodified examples instead. The DHCP example prints its console on the TXD pin at 115200 baud, which makes it the quickest check that a board and its Ethernet link work.

The build needs WCH's GCC, because the SDK uses `WCH-Interrupt-fast` and the `xw` extension. The Makefile expects it at `~/toolchains/wch-riscv-gcc12`; `TOOLCHAIN=` overrides that.

## Testing

Against the real load, with a DC supply of 5 to 30 V on its input: `tools/load_test.py HOST` checks setpoints, mode changes, refused inputs and the input switched on in CC, CR and CW at up to 0.3 A, then restores the load's settings. `tools/race_test.py HOST` checks clients that close or reconnect while the gateway is talking to the load; the input stays off. `tools/kp184_sniff.py` listens on the bus through a second RS485 adapter without transmitting.

Without the load: `tools/kp184_sim.py` answers the load's protocol on a serial port and reproduces replies captured from a real KP184 (`--selftest`). With a USB serial adapter wired to the board's TXD/RXD header in place of the RS485 module, `tools/gateway_test.py HOST SERIAL_PORT` runs the simulator and checks both ports end to end. It refuses to run when a real load answers, because it switches the input on.

## Layout

`firmware/User/` holds the sources: `main.c` (network, ports), `netsock.c` (client sessions that survive the connection ending mid-command), `scpi_server.c` (commands), `kp184.c` (the load's protocol), `bsp_uart.c` (USART2). It started as WCH's ETH_UART example; the tags `dhcp-demo-working`, `eth-uart-bridge-working` and `scpi-gateway-reads-working` mark earlier stages.
