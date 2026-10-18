# Several windows: tmux or screen

`make shell` runs whatever is given as `CMD` in place of the shell. The
`shell/` folder has settings that make tmux and screen fit the calculator's
60 x 21 screen without colours:

```sh
make shell CMD="tmux -L calc -f shell/tmux_calc.conf new -A -s calc"
make shell CMD="screen -c shell/screenrc_vt100"
```

Add `MIRROR=1` to see and type in the session on the computer too.

- **tmux** shows a bar of windows, each named after the program running in
  it. `-L calc` gives it a server of its own, so your usual tmux and its
  settings are not involved. Running the command again reattaches to the
  same session. The prefix is TAN, or ◆ + B. The calculator's function keys
  do the common steps in one: SIN and COS go to the previous and the next
  window; 2nd + SIN lists the windows, 2nd + COS goes back to the last one
  and 2nd + TAN opens a new one; ◆ + SIN scrolls a page back in copy mode
  (`q` leaves it), ◆ + TAN starts copy mode and ◆ + COS pastes.
- **screen** is given a terminal type without colours, so programs such as
  `irssi` mark their bars with reverse video. The file also holds a status
  bar line, commented out. The prefix is ◆ + A.

Over SSH, the same two files work on the remote host: copy them there and
start tmux or screen with the same options.

---

[Project overview](../README.md) · [Getting started](getting-started.md) · [The terminal](terminal.md) · [tmux and screen](tmux-and-screen.md) · [Hardware](hardware.md) · [Link partner](link-partner.md) · [Repository](repository.md)
