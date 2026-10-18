# Shortcuts for the terminal experiment. Run from the repo root:
#
#   make calc:term:build     build calc/term/build/term.v2z
#   make calc:term:send      build it and send it to the calculator (SilverLink)
#   make shell               run a shell here with the calculator as its terminal
#                            (another command: make shell CMD=top)
#                            (also show it here and type here: make shell MIRROR=1)
#   make screenshots         attach to the ESP32 and run nothing: only save the
#                            screens the calculator sends with 2nd + APPS, in
#                            screenshots/ (make shell saves them too)
#   make link:test           run the link layer (link/) on this computer, over
#                            a simulated line that garbles, drops and slips
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
# build compiles into build/<board>-<sketch>/ and flashes nothing; the
# object files stay in its cache/ folder, one per board, so a second build
# only compiles what changed (rm -rf build for a clean one). On the
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

# The commit the terminal sketch is built from, with a + if there were
# changes not yet committed, and the time: $(call version_h,board) writes them
# to a version.h in the build folder, which compile puts on the include path,
# for the Cardputer's build page. The date is the Makefile's because the
# compiler's own __DATE__ only changes when the file using it is rebuilt.
VERSION := $(shell git describe --always --dirty=+ --exclude='*' 2>/dev/null || echo unknown)
version_h = @mkdir -p build/$(1)-term && \
	printf '\#define VERSION "%s"\n\#define BUILT "%s"\n' '$(VERSION)' "$$(date '+%b %e %Y %H:%M:%S')" \
	> build/$(1)-term/version.h

# $(call compile,fqbn,board,sketch) and the same with an upload. link/ is the
# link layer the terminal sketch shares with the calculator's program.
compile = arduino-cli compile --fqbn $(1) --output-dir build/$(2)-$(3) \
	--build-path $(CURDIR)/build/$(2)-$(3)/cache --library $(CURDIR)/link \
	--build-property compiler.cpp.extra_flags=-I$(CURDIR)/build/$(2)-$(3) esp32/esp32_tilink_$(3)
upload = $(call compile,$(1),$(2),$(3)) --upload -p $(PORT)
need_port = @test -n "$(PORT)" || { echo "No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass PORT=..."; exit 1; }
sd_hint = @echo "For M5 Launcher: copy build/cardputer-$(1)/esp32_tilink_$(1).ino.bin to the SD card."

link\:test:
	@mkdir -p build
	$(CC) -std=c99 -O2 -Wall -Wextra -Ilink/src link/src/tlink.c link/test/test_link.c -o build/test_link
	build/test_link

calc\:term\:build:
	$(MAKE) -C calc/term

calc\:term\:send:
	$(MAKE) -C calc/term send

esp32_stamps3\:term\:build:
	$(call version_h,stamps3)
	$(call compile,$(STAMPS3_FQBN),stamps3,term)

esp32_stamps3\:term\:flash:
	$(call version_h,stamps3)
	$(need_port)
	$(call upload,$(STAMPS3_FQBN),stamps3,term)

esp32_stamps3\:linetest\:build:
	$(call compile,$(STAMPS3_FQBN),stamps3,linetest)

esp32_stamps3\:linetest\:flash:
	$(need_port)
	$(call upload,$(STAMPS3_FQBN),stamps3,linetest)

esp32_cardputer\:term\:build:
	$(call version_h,cardputer)
	$(call compile,$(CARDPUTER_TERM_FQBN),cardputer,term)
	$(call sd_hint,term)

esp32_cardputer\:term\:flash:
	$(call version_h,cardputer)
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
	python3 shell/shell.py --port $(PORT) $(if $(MIRROR),--mirror) $(CMD)

screenshots:
	$(need_port)
	python3 shell/shell.py --port $(PORT) --no-shell

.PHONY: shell screenshots link\:test calc\:term\:build calc\:term\:send \
	esp32_stamps3\:term\:build esp32_stamps3\:term\:flash \
	esp32_stamps3\:linetest\:build esp32_stamps3\:linetest\:flash \
	esp32_cardputer\:term\:build esp32_cardputer\:term\:flash \
	esp32_cardputer\:linetest\:build esp32_cardputer\:linetest\:flash
