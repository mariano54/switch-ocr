PROJECT_ROOT := $(CURDIR)
SWITCH_APP_DIR := /work/switch/hello_ocr_nro
OVERLAY_DIR := /work/switch/overlay
SYSMODULE_DIR := /work/switch/sysmodule
DEVKIT_IMAGE := devkitpro/devkita64

MAC_IP ?= 192.168.0.124
SWITCH_IP ?=

.PHONY: server switch-build switch-run switch-clean overlay-build overlay-clean overlay-install overlay-install-reload sysmodule-build sysmodule-clean sysmodule-install sysmodule-enable-boot switch-ocr-install tesla-hotkey-install

server:
	python3 server/app.py --host 0.0.0.0 --port 8742

switch-build:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(SWITCH_APP_DIR)" "$(DEVKIT_IMAGE)" \
		make SERVER_HOST=$(MAC_IP)

switch-run:
ifeq ($(strip $(SWITCH_IP)),)
	$(error "Set SWITCH_IP, for example: make switch-run SWITCH_IP=192.168.0.42")
endif
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(SWITCH_APP_DIR)" "$(DEVKIT_IMAGE)" \
		make run SERVER_HOST=$(MAC_IP) SWITCH_IP=$(SWITCH_IP)

switch-clean:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(SWITCH_APP_DIR)" "$(DEVKIT_IMAGE)" make clean

overlay-build:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(OVERLAY_DIR)" "$(DEVKIT_IMAGE)" \
		make SERVER_HOST=$(MAC_IP)

overlay-clean:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(OVERLAY_DIR)" "$(DEVKIT_IMAGE)" make clean

sysmodule-build:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(SYSMODULE_DIR)" "$(DEVKIT_IMAGE)" \
		make SERVER_HOST=$(MAC_IP)

sysmodule-clean:
	docker run --rm -v "$(PROJECT_ROOT):/work" -w "$(SYSMODULE_DIR)" "$(DEVKIT_IMAGE)" make clean

tools/mtp_put_file: tools/mtp_put_file.c
	cc "$<" $$(/opt/homebrew/bin/pkg-config --cflags --libs libmtp) -o "$@"

overlay-install: overlay-build tools/mtp_put_file
	tools/mtp_put_file 0x00010001 switch/overlay/switch-ocr.ovl switch/.overlays/switch-ocr.ovl

overlay-install-reload: overlay-install tools/mtp_put_file
	tools/mtp_put_file 0x00010001 tmp/nx_ovlloader_reload_flag.bin config/nx-ovlloader/reload_flag.bin

sysmodule-install: sysmodule-build tools/mtp_put_file
	tools/mtp_put_file 0x00010001 switch/sysmodule/out/atmosphere/contents/42000000534F4352/exefs.nsp atmosphere/contents/42000000534F4352/exefs.nsp
	tools/mtp_put_file 0x00010001 switch/sysmodule/out/atmosphere/contents/42000000534F4352/toolbox.json atmosphere/contents/42000000534F4352/toolbox.json
	tools/mtp_put_file 0x00010001 switch/sysmodule/out/atmosphere/contents/42000000534F4352/flags/boot2.flag atmosphere/contents/42000000534F4352/flags/boot2.flag

sysmodule-enable-boot: tools/mtp_put_file
	tools/mtp_put_file 0x00010001 tmp/nx_ovlloader_reload_flag.bin atmosphere/contents/42000000534F4352/flags/boot2.flag

switch-ocr-install: sysmodule-install overlay-install

tesla-hotkey-install: tools/mtp_put_file
	tools/mtp_put_file 0x00010001 tmp/tesla_config.ini config/tesla/config.ini
