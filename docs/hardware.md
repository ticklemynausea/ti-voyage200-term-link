# Hardware, wiring and flashing

## Hardware

- TI Voyage 200 (the TI-89 / TI-92 Plus family should behave the same way);
  two of them for the calculator-to-calculator sniffer
- One ESP32, either of:
  - M5Stack Stamp S3 (ESP32-S3) on a breadboard: [wiring](#wiring-stamp-s3).
    Every sketch here runs on it.
  - M5Stack Cardputer, which has a Stamp S3 inside plus a battery, a screen
    and a keyboard: [wiring](#m5stack-cardputer). The terminal and the
    wiring check are built for it.

  Other ESP32 boards should work with different pins, except that the c2c
  sniffer uses ESP32-S3 features.
- 2.5 mm stereo (TRS) pigtail cables: one per calculator. The plug's moulded
  collar may need shaving down to seat fully in the Voyage 200's recessed port.
- Two 220 ohm resistors

## Wiring (Stamp S3)

The Voyage 200's link lines are open-collector with pull-ups, and on this unit
they idle at **3.3 V**, so the Stamp S3 is wired directly with no level shifter.

| Pigtail wire | Plug part | Stamp S3 |
|---|---|---|
| Red | Tip | 220 ohm -> G1 |
| White | Ring | 220 ohm -> G5 |
| Black | Sleeve | GND |

For the sniffer, the second calculator's pigtail joins the same three rows;
see [its README](../calc-to-calc-protocol-reversed/README.md#wiring).

Measure your own calculator first (tip and ring against sleeve, calculator on,
at the Home screen). If the lines idle at about 5 V, use a bidirectional level
shifter.

Things learned the hard way:

- **A solid ground connection is essential.** With a loose ground the lines
  pick up hum from the laptop's power adapter and toggle every 10 ms.
- **Power order matters.** Plug in the ESP32's USB before connecting the
  calculator, and unplug the calculator first. Otherwise the calculator's
  pull-ups can half-power the unpowered ESP32 through its pin protection
  diodes, and it then fails to appear on USB.
- Stranded pigtail wires make poor breadboard contacts; tin them or solder them
  to header pins.

## M5Stack Cardputer

The terminal and wiring-check sketches also build for the Cardputer, whose
battery makes the setup portable. They pick their pins from the board they
are built for. The calculator connects to the Grove port:

| Grove pin | Grove wire | Through | Pigtail wire | Plug part |
|---|---|---|---|---|
| G2 | yellow | 220 ohm | red | Tip |
| G1 | white | 220 ohm | white | Ring |
| GND | black | direct | black | Sleeve |
| 5 V | red | not connected | | |

The wiring check below tells tip from ring: on `SendCalc` with nothing
answering, the calculator holds the tip low for about two seconds.

The Grove cable's red wire carries 5 V: cut it or insulate its end. G1 and
G2 are 3.3 V pins of the Stamp S3 inside, the same as on the bare Stamp, so
a calculator whose lines idle at 3.3 V needs no level shifter here either.

`make esp32_cardputer:term:build` builds
`build/cardputer-term/esp32_tilink_term.ino.bin` and flashes nothing: copy the
file to the SD card and install it from M5 Launcher. It needs one more
library, used only by this build: `arduino-cli lib install M5Cardputer`.

`make esp32_cardputer:term:flash` builds it and flashes it over USB
instead, which replaces M5 Launcher on the Cardputer (M5Burner puts it
back). Have only one of the two boards plugged in: both targets take the
first ESP32 port they find.

If the upload cannot connect, hold G0 while pressing reset and try again.

The Cardputer's screen shows what the bridge is doing, on three pages. Any
key on its keyboard goes to the next page; `1`, `2` and `3` jump to one.
Nothing typed there is sent to the shell.

| Page | Shows |
|---|---|
| Status | the mode (menu, Serial or SSH), the SSH target or whether the computer is attached, the Wi-Fi network with signal and address, the calculator's presence, bytes per second each way |
| Link | byte counts and rates each way, dropped bytes, resends, link errors, time since the last byte, uptime |
| Log | the last events with the time since power-up: mode changes, Wi-Fi and SSH messages, the calculator or computer coming and going |

Every page has the battery level in its top bar.

Tried on a Cardputer: the pages show on its screen and the calculator
connects through the Grove cable.

## Wiring check

`esp32/esp32_tilink_linetest/` prints the tip and ring levels whenever they
change, plus a periodic heartbeat. It never drives the lines. Use it before
any experiment to check the wiring and the ground connection.
`make esp32_stamps3:linetest:flash` or `make esp32_cardputer:linetest:flash`
builds and flashes it.

## Building and flashing

With `arduino-cli` and the ESP32 board package installed, from the repo root
(replace the sketch folder with the one you want):

```sh
arduino-cli compile --upload -p /dev/cu.usbmodemXXXX \
  --fqbn esp32:esp32:m5stack_stamp_s3 esp32/esp32_tilink_cbl
arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=115200
```

Replace the port with your own (`arduino-cli board list`). The sketches also
open in the Arduino IDE. The terminal sketch also needs the SSH library:
`arduino-cli lib install LibSSH-ESP32`. For it there is a shortcut,
`make esp32_stamps3:term:flash`, which finds the port by itself.
