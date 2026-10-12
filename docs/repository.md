# The repository

## Layout

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
    help.txt, mkhelp.py                the help pages shown by F5, as text, and their converter
tools/
  shell.py                           joins a shell on the computer to the ESP32's port; saves screenshots
shell/
  tmux_calc.conf                     tmux settings for the calculator's screen
  screenrc_vt100                     screen settings for the same
Makefile                             shortcuts for the terminal
calc-to-calc-protocol-reversed/      the calculator-to-calculator protocol: documentation and data
  README.md                            sniffer wiring, flashing, running a capture session
  TESTS.md                             the test sequence (T00-T20)
  PROTOCOL.md                          the reversed calculator-to-calculator protocol
  captures/                            raw logs of every session, named by date and time
docs/
  getting-started.md                 what to install, and the quick start
  terminal.md                        the terminal: running it, keys, how the pieces talk, findings
  tmux-and-screen.md                 several windows on the calculator's screen
  hardware.md                        parts, wiring, wiring check, building and flashing
  link-partner.md                    the link-partner sketch and its protocol findings
  repository.md                      this file
  pics/                              photographs and screenshots
README.md                            what the project is, with pictures
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

---

[Project overview](../README.md) · [Getting started](getting-started.md) · [The terminal](terminal.md) · [tmux and screen](tmux-and-screen.md) · [Hardware](hardware.md) · [Link partner](link-partner.md) · [Repository](repository.md)
