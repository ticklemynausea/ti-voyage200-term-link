# Getting started

What to install, and the shortest way to a shell on the calculator.

## Prerequisites

Developed on macOS. The versions are the ones this was built with, not
minimums.

| Tool | For | Version used |
|---|---|---|
| [GCC4TI](https://github.com/debrouxl/gcc4ti) (`tigcc`) | building the calculator programs | tigcc 1.3.4, TIGCC 0.96 Beta 11 |
| [TiLP](http://lpg.ticalc.org/prj_tilp/) (`tilp`) | sending programs to the calculator | 1.18 |
| A TI link cable for the computer (SilverLink USB) | the same; `tilp` talks through it | |
| [arduino-cli](https://arduino.github.io/arduino-cli/) | compiling and flashing the ESP32 sketches (the Arduino IDE works too) | 1.5.1 |
| ESP32 board package (`esp32:esp32`) | the board definitions | 3.2.0 |
| [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) library | SSH in the terminal sketch | 5.10.0 |
| [M5Cardputer](https://github.com/m5stack/M5Cardputer) library | the status screen, in the Cardputer build only | 1.1.1 |
| Python 3 | the font converter and `shell/shell.py`; standard library only | 3.9 |
| `make` | the shortcuts below | |

The two Arduino pieces are installed once:

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install LibSSH-ESP32
```

`arduino-cli` and the Arduino IDE share one library folder, so a library
installed with either is seen by both. In the IDE the same two are under
Boards Manager ("esp32" by Espressif) and Library Manager ("LibSSH-ESP32").

`tigcc`, `tilp`, `arduino-cli` and `python3` have to be on the PATH. The
hardware is listed in [docs/hardware.md](hardware.md).

## Quick start: the terminal

The ESP32 side comes in two forms. Both run the same sketch, which picks
its pins from the board it is built for:

| | Stamp S3 on a breadboard | Cardputer |
|---|---|---|
| Calculator connects to | G1 and G5, on jumper wires | the Grove port (G2 and G1) |
| Power | USB | its battery, or USB |
| Screen | none | status pages: mode, Wi-Fi, link, log |
| Extra library | none | M5Cardputer |
| Build and flash | `make esp32_stamps3:term:flash` | `make esp32_cardputer:term:flash`, or `make esp32_cardputer:term:build` for a file that M5 Launcher installs |
| Wiring | [hardware.md](hardware.md#wiring-stamp-s3) | [hardware.md](hardware.md#m5stack-cardputer) |

Wire one of them to the link port. With the prerequisites above in place:

```sh
make esp32_stamps3:term:flash   # compile and flash the ESP32 (or esp32_cardputer:term:flash)
make calc:term:send             # build term.v2z and send it (SilverLink cable)
make shell                      # optional: a shell on this computer for the Serial entry
```

Run `term()` on the calculator. F3 opens the settings: the Wi-Fi network,
the SSH host, and a key for logging in without a password. F2 connects over
SSH, F1 uses the shell from `make shell`. The keys, the settings and what
was learned on the way are in [terminal.md](terminal.md).

For several windows on the calculator's screen, see
[tmux and screen](tmux-and-screen.md).

---

[Project overview](../README.md) · [Getting started](getting-started.md) · [The terminal](terminal.md) · [tmux and screen](tmux-and-screen.md) · [Hardware](hardware.md) · [Link partner](link-partner.md) · [Repository](repository.md)
