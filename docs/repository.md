# The repository

## Layout

```
esp32/                               Arduino sketches (one folder per sketch)
  esp32_tilink_term/                 carries calc/term's bytes to Serial or SSH
  esp32_tilink_linetest/             wiring check: prints tip/ring levels without driving them
calc/                                C programs for the calculator (TIGCC / GCC4TI)
  hello/                             hello world; the build and send pattern
  term/                              a 60x21 terminal on the link port, with a menu
    term.c                             screen, VT220 subset, keyboard
    font.txt, mkfont.py                the 3x5 font, drawn as text; its converter and character table
    help.txt, mkhelp.py                the help pages shown by F6, as text, and their converter
shell/                               what runs on the computer for a session
  shell.py                           joins a shell on the computer to the ESP32's port; saves screenshots
  tmux_calc.conf                     tmux settings for the calculator's screen
  screenrc_vt100                     screen settings for the same
link/                                the link layer between calc/term and esp32_tilink_term
  src/tlink.h, src/tlink.c           frames, CRC, acknowledgements and resends, in plain C for both
  test/test_link.c                   runs it over a simulated line that garbles and slips (make link:test)
  library.properties                 lets the sketch take it as an Arduino library
Makefile                             shortcuts for the terminal
docs/
  getting-started.md                 what to install, and the quick start
  terminal.md                        the terminal: running it, keys, how the pieces talk, findings
  tmux-and-screen.md                 several windows on the calculator's screen
  hardware.md                        parts, wiring, wiring check, building and flashing
  manual-tests.md                    tests run by hand on the hardware
  repository.md                      this file
  pics/                              photographs and screenshots
README.md                            what the project is, with pictures
```

## Removed experiments

Earlier experiments were removed. They are in the git history, last present
in commit `d4abde0`:

- `esp32/esp32_tilink_pingpong/` with `calculator/pingpong.c`: a raw-byte link
  using a custom frame format instead of TI's packets.
  - It needs a C program on the calculator (GCC4TI).
  - It was never tested end to end.
  - The terminal took this route again, first with raw bytes and no
    frames, then with frames of its own (`link/`).
- `arduino/ti_link_arduino/` with `calculator/tilink.c`: the first experiment,
  for an Arduino Uno/Nano.

---

[Project overview](../README.md) · [Getting started](getting-started.md) · [The terminal](terminal.md) · [tmux and screen](tmux-and-screen.md) · [Hardware](hardware.md) · [Repository](repository.md)
