# Builds the eth485 gateway firmware in firmware/, or one of WCH's unmodified CH32V208 Ethernet
# examples, against a pinned SDK checkout.
# Flags mirror the examples' MounRiver project (.cproject): rv32imac + WCH xw, ilp32, -Os, newlib-nano.
#
#   make sdk                   fetch the SDK subset into third_party/ (once)
#   make                       build build/eth485/eth485.{elf,bin,hex} from firmware/
#   make EXAMPLE=DHCP          build an SDK example instead (DHCP or ETH_UART)
#   make flash [EXAMPLE=...]   program over WCH-Link with wlink, read back, compare and reset
#   make EXTRA_CFLAGS=-DWDG_TEST  test build with DIAG:HANG, which stops feeding the watchdog

TOOLCHAIN ?= $(HOME)/toolchains/wch-riscv-gcc12/bin/riscv-wch-elf-
WLINK     ?= wlink
# UART that printf goes to: 1 = PA9, 2 = PA2, 3 = PB10. The board's TXD/RXD header is USART2
# (PA2/PA3). The gateway and ETH_UART carry data on it, so their printf stays on PA9, which is
# not on a header; only the DHCP example prints where it can be seen.
ifeq ($(EXAMPLE),DHCP)
DEBUG_UART ?= 2
else
DEBUG_UART ?= 1
endif

SDK_URL := https://github.com/openwch/ch32v20x.git
SDK_REV := b375bbbc7dfe8642d6fb7c17e6d3207ee1c3c9c1
SDK_DIR := third_party/ch32v20x
SDK_SUBSET := EVT/EXAM/SRC EVT/EXAM/ETH/NetLib EVT/EXAM/ETH/DHCP EVT/EXAM/ETH/ETH_UART

# SCPI parser library, used by the gateway firmware only (v2.3).
SCPI_URL := https://github.com/j123b567/scpi-parser.git
SCPI_REV := 361fe25cb1fe4cacb4f7494944c5d4084eb07f61
SCPI_DIR := third_party/scpi-parser

EXAM := $(SDK_DIR)/EVT/EXAM
SRC  := $(EXAM)/SRC
NET  := $(EXAM)/ETH/NetLib
ifdef EXAMPLE
APP  := $(EXAM)/ETH/$(EXAMPLE)
NAME := $(EXAMPLE)
else
APP  := firmware
NAME := eth485
endif
OUT  := build/$(NAME)

CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size

C_SRCS := $(wildcard $(APP)/User/*.c) \
          $(wildcard $(SRC)/Peripheral/src/*.c) \
          $(SRC)/Core/core_riscv.c $(SRC)/Debug/debug.c $(NET)/eth_driver.c
S_SRCS := $(SRC)/Startup/startup_ch32v20x_D8W.S
ifndef EXAMPLE
C_SRCS += $(wildcard $(SCPI_DIR)/libscpi/src/*.c)
APP_FLAGS := -I$(SCPI_DIR)/libscpi/inc -DSCPI_USER_CONFIG=1
endif
OBJS   := $(addprefix $(OUT)/,$(notdir $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)))
vpath %.c $(sort $(dir $(C_SRCS)))
vpath %.S $(sort $(dir $(S_SRCS)))

ARCH    := -march=rv32imac_xw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
CFLAGS  := $(ARCH) -Os -g -std=gnu99 -fmessage-length=0 -fsigned-char \
           -ffunction-sections -fdata-sections -fno-common -Wunused \
           -DCH32V20x_D8W -DDEBUG=$(DEBUG_UART) $(APP_FLAGS) \
           -I$(APP)/User -I$(SRC)/Core -I$(SRC)/Debug -I$(SRC)/Peripheral/inc -I$(NET) \
           $(EXTRA_CFLAGS)
LDFLAGS := $(ARCH) -T $(APP)/Ld/Link.ld -nostartfiles -Wl,--gc-sections \
           -Wl,-Map=$(OUT)/$(NAME).map -Wl,--print-memory-usage \
           --specs=nano.specs --specs=nosys.specs -L$(NET)
LDLIBS  := -lwchnet

.PHONY: all sdk flash clean FORCE
all: $(OUT)/$(NAME).bin $(OUT)/$(NAME).hex
	$(SIZE) $(OUT)/$(NAME).elf

$(OUT)/$(NAME).elf: $(OBJS) $(NET)/libwchnet.a $(APP)/Ld/Link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

$(OUT)/%.o: %.c $(OUT)/.cflags
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/%.o: %.S $(OUT)/.cflags
	$(CC) $(CFLAGS) -x assembler-with-cpp -MMD -MP -c $< -o $@

# Rewritten only when the compiler or its flags change, so a new DEBUG_UART rebuilds the objects.
$(OUT)/.cflags: FORCE | $(OUT)
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@

$(OUT):
	mkdir -p $@

# Link.ld places flash at 0, the alias of 0x08000000, so the address is given explicitly.
# This order is the one that has worked every time; why is only partly known. The
# firmware's watchdog cannot be stopped and resets a chip left halted for about 4 s,
# and a standalone "wlink reset" has been seen to halt and resume without resetting.
# wlink flash restarts the chip itself, so the new firmware is already running during
# the readback, which takes about 2 s. Do not add --no-run: that leaves the restart to
# the watchdog. The readback compare is what catches a bad write.
flash: $(OUT)/$(NAME).bin
	$(WLINK) reset halt
	$(WLINK) flash --address 0x08000000 $<
	$(WLINK) dump 0x08000000 $$(wc -c < $<) --out $(OUT)/readback.bin
	cmp -n $$(wc -c < $<) $< $(OUT)/readback.bin && echo "readback matches"
	$(WLINK) reset run

# Fetches again only when a pinned revision is not in the clone yet.
sdk:
	test -d $(SDK_DIR)/.git || git clone --filter=blob:none --sparse --no-checkout $(SDK_URL) $(SDK_DIR)
	git -C $(SDK_DIR) cat-file -e $(SDK_REV)^{commit} 2>/dev/null || git -C $(SDK_DIR) fetch -q origin
	git -C $(SDK_DIR) sparse-checkout set $(SDK_SUBSET)
	git -C $(SDK_DIR) checkout -q $(SDK_REV)
	test -d $(SCPI_DIR)/.git || git clone --filter=blob:none --no-checkout $(SCPI_URL) $(SCPI_DIR)
	git -C $(SCPI_DIR) cat-file -e $(SCPI_REV)^{commit} 2>/dev/null || git -C $(SCPI_DIR) fetch -q origin
	git -C $(SCPI_DIR) checkout -q $(SCPI_REV)

clean:
	rm -rf build

-include $(OBJS:.o=.d)
