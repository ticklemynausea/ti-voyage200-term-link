# Manual tests

There are no automatic tests for anything that needs the calculator, the
link or the ESP32, so these are run by hand on the hardware. Each section
names the feature it covers; run it after changing that feature.

## A lost link in a session

What it covers: a quiet session asks the ESP32 `1E 'y'` every 3 s, shows
`ESP32: no answer, check the link cable` on the bottom row after 12 s of
silence, takes it away at the next byte, and says
`[ESP32 restarted, session lost: 2nd+ESC for the menu]` when the answer's
two letters change. See "A lost link in a session" and "How the pieces
talk" in [terminal.md](terminal.md).

**Before you start.** Flash the ESP32 and send the program from the same
commit: `make esp32_stamps3:term:flash` (or the Cardputer's) and
`make calc:term:send`. The menu's first line and the Cardputer's build page
show the commit. Have an SSH host that answers, Wi-Fi set up, and the
Serial Monitor or the Cardputer's log open where a test asks for it.
"Quiet" means no output on the calculator's screen: a shell prompt that
nothing is printed to. Time with a watch: the limits are 3 s and 12 s, so
anything within a couple of seconds of them is right.

### Nothing is lost: no change from before

| ID | Steps | Expected |
|---|---|---|
| L01 | SSH session, leave it quiet at the prompt for 2 minutes. | No banner. The prompt and cursor stay as they were; the cursor keeps blinking. Typing works afterwards. On the Cardputer, the calculator does not turn to "waiting". |
| L02 | Serial session with `make shell`, quiet for 2 minutes. Then type `echo ok`. | No banner. Nothing appears in the shell by itself: no `y`, no letters, no stray characters in the line typed. `ok` comes back. |
| L03 | SSH: `seq 1 3000`, then `top` for 30 s, then `q`. | Output as before, no stray characters, no banner. |
| L04 | SSH: run `sleep 5; printf '\e[10;20HX\n'`. | After 5 s an `X` at row 10, column 20. The pings sent during the sleep left no trace. |
| L05 | SSH: open `vim` (or `less` on a long file), wait 10 s, then page with ◆+down a few times. | The screen redraws cleanly: the answers that arrived while it was quiet did not break the escape sequences. |
| L06 | Settings → Wi-Fi → F1 on a stored network → at a question (name or password), wait 20 s, then answer. | The dialog is still waiting and takes the answer. It does not end by itself: `'y'` must not count as a command. |
| L07 | SSH to a host that asks for a password (no key). Wait 20 s at the prompt, then type it. | The login works. No banner while waiting. |
| L08 | Main menu F3 Wi-Fi → choose a network that is out of range (or a stored one with a wrong password). | "Joining Wi-Fi ..." for up to 20 s, then "Wi-Fi: no connection". No banner at any point: the ESP32 answers while it joins. |
| L09 | SSH to a host address that does not answer (an unused address on the LAN). | "Connecting to ..." then "Connection failed" after about 15 s. The banner may show for the last 3 s or so, and goes away when the failure is printed. Note what happened. |
| L10 | Quiet SSH session for 5 s, then 2nd+APPS with `make screenshots` attached. | The screenshot is saved and shows the session without a banner. The session carries on, no stray characters. |
| L11 | In the menus: wait on the main menu, the settings menu, the SSH host list. | Wi-Fi status as before, host and network lists complete and correct. A late `'y'` answer from the end of a session does not appear anywhere. |
| L12 | Debug → Local echo, and Debug → Key codes, each for 20 s without typing. | No banner: these sessions do not use the link. |

### The cable is pulled

To leave the bottom row with something on it for these: in SSH, run `clear`
and then `seq 1 30`, so the prompt sits on the last row.

| ID | Steps | Expected |
|---|---|---|
| L20 | Quiet SSH session with the prompt on the last row. Pull the calculator's link cable. | After about 12 s (no later than 15 s) the bottom row shows the banner in reverse video. The cursor stops blinking. |
| L21 | Then plug it back in, without typing. | Within about 3 s the banner goes and the bottom row is exactly what was under it: the prompt, with the cursor back. The session works: `echo ok`. |
| L22 | As L20, but type a few letters while the cable is out, then plug it back in. | Nothing crashes, the banner goes as in L21. Note whether the letters typed arrived late, arrived in part, or were dropped: any of those is acceptable, but write down which. |
| L23 | Pull the cable for 5 s only during a quiet session, then plug it back. | No banner at all. The session carries on. |
| L24 | SSH: `while true; do date; sleep 1; done`. Pull the cable for 30 s, then plug it back. | Banner after about 12 s. After plugging back the dates carry on and the banner goes at the first one. Some dates from the gap are lost: expected, the ESP32 gives up on bytes it cannot send. |
| L25 | Serial session with `make shell`: L20 and L21 again. | Same as SSH. |
| L26 | Settings → SSH hosts → a host → at a question, pull the cable for 20 s, then plug it back. | Banner while it is out. After plugging back the banner goes, the question is still there and can be answered. |
| L27 | Pull the cable during a quiet SSH session, wait for the banner, press 2nd+ESC. Then plug the cable back. | The menu comes back at once, with "ESP32: no answer" and its entries greyed. After plugging back the menu's status returns within a few seconds and the entries can be chosen. A new SSH session works. |
| L28 | Pull the cable while `seq 1 3000` is printing, wait 20 s, plug back. | Banner shows once printing stops reaching the calculator. After plugging back, whatever output is left arrives and the banner goes; the screen may be out of step with the shell, Ctrl-L redraws it. |

### The ESP32 restarts or goes away

| ID | Steps | Expected |
|---|---|---|
| L30 | Quiet SSH session. Press the ESP32's reset button (or unplug its USB and plug it back at once). | Within about 5 s of the ESP32 coming back, `[ESP32 restarted, session lost: 2nd+ESC for the menu]` appears once. If the restart took more than 12 s, the banner shows first and goes when that line appears. |
| L31 | Then type a few keys and wait a minute. | The line does not appear again. 2nd+ESC goes to the menu, which works, and a new SSH session works. |
| L32 | SSH: `while true; do date; sleep 1; done`, then reset the ESP32. | The dates stop, and the restart line appears a few seconds later. |
| L33 | Serial session, reset the ESP32. | The restart line, as in L30. `make shell` notices the restart and waits for raw mode, as before. |
| L34 | Inside a settings dialog (Wi-Fi), reset the ESP32. | The restart line. 2nd+ESC and the menu work. |
| L35 | Quiet SSH session, then unplug the ESP32's power for good. | Banner after about 12 s, staying. No restart line. 2nd+ESC: menu with "ESP32: no answer". |
| L36 | Reset the ESP32 twice in the same session, a minute apart. | One restart line only. |
| L37 | Reset the ESP32 while the calculator is in the main menu, then start an SSH session. | No restart line: each session takes the letters from its own first answer. |

### Old and new together

Updating only one side is easy to do by mistake, so check what happens.

| ID | Steps | Expected |
|---|---|---|
| L40 | New program on the calculator, a sketch from before this change on the ESP32. Start SSH and leave it quiet. | After 3 s the ESP32 takes `'y'` for an unknown command and ends the session: the shell stops answering and the Cardputer's log says "calculator: menu". This is the documented limit; flashing the ESP32 fixes it. |
| L41 | Old program on the calculator, new sketch on the ESP32. Use SSH and Serial for a while. | Everything works as before. The ESP32 is never asked `'y'`. |

### A real 0x1E in the output

A real `0x1E` from the host is sent to the calculator doubled, and from
`shell.py` to the ESP32 as `1E 'e'`, so the host's output can never pass for
an answer or a control line. Run each in SSH and again in a Serial session
with `make shell`.

| ID | Steps | Expected |
|---|---|---|
| L50 | `printf 'a\x1ew+fake status\nb\n'` | `aw+fake status` on one line, `b` on the next: the `1E` itself draws nothing, and nothing is taken for an answer. |
| L51 | Wait 5 s, then `printf '\x1eyQQ\n'`, and again with `ZZ`. | `yQQ` and `yZZ` are shown. No restart line. |
| L52 | `printf '\e[10;20HX\x1e\e[12;20HY\n'` | `X` at row 10, `Y` at row 12, both in column 20: a `1E` between two sequences leaves both alone. |
| L53 | `head -c 20000 /dev/urandom` (in Serial: a few times), then `reset`. | Garbage on the screen, then a clean terminal after `reset`. The session goes on working: `echo ok`. In Serial, the ESP32 stays in raw mode (no monitor text appears) and output keeps flowing afterwards, which shows that runs of `1E` in the output neither start control lines nor upset the acknowledgements. |
| L54 | Serial only: `printf '\x1e\x1e\x1emonitor\n'; echo still-raw` | Three `1E` draw nothing, then `monitor` and `still-raw` are shown. The ESP32 stays in raw mode. |

## Serial sessions with make shell

What it covers: while the calculator is away the ESP32 leaves `shell.py`'s
bytes unread, so nothing is lost; the ESP32 asks `shell.py` `1E 'k'` and
tells the calculator when it stops answering; `shell.py` keeps the command
running while the ESP32's port is gone and opens it again when it is back.
See "A lost link in a session" in [terminal.md](terminal.md).

Start each test with `make shell` running and the calculator in a Serial
session, unless it says otherwise.

### The calculator away

| ID | Steps | Expected |
|---|---|---|
| S01 | `i=0; while true; do i=$((i+1)); echo $i; sleep 1; done`. Pull the calculator's cable for 30 s, then plug it back. | Banner after about 12 s. After plugging back the numbers carry on **without a gap**: every number appears, the missed ones in a burst. At most a character or two may be missing around the moment the cable came out. |
| S02 | `seq 1 5000`, pull the cable half way through, wait 20 s, plug back. | The rest of the numbers arrive, in order and without a gap, apart from a character or two at the moment the cable came out. |
| S03 | Leave the calculator in the main menu for a minute while `make shell` runs `while true; do date; sleep 1; done`, then choose Serial. | As before this change: the dates held back meanwhile arrive when the session starts. |

### The computer away

| ID | Steps | Expected |
|---|---|---|
| S10 | Quiet Serial session. Kill `make shell` hard: `kill -9` on the `shell.py` process from another window. | After about 10 s: `[no answer from the computer: is make shell still running?]` on the calculator, once. The Cardputer's log says "computer: no answer". |
| S11 | Then run `make shell` again. | `[the computer is answering again]`, the screen is reset and the new shell's prompt appears. It works. |
| S12 | Quiet Serial session, close the terminal window that runs `make shell`. | As S10. |
| S13 | Quiet Serial session, put the Mac to sleep for a minute (the ESP32 needs its own power for this: the Cardputer on battery). Wake it. | As S10 while asleep. On waking, `make shell` carries on and the calculator says the computer is answering again. If `make shell` did not survive the sleep, note what it printed. |
| S14 | Quiet Serial session, leave it for 5 minutes. | No message: `make shell` answers every question. |
| S15 | Serial session without `make shell` ever started since the ESP32 powered up (monitor mode). Wait 20 s. | No message: the ESP32 asks only a paced host. |
| S16 | Start `make shell`, then end it normally (Ctrl-C). Stay in the Serial session 20 s. | `[session ended]`, then no further message: the ESP32 is back in monitor mode. |

### The ESP32's port away

| ID | Steps | Expected |
|---|---|---|
| S20 | `make shell` running, unplug the ESP32's USB cable (Stamp S3: it powers off). | `make shell` prints that the port went away and keeps running; no traceback. The calculator shows its banner. |
| S21 | Plug the USB cable back. | `make shell` prints that the port is back. After the ESP32 has started and the calculator is in a Serial session again, `echo ok` works; Ctrl-L redraws the prompt. |
| S22 | Cardputer on battery: unplug its USB cable with `make shell` running, wait 10 s, plug it back. | `make shell` says the port went away and is back. The calculator shows the no-answer line from the computer while it is out, and the answering-again line once it is back. The session carries on without restarting. |
| S23 | With the port gone, run `yes` in the shell for a few seconds via `--mirror` (or anything that prints a lot), then Ctrl-C it. | `make shell` stays responsive and does not grow without limit: the command is paused once its output piles up. |
| S24 | With the port gone, exit the shell (`exit`, typed in the `--mirror` window). | `make shell` ends at once instead of waiting for the port. |
| S25 | Start `make shell` with the ESP32 unplugged. | "No ESP32 found", as before. |

### Old and new together

| ID | Steps | Expected |
|---|---|---|
| S30 | New `shell.py`, old sketch on the ESP32: `printf 'a\x1eb\n'`. | `ab`: the old sketch passes `1E 'e'` on as it is, and the calculator drops it as a mark and a letter that is not an answer. A real `0x1E` is lost, nothing else. |

## Soak tests: long, heavy streams

The hardest cases, so they come last: run them once everything above
passes. They check that the link stays sound with the link full for
minutes on end. That covers the doubled `0x1E` bytes, the `1E 'e'` escapes
and the window's acknowledgements under load, and pings and answers
meeting a busy link.

The link carries about 3 KB/s, so 1 MB takes about six minutes. Keep the
Cardputer's link page open (or note `/stats` in the Serial Monitor
afterwards, for an ESP32 without a screen). In every test, unless it says
otherwise, these must hold throughout:

