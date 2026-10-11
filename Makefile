# Shortcuts for the terminal experiment. Run from the repo root:
#
#   make calc:term:build     build calc/term/build/term.v2z
#   make calc:term:send      build it and send it to the calculator (SilverLink)
#   make esp32:term:flash    compile esp32_tilink_term and flash the ESP32 (Stamp S3)
#   make esp32:term:cardputer  build it for the M5Stack Cardputer, as a file for
#                            the SD card (M5 Launcher installs it); flashes nothing
#   make esp32:linetest:cardputer  the same for the wiring-check sketch
#   make shell               run a shell here with the calculator as its terminal
#                            (another command: make shell CMD=top)
#                            (also show it here and type here: make shell MIRROR=1)
#
# The ESP32's port is found automatically; override with PORT=/dev/cu.xxx.

FQBN ?= esp32:esp32:m5stack_stamp_s3
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))

calc\:term\:build:
	$(MAKE) -C calc/term

calc\:term\:send:
	$(MAKE) -C calc/term send

esp32\:term\:flash:
	@test -n "$(PORT)" || { echo "No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass PORT=..."; exit 1; }
	arduino-cli compile --upload -p $(PORT) --fqbn $(FQBN) esp32/esp32_tilink_term

# Only builds. Flashing a Cardputer over USB would overwrite M5 Launcher.
# With the screen's library the sketch no longer fits the default 1.3 MB app
# partition, so it takes the 8 MB layout. That layout needs the flash size said
# too: otherwise the bootloader assumes 4 MB and refuses the partition table.
CARDPUTER_BUILD = build/cardputer

esp32\:term\:cardputer:
	arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB --output-dir $(CARDPUTER_BUILD) esp32/esp32_tilink_term
	@echo "Copy $(CARDPUTER_BUILD)/esp32_tilink_term.ino.bin to the SD card and install it from M5 Launcher."

esp32\:linetest\:cardputer:
	arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer --output-dir $(CARDPUTER_BUILD)-linetest esp32/esp32_tilink_linetest
	@echo "Copy $(CARDPUTER_BUILD)-linetest/esp32_tilink_linetest.ino.bin to the SD card and install it from M5 Launcher."

shell:
	@test -n "$(PORT)" || { echo "No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass PORT=..."; exit 1; }
	python3 tools/shell.py --port $(PORT) $(if $(MIRROR),--mirror) $(CMD)

.PHONY: shell calc\:term\:build calc\:term\:send esp32\:term\:flash esp32\:term\:cardputer esp32\:linetest\:cardputer
