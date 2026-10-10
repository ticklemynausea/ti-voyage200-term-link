# Builds hello.v2z for the Voyage 200. Needs TIGCC / GCC4TI on the PATH.

CC    = tigcc
TILP  ?= tilp
CABLE ?= SilverLink

BUILD = build
OUT   = $(BUILD)/hello.v2z

$(OUT): hello.c
	mkdir -p $(BUILD)
	$(CC) -Os -Wall -W hello.c -o $(BUILD)/hello

# Sends the program to the calculator (idle at the Home screen).
send: $(OUT)
	$(TILP) --no-gui --silent --calc=v200 --cable=$(CABLE) $(OUT)

clean:
	rm -rf $(BUILD)

.PHONY: send clean
