# Builds one of WCH's CH32V208 Ethernet examples from a pinned SDK checkout.
# Flags mirror the example's MounRiver project (.cproject): rv32imac + WCH xw, ilp32, -Os, newlib-nano.
#
#   make sdk                   fetch the SDK subset into third_party/ (once)
#   make [EXAMPLE=DHCP]        build build/<EXAMPLE>/<EXAMPLE>.{elf,bin,hex}
#   make flash [EXAMPLE=...]   program over WCH-Link with wlink, then read back and compare

EXAMPLE   ?= DHCP
TOOLCHAIN ?= $(HOME)/toolchains/wch-riscv-gcc12/bin/riscv-wch-elf-
WLINK     ?= wlink
# UART the examples' printf goes to: 1 = PA9, 2 = PA2, 3 = PB10. The board's TXD/RXD header is
# USART2 (PA2/PA3), so that is the default, except for ETH_UART, which uses USART2 for its data.
ifeq ($(EXAMPLE),ETH_UART)
DEBUG_UART ?= 1
else
DEBUG_UART ?= 2
endif

SDK_URL := https://github.com/openwch/ch32v20x.git
SDK_REV := b375bbbc7dfe8642d6fb7c17e6d3207ee1c3c9c1
SDK_DIR := third_party/ch32v20x
SDK_SUBSET := EVT/EXAM/SRC EVT/EXAM/ETH/NetLib EVT/EXAM/ETH/DHCP EVT/EXAM/ETH/ETH_UART

EXAM := $(SDK_DIR)/EVT/EXAM
SRC  := $(EXAM)/SRC
NET  := $(EXAM)/ETH/NetLib
APP  := $(EXAM)/ETH/$(EXAMPLE)
OUT  := build/$(EXAMPLE)

CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size

C_SRCS := $(wildcard $(APP)/User/*.c) \
          $(wildcard $(SRC)/Peripheral/src/*.c) \
          $(SRC)/Core/core_riscv.c $(SRC)/Debug/debug.c $(NET)/eth_driver.c
S_SRCS := $(SRC)/Startup/startup_ch32v20x_D8W.S
OBJS   := $(addprefix $(OUT)/,$(notdir $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)))
vpath %.c $(sort $(dir $(C_SRCS)))
vpath %.S $(sort $(dir $(S_SRCS)))

ARCH    := -march=rv32imac_xw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
CFLAGS  := $(ARCH) -Os -g -std=gnu99 -fmessage-length=0 -fsigned-char \
           -ffunction-sections -fdata-sections -fno-common -Wunused \
           -DCH32V20x_D8W -DDEBUG=$(DEBUG_UART) \
           -I$(APP)/User -I$(SRC)/Core -I$(SRC)/Debug -I$(SRC)/Peripheral/inc -I$(NET)
LDFLAGS := $(ARCH) -T $(APP)/Ld/Link.ld -nostartfiles -Wl,--gc-sections \
           -Wl,-Map=$(OUT)/$(EXAMPLE).map -Wl,--print-memory-usage \
           --specs=nano.specs --specs=nosys.specs -L$(NET)
LDLIBS  := -lwchnet

.PHONY: all sdk flash clean FORCE
all: $(OUT)/$(EXAMPLE).bin $(OUT)/$(EXAMPLE).hex
	$(SIZE) $(OUT)/$(EXAMPLE).elf

$(OUT)/$(EXAMPLE).elf: $(OBJS) $(NET)/libwchnet.a $(APP)/Ld/Link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

$(OUT)/%.o: %.c $(OUT)/.cflags
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/%.o: %.S $(OUT)/.cflags
	$(CC) $(CFLAGS) -x assembler-with-cpp -MMD -MP -c $< -o $@

# Rewritten only when the flags change, so a new DEBUG_UART rebuilds the objects.
$(OUT)/.cflags: FORCE | $(OUT)
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@

$(OUT):
	mkdir -p $@

# Link.ld places flash at 0, the alias of 0x08000000, so the address is given explicitly.
flash: $(OUT)/$(EXAMPLE).bin
	$(WLINK) flash --address 0x08000000 $<
	$(WLINK) dump 0x08000000 $$(stat -c %s $<) --out $(OUT)/readback.bin
	cmp -n $$(stat -c %s $<) $< $(OUT)/readback.bin && echo "readback matches"
	$(WLINK) reset run

sdk:
	test -d $(SDK_DIR)/.git || git clone --filter=blob:none --sparse --no-checkout $(SDK_URL) $(SDK_DIR)
	git -C $(SDK_DIR) sparse-checkout set $(SDK_SUBSET)
	git -C $(SDK_DIR) checkout -q $(SDK_REV)

clean:
	rm -rf build

-include $(OBJS:.o=.d)
