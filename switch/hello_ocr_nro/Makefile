TARGET := switch_ocr_hello
BUILD := build
SOURCES := source

APP_TITLE := Switch OCR Hello
APP_AUTHOR := SwitchOCR
APP_VERSION := 0.1.0

SERVER_HOST ?= 192.168.0.125
SERVER_PORT ?= 8000
SWITCH_IP ?=

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set. Use the root Docker build target or install devkitPro locally.")
endif

include $(DEVKITPRO)/libnx/switch_rules

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
OFILES := $(CFILES:.c=.o)
VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH)
CFLAGS += -D__SWITCH__ -DSERVER_HOST=\"$(SERVER_HOST)\" -DSERVER_PORT=$(SERVER_PORT)
CFLAGS += -I$(DEVKITPRO)/libnx/include

LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)
LDFLAGS += -Wl,-Map,$(BUILD)/$(TARGET).map

LIBS := -L$(DEVKITPRO)/libnx/lib -lnx

.PHONY: all clean run

all: $(BUILD)/$(TARGET).nro

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(addprefix $(BUILD)/,$(OFILES))
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: | $(BUILD)
	nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(BUILD)/$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp
	elf2nro $< $@ --nacp=$(BUILD)/$(TARGET).nacp

run: all
ifeq ($(strip $(SWITCH_IP)),)
	$(error "Set SWITCH_IP, for example: make run SWITCH_IP=192.168.0.42")
endif
	nxlink -a $(SWITCH_IP) $(BUILD)/$(TARGET).nro

clean:
	rm -rf $(BUILD)
