# The calculator as a terminal

The calculator is a dumb terminal: it draws characters, reads keys and moves
raw bytes over the link port. TI's packet protocol is not involved. The ESP32
carries the bytes to a shell, either on the computer it is plugged into or on
any host it can reach by SSH over Wi-Fi.

![The Voyage 200 runs calc/term and connects over its link port to an ESP32 running esp32_tilink_term, which reaches a shell on the computer over USB serial and shell/shell.py, or a shell on another host over Wi-Fi and SSH](pics/diagram.svg)

- **`calc/term/`** is the calculator program (about 13 KB), built with
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
- **`shell/shell.py`** runs a command on the computer in a 60x21 `vt220`
  pseudo-terminal joined to the ESP32's port, for the menu's Serial entry.

## Running it

Needs `tigcc`, `python3`, `tilp` and `arduino-cli` on the PATH, and the SSH
library once: `arduino-cli lib install LibSSH-ESP32`. From the repo root:

```sh
make calc:term:send             # build term.v2z and send it (SilverLink cable)
make esp32_stamps3:term:flash   # compile and flash the bridge sketch (or esp32_cardputer:term:flash)
make shell                      # for the Serial entry: the login shell; or: make shell CMD=top
```

Move the calculator to the ESP32's pigtail and run `term()`. The menu:

| Key | Entry |
|---|---|
| F1 | Serial: the shell that `make shell` runs on the computer |
| F2 | SSH: a shell on one of the hosts in the settings, over Wi-Fi |
| F3 | Settings: a menu of its own, below |
| F4 | Debug: a menu of its own, below |
| F5 | Help: the keys and the menu, from `calc/term/help.txt` |
| ESC | Exit |

Settings:

| Key | Entry |
|---|---|
| F1 | Wi-Fi: the network and its password |
| F2 | SSH hosts: sets, changes or removes one of the hosts |
| F3 | SSH key: makes the ESP32's own key, replaces it or removes it |
| F4 | Copy the key: puts the public key on one of the hosts, after asking |

The ESP32 keeps up to eight SSH hosts, one for each of the keys F1 to F8.
SSH, SSH hosts and Copy the key first show them as a menu, where the F key
picks the host:

```
SSH

   F1   mario@server.example
   F2   pi@192.168.1.20:2222
   F3   -
   ...
   F8   -
   ESC  Back
```

A key with no host does nothing, except under SSH hosts, where it sets
one. The list is asked from the ESP32 each time the menu is shown.

Debug:

| Key | Entry |
|---|---|
| F1 | Local echo: nothing leaves the calculator |
| F2 | Test pattern, with the time a full screen takes |
| F3 | Key codes: shows each key's code |

ESC in any of them goes one menu back.

The menu's bottom row shows the Wi-Fi status, asked from the ESP32 every
three seconds: the network, signal strength and address, or that it is not
connected or not set up. The ESP32 joins the stored network when it powers
up.

`make shell` can be started before or after choosing Serial: its output
waits until the calculator is in that session. Close the Serial Monitor
first: only one program can hold the ESP32's port.

`make shell MIRROR=1` shares the session with the window it runs in: the
output is copied there, and keys typed there go to the same shell, next to
the calculator's. Ctrl-C then goes to the shell like any other key, so
Ctrl-] is what stops it. Without `MIRROR`, the window shows nothing, sends
nothing, and Ctrl-C stops it.

For SSH, go through Settings first. Wi-Fi lists the networks the ESP32
sees and asks for the network (by number or name) and its password. SSH
hosts, then an F key, asks for that host (`host` or `host:port`) and its
user; on a host that is already set it first offers to change or remove
it. They are stored in the ESP32's flash (the Preferences library), so they survive
power cycles and reflashing, and the Wi-Fi password is readable there by
anyone holding the board. `/settings` in the Serial Monitor shows what is
stored, without the password. An ESP32 that had a single host from before
keeps it, as the first one.

**Logging in.** Without a key, the SSH password is asked for at every
connection and never stored. To skip it, the ESP32 can have a key of its
own:

1. Settings, F3 makes an ed25519 key pair on the ESP32 and shows its
   fingerprint. This talks to no host.
2. Settings, F4, then the host's F key, copies the public half to that
   host. It shows the
   fingerprint and the `user@host` it is about to change, and goes ahead
   only when `yes` is typed. It then logs in with the password once and
   adds a line ending in `voyage200` to `~/.ssh/authorized_keys` there.

From then on SSH to that host logs in with the key. There is one key for
all the hosts, and step 2 is done once for each host that should take it.
If a host refuses the key, the password is asked for as before. F3 also
replaces or removes the key; the hosts keep their line until it is deleted
there.

