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

### Reading the answer out of the output

These inject fake answers from the shell, which shows that the calculator
takes them out wherever they fall. Run them in a Serial session or SSH.

| ID | Steps | Expected |
|---|---|---|
| L50 | `printf '\e[10\x1ew+fake status\n;20HX\n'` | An `X` at row 10, column 20. Nothing of `fake status` is shown. |
| L51 | `printf 'caf\xc3\x1ew+x\n\xa9\n'` | `café`: an answer in the middle of a UTF-8 character does not break it. |
| L52 | Wait 5 s, then `printf '\x1eyQQ\n'` (`QQ` is unlikely to be the ESP32's letters; if the restart line does not show, try `ZZ`). | The restart line appears. This shows that the check works, and also that a program on the host can fake it, which only affects the message. |
| L53 | `printf 'a\x1eqb\n'` | `ab`: a `1E` followed by a letter that is not an answer is dropped with the letter, as before. |

## Recording a run

Write down the date, the commit (from the menu's first line), the board,
and for each test either "ok" or what happened instead. A failing test is
worth a note even when the cause is clear.
