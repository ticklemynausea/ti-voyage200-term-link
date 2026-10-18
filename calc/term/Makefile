# Builds term.v2z for the Voyage 200. Needs TIGCC / GCC4TI on the PATH.

CC    = tigcc
TILP  ?= tilp
CABLE ?= SilverLink

BUILD = build
OUT   = $(BUILD)/term.v2z
# The link layer, shared with the ESP32's sketch.
LINK  = ../../link/src

# The commit the program is built from, with a + if there were changes not
# yet committed.
VERSION := $(shell git describe --always --dirty=+ --exclude='*' 2>/dev/null || echo unknown)

$(OUT): term.c $(LINK)/tlink.c $(LINK)/tlink.h $(BUILD)/font.h $(BUILD)/help.h $(BUILD)/version.h
	$(CC) -Os -Wall -W -I$(BUILD) -I$(LINK) term.c $(LINK)/tlink.c -o $(BUILD)/term

# Rewritten only when the commit changes, so that the program is rebuilt then.
$(BUILD)/version.h: FORCE
	mkdir -p $(BUILD)
	echo '#define VERSION "$(VERSION)"' > $@.new
	if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

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

FORCE:

.PHONY: send clean FORCE