The private key never leaves the ESP32, and it is stored there unencrypted:
whoever holds the board can log in to the hosts that have its public half.
`/pubkey` in the Serial Monitor prints the public key line, to add to a
host by hand instead.

Each host's key is remembered at the first connection and checked
afterwards; if it changes, the connection is refused until `/forgethost 3`
(for the third host; without a number, all of them) is typed in the Serial
Monitor. Changing a host's name or port, or removing it, forgets its key
too.

## How the pieces talk

Everything on the link is raw bytes. Three small conventions sit on top, all
built on the byte `0x1E`, which neither a keyboard nor a shell normally
produces.

**Calculator to ESP32.** `0x1E` and a letter is a command; a real `0x1E` is
sent twice.

| Bytes | Meaning |
|---|---|
| `1E 's'` | a Serial session starts |
| `1E '1'` to `1E '8'` | selects one of the SSH hosts for the `h`, `k` or `i` that follows |
| `1E 'h'` | an SSH session with the selected host starts |
| `1E 'n'` | the Wi-Fi settings dialog starts |
| `1E 'k'` | the dialog for the selected host and its user starts |
| `1E 'g'` | the SSH key dialog starts |
| `1E 'i'` | the dialog that copies the key to the selected host starts |
| `1E 'x'` | the session is over |
| `1E 'w'` | what is the Wi-Fi status? |
| `1E 'l'` | which are the SSH hosts? |
| `1E 'p'` | a screenshot: the 3840 bytes of the screen follow, as they are |

**ESP32 to calculator.** Plain terminal output, except two answers. To the
status question: `1E 'w'`, a line of text, a newline. To the hosts
question: `1E 'l'`, then eight lines, each `user@host` (with `:port` if it
is not 22) or empty, each ended by a newline.

**Computer to ESP32** (`shell/shell.py`). Three `0x1E` bytes, a word and a
newline switch the sketch's serial mode: `raw` or `monitor`. In raw mode
entered that way the script keeps at most 128 bytes outstanding, and the
sketch answers `1E 'a'` for every 32 bytes it has passed on.

**ESP32 to computer.** In that paced raw mode a real `0x1E` from the
calculator is sent twice, and a screenshot is `1E 'p'` and its 3840 bytes.
The sketch answers the `raw` line with `1E 'r'` and sends `1E 'b'` when it
starts.

Those two exist because monitor mode prints text meant for a person. A
script that believes the sketch is in raw mode would take that text for
keys typed on the calculator and hand it to the shell. So `shell/shell.py`
passes nothing on until it has seen `1E 'r'`, and when the ESP32 restarts
under it (`ESP-ROM:` from the boot loader, then `1E 'b'`) it asks for raw
mode again and waits. It also takes the port for itself, so a second copy
cannot switch the mode behind its back.

## Keys in a session

| Key | Sends |
|---|---|
| letters, digits, punctuation | themselves |
| ENTER | carriage return |
| backspace | DEL (code 127), which shells take as "erase one character" |
| ◆ + backspace (DEL) | Delete, which erases forwards (`ESC [ 3 ~`) |
| ESC | Escape |
| arrows | the arrow keys |
| ◆ + letter | Ctrl + letter (the keyboard has no Ctrl key) |
| 2nd + I | Shift-Tab (`ESC [ Z`). Tab is ◆ + I, and shift cannot be added to that |
| ◆ + up / down | Page Up / Page Down |
| 2nd + up / down | Home / End (2nd + left / right too) |
| 2nd + plus (CHAR) | a symbol the keyboard lacks, picked from a strip on the bottom two rows |
| F1 to F8 | the function keys |
| 2nd + ESC (QUIT) | nothing: goes back to the menu. ◆ + ESC does the same |
| 2nd + APPS | nothing: takes a [screenshot](#screenshots), in the menus too |

Any other key sends nothing. That covers the TI-only characters, such as the
(−) negation key: use the subtraction key for `-`. The divide key types `/`.

**Ctrl.** ◆ + a letter sends what Ctrl + that letter sends on a computer
keyboard, in either case. The ones used most:

| Key | Is | Usually does |
|---|---|---|
| ◆ + C | Ctrl-C | stops the running program |
| ◆ + D | Ctrl-D | ends the input; at an empty prompt, logs out |
| ◆ + Z | Ctrl-Z | suspends the running program |
| ◆ + L | Ctrl-L | clears or redraws the screen |
| ◆ + I | Ctrl-I | Tab: completes a name in the shell |
| ◆ + R | Ctrl-R | searches the shell's history |
| ◆ + A / ◆ + E | Ctrl-A / Ctrl-E | start / end of the line in the shell |

Ctrl with a symbol is out of reach. The program would apply the same rule
to `[`, `\`, `]`, `^`, `_` and `@`, but the keyboard only types most of them
with 2nd, and ◆ + 2nd + a key sends nothing: tried with `]`, which is 2nd +
divide. So there is no Ctrl-] or Ctrl-\. Ctrl-[ is not missed, since it is
the ESC key.

**Symbols.** 2nd + plus shows a strip on the bottom two rows with each
symbol next to a letter, `a` to `v`, drawn as a key in reverse video. The
letter picks the symbol, and any other key closes the strip. Here the keys
are in square brackets:

```
[ a ]~ [ b ]` [ c ]| [ d ]\ [ e ]$ [ f ]% [ g ]& [ h ]# [ i ]@ [ j ]_ [ k ]{
[ l ]} [ m ][ [ n ]] [ o ]< [ p ]> [ q ]' [ r ]" [ s ]; [ t ]: [ u ]! [ v ]?
```

