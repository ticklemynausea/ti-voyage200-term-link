# The calculator as a terminal

The calculator is a dumb terminal: it draws characters, reads keys and moves
raw bytes over the link port. TI's packet protocol is not involved. The ESP32
carries the bytes to a shell, either on the computer it is plugged into or on
any host it can reach by SSH over Wi-Fi.

```
Voyage 200             ESP32
calc/term  <- link ->  esp32_tilink_term  <- USB serial ->  tools/shell.py -> shell
                                          <- Wi-Fi, SSH ->  a shell on another host
```

- **`calc/term/`** is the calculator program (about 9 KB), built with
  TIGCC / GCC4TI. It opens on a menu that picks where the session goes. It
  draws its own fixed-width 3x5 font, because the calculator's small font is
  proportional. The glyphs are drawn as text in `font.txt`. It handles the
  VT220 codes that shells and full-screen programs use: cursor movement,
  erasing, reverse video, scroll regions, inserting and deleting lines and
  characters. Text arrives as UTF-8; see [Characters](#characters) for what
  it can draw.
- **`esp32/esp32_tilink_term/`** carries the bytes to the backend the menu
  chose. `Ssh.h` / `Ssh.cpp` hold the SSH side, on the LibSSH-ESP32 library.
  On the serial side, its monitor mode shows what the calculator sends, and
  `/burst`, `/stats` and `/raw` test the link.
- **`tools/shell.py`** runs a command on the computer in a 60x21 `vt220`
  pseudo-terminal joined to the ESP32's port, for the menu's Serial entry.

## Running it

Needs `tigcc`, `python3`, `tilp` and `arduino-cli` on the PATH, and the SSH
library once: `arduino-cli lib install LibSSH-ESP32`. From the repo root:

```sh
make calc:term:send      # build term.v2z and send it (SilverLink cable)
make esp32:term:flash    # compile and flash the bridge sketch
make shell               # for the Serial entry: the login shell; or: make shell CMD=top
```

Move the calculator to the ESP32's pigtail and run `term()`. The menu:

| Key | Entry |
|---|---|
| F1 | Serial: the shell that `make shell` runs on the computer |
| F2 | SSH: a shell on the host in the settings, over Wi-Fi |
| F3 | Settings: the Wi-Fi network, the SSH host and the user |
| F4 | Local echo: nothing leaves the calculator |
| F5 | Test pattern, with the time a full screen takes |
| F6 | Key codes: shows each key's code |
| ESC | Exit |

The menu's bottom row shows the Wi-Fi status, asked from the ESP32 every
three seconds: the network, signal strength and address, or that it is not
connected or not set up. The ESP32 joins the stored network when it powers
up.

`make shell` can be started before or after choosing Serial: its output
waits until the calculator is in that session. Close the Serial Monitor
first: only one program can hold the ESP32's port.

For SSH, choose Settings first. The ESP32 lists the Wi-Fi networks it sees
and asks for the network (by number or name), its password, the SSH host
(`host` or `host:port`) and the user. They are stored in the ESP32's flash
(the Preferences library), so they survive power cycles and reflashing, and
the Wi-Fi password is readable there by anyone holding the board. The SSH
password is asked for at every connection and never stored. `/settings` in
the Serial Monitor shows what is stored, without the password.

The host's key is remembered at the first connection and checked
afterwards; if it changes, the connection is refused until `/forgethost` is
typed in the Serial Monitor.

## How the pieces talk

Everything on the link is raw bytes. Three small conventions sit on top, all
built on the byte `0x1E`, which neither a keyboard nor a shell normally
produces.

**Calculator to ESP32.** `0x1E` and a letter is a command; a real `0x1E` is
sent twice.

| Bytes | Meaning |
|---|---|
| `1E 's'` | a Serial session starts |
| `1E 'h'` | an SSH session starts |
| `1E 'c'` | the Settings dialog starts |
| `1E 'x'` | the session is over |
| `1E 'w'` | what is the Wi-Fi status? |

**ESP32 to calculator.** Plain terminal output, except the answer to the
status question: `1E 'w'`, a line of text, a newline.

**Computer to ESP32** (`tools/shell.py`). Three `0x1E` bytes, a word and a
newline switch the sketch's serial mode: `raw` or `monitor`. In raw mode
entered that way the script keeps at most 128 bytes outstanding, and the
sketch answers `1E 'a'` for every 32 bytes it has passed on.

## Keys in a session

| Key | Does |
|---|---|
| ◆ + letter | Ctrl + letter (the keyboard has no Ctrl key); ◆ + I is Tab |
| ◆ + up / down | Page Up / Page Down |
| 2nd + up / down | Home / End (2nd + left / right too) |
| 2nd + plus (CHAR) | a strip of symbols on the bottom row; a letter picks one |
| F1 to F8 | the function keys |
| 2nd + ESC (QUIT) | back to the menu |

Home and End work in the shell and in vim, but not in `less`: use `g` and
`G` there. The menu's first line shows when the program was built, which tells
whether the calculator has the latest copy.

The divide key types `/`. The (−) negation key is a TI-only character and is
dropped; use the subtraction key for `-`.

## Characters

The shell's text is read as UTF-8. With 3x5 pixels per letter there is no room
for accents, so the terminal draws what it can and stands in for the rest:

| Characters | Drawn as |
|---|---|
| ASCII | its own glyph, from `font.txt` |
| Box drawing (U+2500 to U+257F), as `tree` and `mc` use | real lines that join from cell to cell; heavy, double and dashed lines all come out as plain ones |
| Block elements (U+2580 to U+259F), as progress bars use | halves, eighths, quadrants and shades, to the nearest pixel |
| Latin letters with accents (U+00A0 to U+017F) | the plain letter: `é` as `e`, `ç` as `c`, `ß` as `s` |
| Curly quotes, dashes, arrows, bullets, ticks and the like | the nearest ASCII character: `→` as `>`, `✓` as `v`, `…` as `.` |
| Braille patterns, as spinners use | `.` |
| Accents sent as separate marks, zero-width marks | nothing |
| East Asian characters and emoji | two solid blocks, so that columns stay aligned |
| Anything else | one solid block |

The VT100 line-drawing set (`ESC ( 0`), which older programs use for boxes,
draws with the same line glyphs.

`python3 calc/term/mkfont.py calc/term/font.txt --list` prints every covered
character beyond ASCII with what it is drawn as: 495 of them at the moment.
The stand-ins are listed at the top of `mkfont.py`; adding one is a matter of
adding the character to the right line. The menu's test pattern (F5) shows a
sample.

This is about the screen only. The keyboard still types ASCII.

## Findings

- **Raw bytes work through AMS's own link queues.** After `OSLinkOpen`, the
  OS keeps moving bytes between the port and its queues in the background,
  and `OSWriteLinkBlock` / `OSReadLinkBlock` only touch the queues.
- **`kbhit` and `ngetchx` must be avoided.** They also poll the link and
  treat incoming bytes as TI packets. The program reads the keyboard queue
  directly (`OSdequeue` on `kbd_queue()`).
- **◆ + key arrives as 8192 + the key's code** on the Voyage 200 (◆ + up is
  8530), as TIGCC's key table says. On the TI-89 the number is 16384.