- no banner, no restart line, no line about the computer
- the calculator keeps drawing: the output never stops for more than a
  second or two
- afterwards, Ctrl-C (or `q`) stops the program within a few seconds. A key
  pressed during a stream can collide with the ESP32's bytes and be lost
  (see "Collisions" under Findings in [terminal.md](terminal.md)), so press
  it again if nothing happens
- afterwards, `echo ok` works and the prompt is where it should be (run
  `reset` first after binary output)
- the link page's "dropped" count does not go up

| ID | Steps | Expected |
|---|---|---|
| K01 | SSH: `yes` for 10 minutes, then Ctrl-C. | As above. |
| K02 | SSH: `head -c 1000000 /dev/urandom`, to the end (about 6 minutes). Then `reset`. | As above. The random bytes contain thousands of `0x1E`: none of them ends the session, shows the restart line or leaves stray answer text. The calculator does not hang on the escape sequences that random bytes make. |
| K03 | Serial with `make shell`: `yes` for 10 minutes, then Ctrl-C. Check `ps -o rss= -p <pid of shell.py>` at the start and the end. | As above. The output does not slow down over time, which would point to the window shrinking. `shell.py`'s memory stays about the same. |
| K04 | Serial with `make shell`: `head -c 1000000 /dev/urandom`, to the end. Then `reset`. | As above. The ESP32 stays in raw mode: no monitor text shows up as keys or on the screen. The output keeps its speed to the end. This shows that the `1E 'e'` escapes and the acknowledgements stay in step. |
| K05 | Serial with `make shell`: `seq 1 200000`, to the end. | As above. The last lines read `199999`, `200000`. |
| K06 | SSH: `yes`, and press 2nd+APPS five times, a few seconds apart, with `make screenshots` attached. | Five complete PNG files, each showing lines of `y`. The stream carries on after each one. |
| K07 | Both ways at once. In SSH run `cat`, then hold down a letter key for 60 s (auto-repeat), so that every key is echoed back while more are typed. | The session survives and `cat` is still echoing at the end. Ctrl-D ends it. Some keys may be lost to collisions: note roughly how many letters came back for 60 s of holding, for comparison between runs. |
| K08 | Serial with `make shell`: `seq 1 100000`. Open the CHAR strip (2nd + plus) for 20 s, then close it with any key. | The output carries on where it stopped, **without a gap**: the ESP32 stopped reading `make shell` while the calculator was not taking bytes. |
| K09 | SSH: the same as K08. | Known gap: SSH output is dropped while the calculator is not taking bytes, so numbers are missing after the strip closes. The session must carry on, without a banner or a stall. Note how many numbers were lost. |
| K10 | Serial with `make shell`: `head -c 1000000 /dev/urandom`. Pull the calculator's cable for 30 s half way through, then plug it back. | Banner while it is out. After plugging back the stream carries on at full speed to the end; the ESP32 is still in raw mode, and the output after `reset` and `echo ok` is clean. |
| K11 | Leave a quiet SSH session for 30 minutes (about 600 pings), then run `seq 1 3000`. | No banner or message in the 30 minutes. The output then runs as in L03. |
| K12 | Leave a Serial session with `make shell` running `while true; do date; sleep 60; done` for several hours, overnight if possible. | One date a minute the whole time, none missing. No banner or message. `shell.py`'s memory is about the same at the end as after the first minute. |

## Recording a run

Write down the date, the commit (from the menu's first line), the board,
and for each test either "ok" or what happened instead. A failing test is
worth a note even when the cause is clear.