**Arrows.** They send the usual codes (`ESC [ A` and so on). Programs such as
`less` and `vim` ask for the "application" codes instead (`ESC O A`), and
the arrows, Home and End switch to those until the program switches back.

**Shift-Tab.** 2nd + I sends `ESC [ Z`, which programs that read the keys
themselves (Claude Code for one) take as Shift-Tab. Programs built on
ncurses, such as nchat, do not: they look the key up in the terminal's
description, and `vt220` has no entry for it, inside tmux as well, since
`tmux_calc.conf` gives its programs the same description. Such a program
has to be told the sequence. For nchat that is
`prev_chat=\033\133\132` in `~/.config/nchat/key.conf`, edited while nchat
is not running.

**Function keys.** F1 to F4 send `ESC O P` to `ESC O S`; F5 to F8 send
`ESC [ 15 ~`, `17 ~`, `18 ~` and `19 ~`.

**Finding a key's code.** The Debug menu's Key codes entry shows the number
the calculator gives for each key or combination. 2nd adds 4096 and ◆ adds
8192 to the key's own code.

The menu's first line shows when the program was built, which tells whether
the calculator has the latest copy.

## Screenshots

2nd + APPS sends the calculator's screen to the computer: the real pixels,
240 x 128, of whatever `term()` is showing. It works in the menus, the help
pages, the settings dialogs, with the symbol strip open, and in Serial and
SSH sessions. It takes a second or two, during which the screen stands
still.

Something on the computer has to be attached to the ESP32 to receive it.
The calculator does not have to be in a Serial session:

```sh
make shell         # the shell for the Serial entry; saves screenshots too
make screenshots   # runs nothing, only saves screenshots
```

Each one is saved as `screenshots/voyage200-<date>-<time>.png`, with every
calculator pixel four pixels wide (`shell/shell.py --scale` changes that,
`--screenshots` the folder). With nothing attached the picture is dropped;
the Serial Monitor and the Cardputer's log say so.

The screen is a plain bitmap in the calculator's memory, so the program
sends those bytes down the link and the ESP32 passes them on. The ESP32
holds back its own output while they arrive. Only `term()` can do this:
the Home screen and other programs are out of its reach.

## Characters

The shell's text is read as UTF-8. With 3x5 pixels per letter there is little
room for accents, so the terminal draws what it can and stands in for the rest:

| Characters | Drawn as |
|---|---|
| ASCII | its own glyph, from `font.txt` |
| Box drawing (U+2500 to U+257F), as `tree` and `mc` use | real lines that join from cell to cell; heavy, double and dashed lines all come out as plain ones |
| Block elements (U+2580 to U+259F), as progress bars use | halves, eighths, quadrants and shades, to the nearest pixel |
| Latin letters with accents (U+00A0 to U+017F) | the letter with its accent where the cell has room: acute, grave, circumflex, tilde and diaeresis as one row of pixels above a small letter (`á`, `à`, `ê`, `õ`), the cedilla as a pixel below (`ç`, `Ç`). Tilde and diaeresis look the same. The rest are drawn plain: accented capitals (`É` as `E`), small letters with an ascender, `ü`, other accents, `ß` as `s` |
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
adding the character to the right line. The Debug menu's test pattern shows a
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
  `shell/shell.py` keeps at most 128 bytes outstanding and the sketch
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
  48 KB instead of the default 8 KB. With SSH and its keys the sketch takes
  89% of the default 1.3 MB program partition.

For the wiring and the hardware, see [hardware.md](hardware.md).

---

[Project overview](../README.md) · [Getting started](getting-started.md) · [The terminal](terminal.md) · [tmux and screen](tmux-and-screen.md) · [Hardware](hardware.md) · [Link partner](link-partner.md) · [Repository](repository.md)
