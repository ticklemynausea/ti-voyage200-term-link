# Manual tests

There are no automatic tests for anything that needs the calculator, the
link or the ESP32, so these are run by hand on the hardware. Each section
names the feature it covers; run it after changing that feature.

The link layer itself, in `link/`, has automatic tests that run on the
computer: `make link:test`. Run them first after changing it.

## A lost link in a session

What it covers: the calculator sends a PING whenever it has sent nothing for
2 s, output or not; a session shows `ESP32: no answer, check the link cable`
on the bottom row after 12 s without a frame from the ESP32, takes it away at
the next one, and says `[ESP32 restarted, session lost: 2nd+ESC for the menu]`
when the two letters in the answer to the link's HELLO change. Frames that
are lost or garbled are sent again, so nothing is lost or garbled on the
screen. See "A lost link in a session" and "How the pieces
talk" in [terminal.md](terminal.md).

**Before you start.** Flash the ESP32 and send the program from the same
commit: `make esp32_stamps3:term:flash` (or the Cardputer's) and
`make calc:term:send`. The menu's first line and the Cardputer's build page
show the commit. Have an SSH host that answers, Wi-Fi set up, and the
Serial Monitor or the Cardputer's log open where a test asks for it.
"Quiet" means no output on the calculator's screen: a shell prompt that
nothing is printed to. Time with a watch: the limits are 2 s, 5 s and 12 s, so
anything within a couple of seconds of them is right.

### Nothing is lost: no change from before

| ID | Result | Steps | Expected |
|---|---|---|---|
| L01 | pass | SSH session, leave it quiet at the prompt for 2 minutes. | No banner. The prompt and cursor stay as they were; the cursor keeps blinking. Typing works afterwards. On the Cardputer, the calculator does not turn to "waiting". |
| L02 |  | Serial session with `make shell`, quiet for 2 minutes. Then type `echo ok`. | No banner. Nothing appears in the shell by itself: no `y`, no letters, no stray characters in the line typed. `ok` comes back. |
| L03 | pass | SSH: `seq 1 3000`, then `top` for 30 s, then `q`. | Output as before, no stray characters, no banner. |
| L04 | pass | SSH: run `sleep 5; printf '\e[10;20HX\n'`. | After 5 s an `X` at row 10, column 20. The pings sent during the sleep left no trace. |
| L05 | pass | SSH: open `vim` (or `less` on a long file), wait 10 s, then page with ◆+down a few times. | The screen redraws cleanly: the answers that arrived while it was quiet did not break the escape sequences. |
| L06 | pass | Settings → Wi-Fi → F1 on a stored network → at a question (name or password), wait 20 s, then answer. | The dialog is still waiting and takes the answer. It does not end by itself: `'y'` must not count as a command. |
| L07 |  | SSH to a host that asks for a password (no key). Wait 20 s at the prompt, then type it. | The login works. No banner while waiting. |
| L08 | pass | Main menu F3 Wi-Fi → choose a network that is out of range (or a stored one with a wrong password). | "Joining Wi-Fi ..." for up to 20 s, then "Wi-Fi: no connection". No banner at any point: the ESP32 answers while it joins. |
| L09 | pass | SSH to a host address that does not answer (an unused address on the LAN). | "Connecting to ..." then "Connection failed" after about 15 s. The banner may show for the last 3 s or so, and goes away when the failure is printed. Note what happened. |
| L10 | pass | Quiet SSH session for 5 s, then ◆+APPS with `make screenshots` attached. | The screenshot is saved and shows the session without a banner. The session carries on, no stray characters. |
| L11 |  | In the menus: wait on the main menu, the settings menu, the SSH host list. | Wi-Fi status as before, host and network lists complete and correct. A late `'y'` answer from the end of a session does not appear anywhere. |
| L12 |  | Debug → Local echo, and Debug → Key codes, each for 20 s without typing. | No banner: these sessions do not use the link. |
| L13 |  | SSH: `yes`, and after a few seconds press 2nd+ESC. | The menu comes back within a second or two, while the output is still pouring in. Before, the keys were only read once the output paused. |
| L14 |  | SSH: `yes`, then Ctrl-C (◆ + C). | Nothing is lost to a collision any more: `yes` stops within a few seconds of one press. Note it if a second press was needed. |
| L15 |  | SSH: `seq 1 100000`, and press ◆+APPS during it, with `make screenshots` attached. | The screenshot is saved, complete, showing numbers. The numbers carry on afterwards. |

### The cable is pulled

To leave the bottom row with something on it for these: in SSH, run `clear`
and then `seq 1 30`, so the prompt sits on the last row.

| ID | Result | Steps | Expected |
|---|---|---|---|
| L20 | pass | Quiet SSH session with the prompt on the last row. Pull the calculator's link cable. | After about 12 s (no later than 15 s) the bottom row shows the banner in reverse video. The cursor stops blinking. |
| L21 | pass | Then plug it back in, without typing. | Within about 2 s the banner goes and the bottom row is exactly what was under it: the prompt, with the cursor back. The session works: `echo ok`. |
| L22 |  | As L20, but type a few letters while the cable is out, then plug it back in. | Nothing crashes, the banner goes as in L21. The letters typed arrive once the cable is back, late but in order, none missing; only typing more than the calculator can hold (a few hundred letters) loses the rest. |
| L23 | pass | Pull the cable for 5 s only during a quiet session, then plug it back. | No banner at all. The session carries on. |
| L24 |  | SSH: `while true; do date; sleep 1; done`. Pull the cable for 30 s, then plug it back. | Banner after about 12 s. After plugging back, within a few seconds, the banner goes and the dates from the gap arrive in a burst, **none missing**, clean and not garbled: the ESP32 sent nothing while the cable was out and left the SSH output unread. Then one a second again. 2nd+ESC afterwards reaches a working menu. |
| L29 |  | L24 five times in a row, plugging the cable back at a different moment each time. | Each time as in L24. If the menu is ever left at "ESP32: no answer", go on with L38. |
| L24b |  | SSH: `while true; do date; sleep 1; done`. Unplug the cable and plug it back **at once**, five times, a while apart. | Nothing is garbled at any point. After a replug the dates may pause for a few seconds and then catch up, none missing; keys typed meanwhile arrive late. No banner, no restart of `term()`, no reset of the ESP32. Note how many seconds each pause took; expect about 5 s plus a few. The Cardputer's link page may count bad frames and frames sent again: that is the link layer at work. |
| L25 |  | Serial session with `make shell`: L20 and L21 again. | Same as SSH. |
| L26 | pass | Settings → SSH hosts → a host → at a question, pull the cable for 20 s, then plug it back. | Banner while it is out. After plugging back the banner goes, the question is still there and can be answered. |
| L27 | pass | Pull the cable during a quiet SSH session, wait for the banner, press 2nd+ESC. Then plug the cable back. | The menu comes back at once, with "ESP32: no answer" and its entries greyed. After plugging back the menu's status returns within a few seconds and the entries can be chosen. A new SSH session works. |
| L28 |  | Pull the cable while `seq 1 3000` is printing, wait 20 s, plug back. | Banner shows once printing stops reaching the calculator. After plugging back, the rest of the numbers arrive and the banner goes. No number is missing, not even at the moment the cable came out. |

### The ESP32 restarts or goes away

| ID | Result | Steps | Expected |
|---|---|---|---|
| L30 | pass | Quiet SSH session. Press the ESP32's reset button (or unplug its USB and plug it back at once). | Within about 5 s of the ESP32 coming back, `[ESP32 restarted, session lost: 2nd+ESC for the menu]` appears once. If the restart took more than 12 s, the banner shows first and goes when that line appears. |
| L31 | pass | Then type a few keys and wait a minute. | The line does not appear again. 2nd+ESC goes to the menu, which works, and a new SSH session works. |
| L32 | pass | SSH: `while true; do date; sleep 1; done`, then reset the ESP32. | The dates stop, and the restart line appears a few seconds later. |
| L33 |  | Serial session, reset the ESP32. | The restart line, as in L30. `make shell` notices the restart and waits for raw mode, as before. |
| L34 | pass | Inside a settings dialog (Wi-Fi), reset the ESP32. | The restart line. 2nd+ESC and the menu work. |
| L35 | pass | Quiet SSH session, then unplug the ESP32's power for good. | Banner after about 12 s, staying. No restart line. 2nd+ESC: menu with "ESP32: no answer". |
| L36 | pass | Reset the ESP32 twice in the same session, a minute apart. | One restart line only. |
| L37 | pass | Reset the ESP32 while the calculator is in the main menu, then start an SSH session. | No restart line: each session takes the letters from its own first answer. |
| L38 |  | Main menu, ESP32 powered off for a minute: the menu resets its link every 3 s meanwhile. Power the ESP32 again. | "ESP32: no answer" while it is off. Within a few seconds of the ESP32 starting, the Wi-Fi status is back and the entries can be chosen; nothing else shows on the screen. If a run of L29 left the menu at "no answer" with the ESP32 on, the same: it comes back within about 10 s, without leaving `term()` or resetting the ESP32. |
| L39 |  | SSH session, 2nd+ESC with the cable out, wait 20 s in the menu, plug the cable back. | The menu answers again within a few seconds. The Cardputer shows the SSH session closed: the menu sent the end of the session again after resetting its link. |

### The link's own bytes in the output

The output goes to the calculator in frames, so no byte of it can pass for
the ESP32: not `0x1E`, which used to mark its answers, nor `C0` and `C1`,
which delimit frames. From `shell.py` to the ESP32 a real `0x1E` is sent as
`1E 'e'`, so it can never pass for a control line either. Run each in SSH
and again in a Serial session with `make shell`.

| ID | Result | Steps | Expected |
|---|---|---|---|
| L50 |  | `printf 'a\x1ew+fake status\nb\n'` | `aw+fake status` on one line, `b` on the next: the `1E` itself draws nothing, and nothing is taken for an answer. |
| L51 |  | Wait 5 s, then `printf '\x1eyQQ\n'`, and again with `ZZ`. | `yQQ` and `yZZ` are shown. No restart line. |
| L52 |  | `printf '\e[10;20HX\x1e\e[12;20HY\n'` | `X` at row 10, `Y` at row 12, both in column 20: a `1E` between two sequences leaves both alone. |
| L53 |  | `head -c 20000 /dev/urandom` (in Serial: a few times), then `reset`. | Garbage on the screen, then a clean terminal after `reset`. The session goes on working: `echo ok`. In Serial, the ESP32 stays in raw mode (no monitor text appears) and output keeps flowing afterwards, which shows that runs of `1E` in the output neither start control lines nor upset the acknowledgements. The random bytes contain `C0` and `C1` too: the link carries them like any other. |
| L54 |  | Serial only: `printf '\x1e\x1e\x1emonitor\n'; echo still-raw` | Three `1E` draw nothing, then `monitor` and `still-raw` are shown. The ESP32 stays in raw mode. |
| L55 |  | `printf 'a\xc0\xc1\xc0b\n'; printf '\xc1\xc1\xc1\xc1\xc1\xc1c\n'` | Two lines, ending in `b` and `c`, with a few blocks or nothing where the stray bytes were. The session carries on: `echo ok`. The Cardputer's "bad" row does not go up. |

## Serial sessions with make shell

What it covers: while the calculator is away the ESP32 leaves `shell.py`'s
bytes unread, so nothing is lost; the ESP32 asks `shell.py` `1E 'k'` and
tells the calculator when it stops answering; `shell.py` keeps the command
running while the ESP32's port is gone and opens it again when it is back.
See "A lost link in a session" in [terminal.md](terminal.md).

Start each test with `make shell` running and the calculator in a Serial
session, unless it says otherwise.

### The calculator away

| ID | Result | Steps | Expected |
|---|---|---|---|
| S01 |  | `i=0; while true; do i=$((i+1)); echo $i; sleep 1; done`. Pull the calculator's cable for 30 s, then plug it back. | Banner after about 12 s. After plugging back the numbers carry on **without a gap**: every number appears, the missed ones in a burst. At most a character or two may be missing around the moment the cable came out. |
| S02 |  | `seq 1 5000`, pull the cable half way through, wait 20 s, plug back. | The rest of the numbers arrive, in order and without a gap, apart from a character or two at the moment the cable came out. |
| S03 |  | Leave the calculator in the main menu for a minute while `make shell` runs `while true; do date; sleep 1; done`, then choose Serial. | As before this change: the dates held back meanwhile arrive when the session starts. |

### The computer away

| ID | Result | Steps | Expected |
|---|---|---|---|
| S10 |  | Quiet Serial session. Kill `make shell` hard: `kill -9` on the `shell.py` process from another window. | After about 10 s: `[no answer from the computer: is make shell still running?]` on the calculator, once. The Cardputer's log says "computer: no answer". |
| S11 |  | Then run `make shell` again. | `[the computer is answering again]`, the screen is reset and the new shell's prompt appears. It works. |
| S12 |  | Quiet Serial session, close the terminal window that runs `make shell`. | As S10. |
| S13 |  | Quiet Serial session, put the Mac to sleep for a minute (the ESP32 needs its own power for this: the Cardputer on battery). Wake it. | As S10 while asleep. On waking, `make shell` carries on and the calculator says the computer is answering again. If `make shell` did not survive the sleep, note what it printed. |
| S14 |  | Quiet Serial session, leave it for 5 minutes. | No message: `make shell` answers every question. |
| S15 |  | Serial session without `make shell` ever started since the ESP32 powered up (monitor mode). Wait 20 s. | No message: the ESP32 asks only a paced host. |
| S16 |  | Start `make shell`, then end it normally (Ctrl-C). Stay in the Serial session 20 s. | `[session ended]`, then no further message: the ESP32 is back in monitor mode. |

### The ESP32's port away

| ID | Result | Steps | Expected |
|---|---|---|---|
| S20 |  | `make shell` running, unplug the ESP32's USB cable (Stamp S3: it powers off). | `make shell` prints that the port went away and keeps running; no traceback. The calculator shows its banner. |
| S21 |  | Plug the USB cable back. | `make shell` prints that the port is back. After the ESP32 has started and the calculator is in a Serial session again, `echo ok` works; Ctrl-L redraws the prompt. |
| S22 |  | Cardputer on battery: unplug its USB cable with `make shell` running, wait 10 s, plug it back. | `make shell` says the port went away and is back. The calculator shows the no-answer line from the computer while it is out, and the answering-again line once it is back. The session carries on without restarting. |
| S23 |  | With the port gone, run `yes` in the shell for a few seconds via `--mirror` (or anything that prints a lot), then Ctrl-C it. | `make shell` stays responsive and does not grow without limit: the command is paused once its output piles up. |
| S24 |  | With the port gone, exit the shell (`exit`, typed in the `--mirror` window). | `make shell` ends at once instead of waiting for the port. |
| S25 |  | Start `make shell` with the ESP32 unplugged. | "No ESP32 found", as before. |

## The Cardputer's memory page

Press `4` on the Cardputer for the memory page. The calculator sends its
numbers every 5 s while `term` runs.

| ID | Result | Steps | Expected |
|---|---|---|---|
| M01 |  | Start `term` and stay in the main menu. Note the calculator's free RAM and archive on its own MEM screen first (2nd + 6, before starting `term`). | The ESP32 rows are filled and none is orange. The calculator's "s ago" counts up to about 5 and back to 0. Its "RAM free" is below the MEM screen's by about the size of `term` and a little more, its "archive" free and "archived" match MEM's to within a few KB. |
| M02 |  | SSH session running `yes` for a minute. | "s ago" still goes no higher than about 5: the reports wait for room but still get through. `yes` keeps its speed. |
| M03 |  | Quit `term` (2nd+ESC in the menu), then wait 30 s. | "s ago" keeps growing; the numbers stay as they were. Start `term` again: back to at most 5 s. |
| M04 |  | With `term` running, reset the ESP32. | The calculator's rows come back within a few seconds of the ESP32 starting, without waiting for the minute the archive takes. |

## The clock

The ESP32 sets the calculator's clock from an NTP server on its own
network. Set the time zone first in the Serial Monitor, such as
`/tz lisbon`. Press `2` on the Cardputer for the time
page; on a Stamp S3, `/settings` shows the servers.

| ID | Result | Steps | Expected |
|---|---|---|---|
| T01 |  | On the network whose router serves NTP, set the calculator's clock wrong by hand, then start `term`. | Within a few seconds the calculator's clock (MODE, F3 Clock, or the status bar) shows the right local time, to within 2 s, and is on. The time page's time is green, the gateway row green, "calculator set" counts up from 0. |
| T02 |  | `/tz UTC`, then leave `term` and start it again. | The calculator's clock moves to UTC, and its time zone setting is 0. `/tz` back to yours and again: back to local time. |
| T02b |  | `/tz new york`, `/tz america/new_york`, `/tz EST5EDT,M3.2.0,M11.1.0`, `/tz york`, `/tz atlantis`. | The first two give America/New_York and its string; the third takes the string, with no name; `york` lists America/New_York; `atlantis` says no zone has it. The time zone is unchanged by the last two. |
| T03 |  | Power the ESP32 off and on, `/settings`. | The time zone is the one set before. |
| T04 |  | Join a network without NTP (a phone's hotspot), set the calculator's clock wrong, start `term`. | The time page says "none: the time is not set" or "no answer yet", the time is orange or "not set". The calculator's clock stays wrong. |
| T05 |  | Leave `term` running in the menu for over an hour on the NTP network. | "last answer" goes back to 0 about every hour, and "calculator set" with it. |

## The OS's modifier indicators

The OS's 2nd, shift and diamond indicators are kept off the screen while
`term` runs (see Findings in [terminal.md](terminal.md)).

| ID | Result | Steps | Expected |
|---|---|---|---|
| I01 | pass | Main menu: press 2nd, then 2nd again; then diamond, then a letter. | The reverse status row stays whole: no indicator, no white hole. |
| I02 | pass | SSH session in tmux: press 2nd, diamond and shift, each followed by a key. | The tmux bar stays whole. |
| I03 | pass | SSH session: shift lock on, type a few letters, shift lock off. | Capitals while it is on; no indicator, no hole. |
| I04 |  | With text on the bottom row and 2nd armed, let the screen scroll (`ls -l`). | No stray pixels in the text. |
| I05 |  | ◆ + APPS on the main menu. | The screenshot's status row has no white block. |
| I06 | pass | Keys still work: 2nd + ESC, diamond + C, 2nd + divide (`]`), shift + a letter, a held key repeating. | All as before. |
| I07 | pass | Quit `term`, then press 2nd on the Home screen. | The OS's own indicator shows there as usual: the handler was taken out. |

## More keys: Alt, F9 to F12, Insert, modified arrows

In a Serial or SSH session. `cat -v` shows the bytes a key sends: run it,
press the keys, then ◆ + D. Run them in bash (or zsh) where a step says so.

| ID | Result | Steps | Expected |
|---|---|---|---|
| X01 |  | bash: `echo one two`, ENTER; then `ls `, shift + Hand, `.`. | `two` is put in after `ls `. |
| X02 |  | bash: type `one two three`, press 2nd + left (Home), then shift + Hand, `d`. | `one` is deleted. |
| X03 |  | bash: type `one two`, then shift + Hand, backspace. | `two` is deleted. |
| X04 |  | bash: shift + Hand, shift + Hand, `d`. | A plain `d` is typed: the second press called Alt off. |
| X05 |  | `cat -v`: Hand + left, right, up, down. | `^[[1;5D^[[1;5C^[[1;5A^[[1;5B` |
| X06 |  | `cat -v`: shift + left, right, up, down. | `^[[1;2D^[[1;2C^[[1;2A^[[1;2B` |
| X07 |  | `cat -v`: ◆ + F1, F2, F3, F4. | `^[[20~^[[21~^[[23~^[[24~` |
| X08 |  | `cat -v`: 2nd + backspace, then ◆ + space. | `^[[2~^@` |
| X09 |  | `htop`: ◆ + F1, ESC, then ◆ + F2. | F9 opens the kill menu, ESC closes it, F10 quits. |
| X10 |  | zsh or `vim` insert mode: Hand + left / right. | The cursor moves a word at a time. |
| X11 |  | Debug, Key codes: shift + Hand. | `380` is printed, and the next key prints its code as usual. |
| X12 |  | bash: `ec`, then APPS. | It completes to `echo`. |
| X13 |  | bash: type `one two`, then CLEAR. | The line is erased. |
| X14 |  | bash: LN, then type `echo`. | The history search finds the last `echo`. |
| X15 |  | bash: MODE, `.`, as in X01. | The last argument is put in, as with shift + Hand. |
| X16 |  | tmux with two windows: COS, SIN, then TAN, `c`. | Next window, previous window, then a new one. |
| X17 |  | `cat -v`: ◆ + ^. | `^^` |
| X18 |  | `cat -v`: 2nd + APPS, ◆ + MODE, 2nd + LN, ◆ + LN. | `^[[Z^]^_^\` (◆ + LN may end `cat` instead: Ctrl-\ quits it) |
| X19 |  | bash: type `one two`, then 2nd + LN. | The last word typed is undone. |
| X20 |  | `cat /bin/ls` or `head -c 2000 /dev/urandom`, then ◆ + CLEAR. | The terminal is reset and the shell redraws its prompt; what is typed next looks right. |
| X21 |  | tmux: 2nd + TAN, 2nd + TAN, then 2nd + SIN, ESC; 2nd + COS. | Two new windows; the list opens and closes; then the window before. |
| X22 |  | tmux, after `seq 100`: STO, ◆ + up a few times, `q`. | Copy mode, pages back, then out of it. |
| X23 |  | tmux: STO, move to a word, select and copy it (emacs keys: ◆ + space, move, MODE then w; vi keys: Space, move, ENTER); then 2nd + STO. | The copied text is typed at the prompt. |
| X24 |  | `cat -v`: (−), θ, 2nd + D, 2nd + F, 2nd + J, 2nd + ^, 2nd + ×. | ``-$~`%`~`` |
| X25 |  | bash: `echo one two`, ENTER, then 2nd + (−); clear the line, then 2nd + ENTER. | `two` is put in; then `echo one two` runs again. |
| X26 |  | tmux_calc.conf, three windows: 2nd + 1, 2nd + 3, 2nd + 2. | Windows 1, 3, then 2 (the bar shows the first as `1`). |
| X27 |  | tmux: ◆ + SIN, ◆ + COS, ◆ + TAN a few times; then 2nd + minus, ESC. | The window splits side by side, then one pane splits top and bottom; the cursor moves through the three panes; the list of sessions opens and closes. |

## Soak tests: long, heavy streams

The hardest cases, so they come last: run them once everything above
passes. They check that the link stays sound with the link full for
minutes on end. That covers the link layer's frames and acknowledgements,
the `1E 'e'` escapes and the serial window's acknowledgements under load,
and pings and answers meeting a busy link.

The link carries about 3 KB/s, so 1 MB takes about six minutes. Keep the
Cardputer's link page open (or note `/stats` in the Serial Monitor
afterwards, for an ESP32 without a screen). In every test, unless it says
otherwise, these must hold throughout:

- no banner, no restart line, no line about the computer
- the calculator keeps drawing: the output never stops for more than a
  second or two
- afterwards, Ctrl-C (or `q`) stops the program within a few seconds of one
  press: a key that collides with the ESP32's bytes is sent again
- afterwards, `echo ok` works and the prompt is where it should be (run
  `reset` first after binary output)
- the link page's "dropped" row stays at 0. Its "sent again", "bad" and
  "handshake" rows may go up: they count what the link layer put right
- the memory page's "heap free" is back to about where it started once
  the stream is over, and "least heap" stays above 20 KB

| ID | Result | Steps | Expected |
|---|---|---|---|
| K01 |  | SSH: `yes` for 10 minutes, then Ctrl-C. | As above. |
| K02 |  | SSH: `head -c 1000000 /dev/urandom`, to the end (about 6 minutes). Then `reset`. | As above. The random bytes contain thousands of `0x1E`, `C0` and `C1`: none of them ends the session, shows the restart line or leaves stray text. The calculator does not hang on the escape sequences that random bytes make. |
| K03 |  | Serial with `make shell`: `yes` for 10 minutes, then Ctrl-C. Check `ps -o rss= -p <pid of shell.py>` at the start and the end. | As above. The output does not slow down over time, which would point to the window shrinking. `shell.py`'s memory stays about the same. |
| K04 |  | Serial with `make shell`: `head -c 1000000 /dev/urandom`, to the end. Then `reset`. | As above. The ESP32 stays in raw mode: no monitor text shows up as keys or on the screen. The output keeps its speed to the end. This shows that the `1E 'e'` escapes and the acknowledgements stay in step. |
| K05 |  | Serial with `make shell`: `seq 1 200000`, to the end. | As above. The last lines read `199999`, `200000`. |
| K06 |  | SSH: `yes`, and press ◆+APPS five times, a few seconds apart, with `make screenshots` attached. | Five complete PNG files, each showing lines of `y`. The stream carries on after each one. |
| K07 |  | Both ways at once. In SSH run `cat`, then hold down a letter key for 60 s (auto-repeat), so that every key is echoed back while more are typed. | The session survives and `cat` is still echoing at the end. Ctrl-D ends it. No key is lost: compare the letters that came back with the auto-repeat rate times 60 s. |
| K08 |  | Serial with `make shell`: `seq 1 100000`. Open the CHAR strip (2nd + plus) for 20 s, then close it with any key. | The output carries on where it stopped, **without a gap**: the ESP32 stopped reading `make shell` while the calculator was not taking bytes. |
| K09 |  | SSH: the same as K08. | The same: without a gap. The ESP32 stopped reading the SSH output while the calculator was not taking bytes. |
| K10 |  | Serial with `make shell`: `head -c 1000000 /dev/urandom`. Pull the calculator's cable for 30 s half way through, then plug it back. | Banner while it is out. After plugging back the stream carries on at full speed to the end; the ESP32 is still in raw mode, and the output after `reset` and `echo ok` is clean. |
| K11 |  | Leave a quiet SSH session for 30 minutes (about 600 pings), then run `seq 1 3000`. | No banner or message in the 30 minutes. The output then runs as in L03. |
| K12 |  | Leave a Serial session with `make shell` running `while true; do date; sleep 60; done` for several hours, overnight if possible. | One date a minute the whole time, none missing. No banner or message. `shell.py`'s memory is about the same at the end as after the first minute. |

## Recording a run

The Result column holds the last run: pass, fail, or not run; empty means
not run yet. The results there are from the run of 17 October 2026, before
the link layer; the tests whose expectations it changed are empty again,
and the rest are worth running again too.

Write down the date, the commit (from the menu's first line), the board,
and for each test either "ok" or what happened instead. A failing test is
worth a note even when the cause is clear.
