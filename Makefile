# Shortcuts for the terminal experiment. Run from the repo root:
#
#   make calc:term:build     build calc/term/build/term.v2z
#   make calc:term:send      build it and send it to the calculator (SilverLink)
#   make shell               run a shell here with the calculator as its terminal
#                            (another command: make shell CMD=top)
#                            (also show it here and type here: make shell MIRROR=1)
#
# For the ESP32 the target starts with the board, esp32_stamps3 or
# esp32_cardputer, then the sketch, then build or flash:
#
#   make esp32_stamps3:term:build        the terminal bridge, esp32_tilink_term
#   make esp32_stamps3:term:flash
#   make esp32_stamps3:linetest:build    the wiring check, esp32_tilink_linetest
#   make esp32_stamps3:linetest:flash
#   make esp32_cardputer:term:build
#   make esp32_cardputer:term:flash
#   make esp32_cardputer:linetest:build
#   make esp32_cardputer:linetest:flash
#
# build compiles into build/<board>-<sketch>/ and flashes nothing. On the
# Cardputer the .bin file there can go on the SD card for M5 Launcher to
# install. flash compiles and uploads over USB, which on the Cardputer
# replaces M5 Launcher.
#
# The ESP32's port is found automatically; override with PORT=/dev/cu.xxx.
# With both boards plugged in, say which.

PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))

STAMPS3_FQBN ?= esp32:esp32:m5stack_stamp_s3
CARDPUTER_FQBN ?= esp32:esp32:m5stack_cardputer
# With the screen's library the terminal sketch no longer fits the Cardputer's
# default 1.3 MB app partition, so it takes the 8 MB layout. That layout needs
# the flash size said too: otherwise the bootloader assumes 4 MB and refuses
# the partition table.
CARDPUTER_TERM_FQBN ?= $(CARDPUTER_FQBN):FlashSize=8M,PartitionScheme=default_8MB

# $(call compile,fqbn,board,sketch) and the same with an upload.
compile = arduino-cli compile --fqbn $(1) --output-dir build/$(2)-$(3) esp32/esp32_tilink_$(3)
upload = $(call compile,$(1),$(2),$(3)) --upload -p $(PORT)
need_port = @test -n "$(PORT)" || { echo "No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass PORT=..."; exit 1; }
sd_hint = @echo "For M5 Launcher: copy build/cardputer-$(1)/esp32_tilink_$(1).ino.bin to the SD card."

calc\:term\:build:
	$(MAKE) -C calc/term

calc\:term\:send:
	$(MAKE) -C calc/term send

esp32_stamps3\:term\:build:
	$(call compile,$(STAMPS3_FQBN),stamps3,term)

esp32_stamps3\:term\:flash:
	$(need_port)
	$(call upload,$(STAMPS3_FQBN),stamps3,term)

esp32_stamps3\:linetest\:build:
	$(call compile,$(STAMPS3_FQBN),stamps3,linetest)

esp32_stamps3\:linetest\:flash:
	$(need_port)
	$(call upload,$(STAMPS3_FQBN),stamps3,linetest)

esp32_cardputer\:term\:build:
	$(call compile,$(CARDPUTER_TERM_FQBN),cardputer,term)
	$(call sd_hint,term)

esp32_cardputer\:term\:flash:
	$(need_port)
	$(call upload,$(CARDPUTER_TERM_FQBN),cardputer,term)

esp32_cardputer\:linetest\:build:
	$(call compile,$(CARDPUTER_FQBN),cardputer,linetest)
	$(call sd_hint,linetest)

esp32_cardputer\:linetest\:flash:
	$(need_port)
	$(call upload,$(CARDPUTER_FQBN),cardputer,linetest)

shell:
	$(need_port)
	python3 tools/shell.py --port $(PORT) $(if $(MIRROR),--mirror) $(CMD)

.PHONY: shell calc\:term\:build calc\:term\:send \
	esp32_stamps3\:term\:build esp32_stamps3\:term\:flash \
	esp32_stamps3\:linetest\:build esp32_stamps3\:linetest\:flash \
	esp32_cardputer\:term\:build esp32_cardputer\:term\:flash \
	esp32_cardputer\:linetest\:build esp32_cardputer\:linetest\:flash
