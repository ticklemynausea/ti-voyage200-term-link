# Builds term.v2z for the Voyage 200. Needs TIGCC / GCC4TI on the PATH.

CC    = tigcc
TILP  ?= tilp
CABLE ?= SilverLink

BUILD = build
OUT   = $(BUILD)/term.v2z

$(OUT): term.c $(BUILD)/font.h $(BUILD)/help.h
	$(CC) -Os -Wall -W -I$(BUILD) term.c -o $(BUILD)/term

$(BUILD)/font.h: font.txt mkfont.py
	mkdir -p $(BUILD)
	python3 mkfont.py font.txt $@

$(BUILD)/help.h: help.txt mkhelp.py
	mkdir -p $(BUILD)
	python3 mkhelp.py help.txt $@

# Sends the program to the calculator (idle at the Home screen).
send: $(OUT)
	$(TILP) --no-gui --silent --calc=v200 --cable=$(CABLE) $(OUT)

clean:
	rm -rf $(BUILD)

.PHONY: send clean