- **2nd + key arrives as 4096 + the key's code** when the combination is a
  function rather than a character: 2nd + plus is 4139. The CHAR menu
  belongs to AMS, hence the program's own symbol strip.
- **The link held up in a burst test:** 7083 bytes to the calculator with
  none dropped and no retries, and 127 bytes back from typing meanwhile.
- **Serial output has to be paced.** The link carries about 3 KB/s, far
  less than USB, and the ESP32 drops serial bytes it has no room for.
  Programs that repaint the whole screen then lost part of their output.
  `tools/shell.py` keeps at most 128 bytes outstanding and the sketch
  acknowledges every 32 it has passed on.
- **Collisions can lose a keystroke.** The link is half duplex. That test
  logged one link error, most likely a key pressed while the ESP32 was
  sending. If both ends start a byte at the same instant on different lines,
  each can take the other's bit for an acknowledgement. Not fixed yet.
- **The terminal type matters for keys.** Under `vt100`, programs have no
  code for Page Up and Page Down, and `less` only accepts the arrow keys'
  "application" codes, which it switches on at start. The terminal now
  follows that switch and reports itself as `vt220`, which has page keys.
- **An unplugged pigtail floats** and reads as a stream of garbage bytes, so
  this sketch turns on the ESP32's internal pull-ups.
- **libssh needs a large stack.** The sketch gives the Arduino loop task
  48 KB instead of the default 8 KB. With SSH the sketch takes 78% of the
  default 1.3 MB program partition.

For the wiring and the hardware, see [hardware.md](hardware.md).
