# Shortcuts for the terminal experiment. Run from the repo root:
#
#   make calc:term:build     build calc/term/build/term.v2z
#   make calc:term:send      build it and send it to the calculator (SilverLink)
#   make esp32:term:flash    compile esp32_tilink_term and flash the ESP32
#   make shell               run a shell here with the calculator as its terminal
#                            (another command: make shell CMD=top)
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

shell:
	@test -n "$(PORT)" || { echo "No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass PORT=..."; exit 1; }
	python3 tools/shell.py --port $(PORT) $(CMD)

.PHONY: shell calc\:term\:build calc\:term\:send esp32\:term\:flash
