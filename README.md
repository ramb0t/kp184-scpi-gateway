# eth485

Ethernet-to-RS485 bridge that puts a KKP184 electronic load on the LAN, built on a WCH CH32V208 Ethernet dev board.
So far the board runs WCH's DHCP example built from this repo; the bridge firmware, wiring and the load's protocol notes are still to come.

## Build and flash

`make sdk` fetches the pinned WCH SDK subset into `third_party/`, `make` builds the DHCP example, and `make flash` programs it over a WCH-Link with `wlink` and compares a readback. `EXAMPLE=ETH_UART` selects WCH's serial-to-Ethernet example instead.

The examples need WCH's GCC build, because they use `WCH-Interrupt-fast` and the `xw` extension. The Makefile expects it at `~/toolchains/wch-riscv-gcc12`; `TOOLCHAIN=` overrides that.
