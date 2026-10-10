# ti-voyager-arduino-link

A **TI Voyage 200** graphing calculator as a terminal to real computers, and
the reverse engineering of its link port that led there. An ESP32 (M5Stack
Stamp S3, Arduino framework) sits on the calculator's 2.5 mm link port and
does the networking; the calculator draws the text and reads the keys.

The headline: a 9 KB C program turns the calculator into a 60x21 VT220
terminal. From its menu you open a shell on the computer the ESP32 is plugged
into, or an SSH session to any host over Wi-Fi. `vim`, `less`, `irssi` and
`links` run on it.

```
Voyage 200             ESP32
calc/term  <- link ->  esp32_tilink_term  <- USB serial ->  a shell on the computer
                                          <- Wi-Fi, SSH ->  a shell on another host
```

This is a hobby project and a record of experiments, not a finished product.
Treat the protocol notes as findings from these calculators, not as a
specification.

## What is here

| Part | What it does | Read |
|---|---|---|
| **Calculator as a terminal** | The terminal program, the ESP32 bridge with SSH and stored Wi-Fi settings, and the script that joins a local shell to it. | [docs/terminal.md](docs/terminal.md) |
| **ESP32 as a link partner** | The ESP32 speaks TI's packet protocol, posing as a CBL data logger or another calculator, so TI-BASIC's `Send`, `Get` and `SendCalc` talk to it. | [docs/link-partner.md](docs/link-partner.md) |
| **Calculator-to-calculator protocol** | The ESP32 listens passively between two Voyage 200s; twenty tests document what they say to each other. | [PROTOCOL.md](calc-to-calc-protocol-reversed/PROTOCOL.md), [its README](calc-to-calc-protocol-reversed/README.md) |
| **Hardware** | Parts, wiring, a wiring check sketch, building and flashing. | [docs/hardware.md](docs/hardware.md) |

## Quick start: the terminal

Wire the ESP32 to the link port as in [docs/hardware.md](docs/hardware.md).
With `tigcc`, `python3`, `tilp` and `arduino-cli` on the PATH, and
`arduino-cli lib install LibSSH-ESP32` done once:

```sh
make esp32:term:flash    # compile and flash the ESP32
make calc:term:send      # build term.v2z and send it (SilverLink cable)
make shell               # optional: a shell on this computer for the Serial entry
```

Run `term()` on the calculator. F3 sets the Wi-Fi network and the SSH host,
F2 connects over SSH, F1 uses the shell from `make shell`. The keys, the
settings and what was learned on the way are in
[docs/terminal.md](docs/terminal.md).

## Repository layout

```
esp32/                               Arduino sketches (one folder per sketch)
  esp32_tilink_cbl/                  the ESP32 as a link partner
    esp32_tilink_cbl.ino               packet layer, decoders, Serial commands
    TiLink.h, TiLink.cpp               bit-level link driver with error diagnostics
  esp32_tilink_c2c/                  passive sniffer between two calculators
    esp32_tilink_c2c.ino               sampler, packet printer, line history, Serial commands
    BitDecoder.h                       passive bit decoder (plain C++, testable on a PC)
  esp32_tilink_term/                 carries calc/term's bytes to Serial or SSH
  esp32_tilink_linetest/             wiring check: prints tip/ring levels without driving them
calc/                                C programs for the calculator (TIGCC / GCC4TI)
  hello/                             hello world; the build and send pattern
  term/                              a 60x21 terminal on the link port, with a menu
    term.c                             screen, VT220 subset, keyboard
    font.txt, mkfont.py                the 3x5 font, drawn as text; its converter and character table
tools/
  shell.py                           joins a shell on the computer to the ESP32's port
Makefile                             shortcuts for the terminal
calc-to-calc-protocol-reversed/      the calculator-to-calculator protocol: documentation and data
  README.md                            sniffer wiring, flashing, running a capture session
  TESTS.md                             the test sequence (T00-T20)
  PROTOCOL.md                          the reversed calculator-to-calculator protocol
  captures/                            raw logs of every session, named by date and time
docs/
  terminal.md                        the terminal: running it, keys, how the pieces talk, findings
  link-partner.md                    the link-partner sketch and its protocol findings
  hardware.md                        parts, wiring, wiring check, building and flashing
README.md                            this file
```

## Removed experiments

Earlier experiments were removed once the CBL sketch covered their ground.
They are in the git history, last present in commit `d4abde0`:

- `esp32/esp32_tilink_sniffer/`: an early receive-only sniffer. It took part
  in the handshake, unlike the passive c2c sniffer.
- `esp32/esp32_tilink_pingpong/` with `calculator/pingpong.c`: a raw-byte link
  using a custom frame format instead of TI's packets.
  - It needs a C program on the calculator (GCC4TI).
  - It was never tested end to end.
  - The terminal took this route again, with raw bytes and no frames.
- `arduino/ti_link_arduino/` with `calculator/tilink.c`: the first experiment,
  for an Arduino Uno/Nano.

## Credits

- [ArTICL](https://github.com/KermMartian/ArTICL) by Christopher Mitchell, the
  Arduino TI linking library, for the pin conventions and approach.
- The [TIGCC documentation](http://tigcc.ticalc.org/doc/link.html) for the
  calculator's link functions and variable types.
- [TI calculator character sets](https://en.wikipedia.org/wiki/TI_calculator_character_sets)
  on Wikipedia for the TI-89/92/Voyage 200 character table.
- [GCC4TI](https://github.com/debrouxl/gcc4ti) / TIGCC, the C compiler for the
  calculator, and [TiLP](http://lpg.ticalc.org/prj_tilp/) for sending programs
  to it.
- [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) by Ewan Parker, the
  libssh port the SSH sessions run on.
