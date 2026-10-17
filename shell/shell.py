#!/usr/bin/env python3
"""Runs a shell on this computer with the calculator as its terminal.

The ESP32 (esp32_tilink_term) is put in raw mode and its serial port is
joined to a pseudo-terminal of 60 x 21 characters with TERM=vt220. Start
term() on the calculator first.

    shell/shell.py                     the login shell
    shell/shell.py top                 any other command
    shell/shell.py --port /dev/cu.xxx  when the port is not found by itself
    shell/shell.py --mirror            share the session with this window
    shell/shell.py --no-shell          run nothing, only save screenshots

With --mirror the output is also copied to this window, and keys typed here
go to the same shell, alongside the calculator's.

2nd + APPS on the calculator sends its screen, from the menus and from any
session. While this script is attached it is saved as a PNG in screenshots/
(--screenshots DIR), each pixel drawn --scale times as large (default 4).

Ends when the command exits, or with Ctrl-C here (Ctrl-] with --mirror, where
Ctrl-C goes to the shell). If the ESP32's port goes away (unplugged, or the
ESP32 lost power) the command keeps running, paused once its output piles
up, and the port is opened again when it comes back.
"""
import argparse
import fcntl
import glob
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time
import tty
import zlib

COLS, ROWS = 60, 21
MARK = 0x1E
CONTROL = bytes([MARK] * 3)
MAX_QUEUE = 512  # bytes waiting for the ESP32 before the command is paused
QUIT_KEY = 0x1D  # Ctrl-], as in telnet: Ctrl-C has to reach the shell

# The link is far slower than USB and the ESP32 drops serial bytes it has no
# room for. So at most WINDOW bytes are outstanding; the ESP32 answers every
# ACK_EVERY bytes it has dealt with by sending MARK 'a'. A real MARK byte from
# the calculator arrives doubled. A real MARK byte in the command's output is
# sent as MARK 'e', so that three marks in a row only ever start a control
# line; it counts as one byte of the window.
WINDOW = 128
ACK_EVERY = 32

# In a quiet Serial session the ESP32 asks MARK 'k', are you there?, and is
# answered the same. Without an answer it tells the calculator.

# How often a port that went away is looked for.
REOPEN_WAIT = 1.0

# A screenshot arrives as MARK 'p' and the calculator's screen: one bit per
# pixel, a set bit being a dark one, the leftmost pixel in the top bit.
SCREEN_W, SCREEN_H = 240, 128
SCREEN_BYTES = SCREEN_W * SCREEN_H // 8

# In monitor mode the ESP32 prints text for a person, and here that text would
# be taken for keys typed on the calculator. So nothing counts as a key until
# the ESP32 has answered "raw" with MARK 'r'. It sends MARK 'b' when it
# starts, and its boot loader prints BOOT_TEXT before that: after either,
# raw mode is asked for again. SYNC_WAIT is how long an answer may take.
BOOT_TEXT = b"ESP-ROM:"
SYNC_WAIT = 1.0
HELD_WAIT = 0.02  # how long the start of what may be BOOT_TEXT is kept back

NO_SHELL_NOTICE = b"[no shell is running on the computer: it only saves\r\nscreenshots. \"make shell\" runs one. 2nd+ESC for the menu]\r\n"


# Programs ask the terminal questions (what are you? how big is your window?)
# and read the answer as if it were typed. The calculator never answers. This
# window would, with its own size, and tmux then lays its screen out for this
# window instead of the calculator's 60 x 21. So the questions are left out
# of what is copied here. Window commands (CSI ... t) go the same way: they
# would resize or move this window.
QUESTIONS = re.compile(
    rb"\x1b\[[?>=]?[0-9;]*(?:[cntx]|\$p)"          # device, status, window, mode reports
    rb"|\x1b\[>[0-9;]*q|\x1b\[\?u"                 # terminal version, keyboard mode
    rb"|\x1b\][0-9;]+\?(?:\x07|\x1b\\)"            # colours
    rb"|\x1bP[+$]q[^\x1b]*\x1b\\"                  # settings and capabilities
    rb"|\x05")                                     # answerback
# The start of what could become one of them, cut off at the end of a read.
UNFINISHED = re.compile(rb"\x1b(?:\[[?>=]?[0-9;]*\$?|\][0-9;]*\??\x1b?|P(?:[+$](?:q[^\x1b]*\x1b?)?)?)?$")


class Mirror:
    """Copies the command's output to this window, minus the questions."""

    def __init__(self):
        self.held = b""

    def write(self, data):
        data = QUESTIONS.sub(b"", self.held + data)
        cut = UNFINISHED.search(data)
        self.held = data[cut.start():] if cut else b""
        if cut:
            data = data[:cut.start()]
        if data:
            os.write(1, data)


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def screen_to_png(screen, scale):
    """The calculator's screen as a greyscale PNG, black on white."""
    rows = bytearray()
    for y in range(SCREEN_H):
        line = screen[y * SCREEN_W // 8:(y + 1) * SCREEN_W // 8]
        pixels = b"".join((b"\x00" if byte & (0x80 >> bit) else b"\xff") * scale
                          for byte in line for bit in range(8))
        rows += (b"\x00" + pixels) * scale  # each row starts with its filter type
    header = struct.pack(">IIBBBBB", SCREEN_W * scale, SCREEN_H * scale, 8, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + png_chunk(b"IEND", b""))


def save_screenshot(screen, folder, scale):
    os.makedirs(folder, exist_ok=True)
    stem = os.path.join(folder, time.strftime("voyage200-%Y%m%d-%H%M%S"))
    path, n = stem + ".png", 1
    while os.path.exists(path):
        n += 1
        path = f"{stem}-{n}.png"
    with open(path, "wb") as f:
        f.write(screen_to_png(screen, scale))
    return path


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # Nobody else may open the port meanwhile: a second copy of this script
    # would take part of the bytes, and switch the ESP32's mode when it ends.
    fcntl.ioctl(fd, termios.TIOCEXCL)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0  # no input, output or line processing
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def write_all(fd, data):
    while data:
        select.select([], [fd], [])
        data = data[os.write(fd, data):]


def escape(data):
    return data.replace(bytes([MARK]), bytes([MARK, ord("e")]))


class PortLost(Exception):
    """The ESP32's port went away."""


def read_serial(fd):
    try:
        data = os.read(fd, 1024)
    except BlockingIOError:
        return b""
    except OSError as err:
        raise PortLost from err
    if not data:
        raise PortLost
    return data


def write_serial(fd, data):
    try:
        write_all(fd, data)
    except BlockingIOError:
        raise
    except OSError as err:
        raise PortLost from err


def notice(text):
    """A line in this window; \r\n since it may be in raw mode for --mirror."""
    os.write(2, f"{text}\r\n".encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", help="ESP32 serial port (default: first /dev/cu.usbmodem*)")
    parser.add_argument("--mirror", action="store_true", help="show the output in this window and send its keys too")
    parser.add_argument("--no-shell", action="store_true", help="run no command: only save screenshots")
    parser.add_argument("--screenshots", default="screenshots", metavar="DIR", help="where screenshots are saved")
    parser.add_argument("--scale", type=int, default=4, help="size of a calculator pixel in the screenshots")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run (default: login shell)")
    args = parser.parse_args()

    def find_port():
        return args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)

    port = find_port()
    if not port:
        sys.exit("No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass --port.")
    if args.no_shell and (args.command or args.mirror):
        sys.exit("--no-shell goes without a command and without --mirror.")
    if args.scale < 1:
        sys.exit("--scale is 1 or more.")
    command = args.command or [os.environ.get("SHELL", "/bin/sh"), "-l"]

    try:
        serial = open_serial(port)
    except OSError as err:
        sys.exit(f"{port}: {err.strerror}. Is another make shell, make screenshots or Serial Monitor using it?")
    write_all(serial, CONTROL + b"raw\n")
    write_all(serial, b"\x1bc")  # reset the calculator's terminal

    pid, master = (None, None) if args.no_shell else pty.fork()
    if pid == 0:
        os.environ.update(TERM="vt220", LANG="en_US.UTF-8")
        for name in ("LC_ALL", "COLUMNS", "LINES", "TERM_PROGRAM", "ITERM_SESSION_ID"):
            os.environ.pop(name, None)
        try:
            os.execvp(command[0], command)
        except OSError as err:
            os.write(2, f"{command[0]}: {err.strerror}\r\n".encode())
            os._exit(127)
    if pid:
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        os.kill(pid, signal.SIGWINCH)

    # With --mirror this window's keyboard joins in. It is switched to raw
    # mode so that every key, Ctrl-C included, goes to the shell as typed.
    keyboard = sys.stdin.fileno() if args.mirror and sys.stdin.isatty() else None
    stop_key = "Ctrl-]" if keyboard is not None else "Ctrl-C"
    if pid:
        print(f"{' '.join(command)} is running on the calculator via {port}. {stop_key} here to stop.")
    else:
        print(f"Attached to {port}, running nothing. {stop_key} here to stop.")
    print(f"2nd + APPS on the calculator saves its screen in {args.screenshots}/.")
    if keyboard is not None:
        keyboard_attrs = termios.tcgetattr(keyboard)
        tty.setraw(keyboard)
    to_calc = b""
    mirror = Mirror()
    in_flight = 2  # the reset sequence sent above
    after_mark = False
    screen = None  # the bytes of a screenshot that is arriving
    synced = False  # the ESP32 has said that it is in raw mode
    assume_at = time.monotonic() + SYNC_WAIT  # an ESP32 already in raw mode may not answer
    ask_again_at = None  # after a restart of the ESP32: when to ask for raw mode again
    held = b""
    out = b""  # escaped bytes on their way to the ESP32, and answers to MARK 'k'
    reopen_at = None  # while the port is gone: when to look for it again
    exited = False  # the command is gone; what is queued still has to go out
    try:
        # Once the command is gone, nothing more is waited for from a port that went away.
        while not (exited and (serial is None or not (to_calc or out))):
            readers = [serial] if serial is not None else []
            if pid and not exited and len(to_calc) < MAX_QUEUE:
                readers.append(master)
            if keyboard is not None and not exited:
                readers.append(keyboard)
            may_send = serial is not None and (out or (synced and to_calc and in_flight < WINDOW))
            wake_at = reopen_at or assume_at or ask_again_at
            wait = max(0, wake_at - time.monotonic()) if wake_at else HELD_WAIT if held else None
            readable, writable, _ = select.select(readers, [serial] if may_send else [], [], wait)

            lost = False
            keys = bytearray()
            try:
                data = b""
                if serial in readable:
                    data = held + read_serial(serial)
                    held = b""
                elif not readable and not writable:  # the wait ran out
                    if reopen_at:
                        port = find_port() or port
                        try:
                            serial = open_serial(port)
                        except OSError:
                            reopen_at = time.monotonic() + REOPEN_WAIT
                        else:
                            notice(f"{port} is back.")
                            reopen_at = None
                            write_serial(serial, CONTROL + b"raw\n")
                            assume_at = time.monotonic() + SYNC_WAIT
                    elif assume_at:
                        synced, assume_at = True, None
                    elif ask_again_at:
                        write_serial(serial, CONTROL + b"raw\n")
                        ask_again_at = time.monotonic() + SYNC_WAIT
                    else:
                        data, held = held, b""  # it was not BOOT_TEXT after all
                if data and synced and screen is None:
                    at = data.find(BOOT_TEXT)
                    if at >= 0:  # the ESP32 restarted; MARK 'b' follows its boot messages
                        data = data[:at]
                        synced, ask_again_at = False, time.monotonic() + 3 * SYNC_WAIT
                    elif serial in readable:
                        for n in range(len(BOOT_TEXT) - 1, 0, -1):
                            if data.endswith(BOOT_TEXT[:n]):
                                data, held = data[:-n], data[-n:]
                                break

                for byte in data:
                    if screen is not None:
                        screen.append(byte)
                        if len(screen) == SCREEN_BYTES:
                            path = save_screenshot(screen, args.screenshots, args.scale)
                            screen = None
                            if keyboard is None:  # a mirrored session owns this window
                                print(f"Saved {path}")
                    elif after_mark:
                        after_mark = False
                        if byte == ord("r"):
                            if not synced:
                                in_flight = 0
                            synced, assume_at, ask_again_at = True, None, None
                        elif byte == ord("b"):
                            write_serial(serial, CONTROL + b"raw\n")
                            synced, assume_at, ask_again_at = False, None, time.monotonic() + SYNC_WAIT
                        elif byte == ord("k"):
                            out += bytes([MARK, ord("k")])
                        elif not synced:
                            pass
                        elif byte == MARK:
                            keys.append(byte)
                        elif byte == ord("a"):
                            in_flight = max(0, in_flight - ACK_EVERY)
                        elif byte == ord("p"):
                            screen = bytearray()
                    elif byte == MARK:
                        after_mark = True
                    elif synced:
                        keys.append(byte)
            except PortLost:
                lost = True
            if keys and pid and not exited:
                write_all(master, bytes(keys))
            elif not pid and 0x0C in keys:  # what a Serial session starts with
                to_calc += NO_SHELL_NOTICE
            if keyboard in readable:
                typed = os.read(keyboard, 1024)
                if not typed or QUIT_KEY in typed:
                    break
                write_all(master, typed)
            if master in readable:
                try:
                    data = os.read(master, 1024)
                except OSError:  # the command exited
                    data = b""
                exited = not data
                to_calc += data
                if args.mirror:
                    mirror.write(data)
            if serial in writable and not lost:
                # The window counts the command's bytes; they are escaped
                # only on the way out, a window's worth at a time.
                if not out:
                    room = WINDOW - in_flight
                    chunk, to_calc = to_calc[:room], to_calc[room:]
                    in_flight += len(chunk)
                    out = escape(chunk)
                try:
                    out = out[os.write(serial, out):]
                except BlockingIOError:
                    pass
                except OSError:
                    lost = True

            if lost:
                # What was on its way is dropped: at most a window's worth.
                # The command keeps running, and is paused once MAX_QUEUE
                # bytes of its output wait.
                try:
                    os.close(serial)
                except OSError:
                    pass
                serial, out, in_flight, held = None, b"", 0, b""
                synced, assume_at, ask_again_at = False, None, None
                after_mark, screen = False, None
                reopen_at = time.monotonic() + REOPEN_WAIT
                notice(f"{port} went away. Waiting for it to come back; {stop_key} here to stop.")
    except KeyboardInterrupt:
        pass
    finally:
        if keyboard is not None:
            termios.tcsetattr(keyboard, termios.TCSADRAIN, keyboard_attrs)
        try:
            if pid:
                os.kill(pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        if serial is not None:
            try:
                write_all(serial, b"\r\n[session ended]\r\n" + CONTROL + b"monitor\n")
                os.close(serial)
            except OSError:
                pass
    print("\nSession ended.")


if __name__ == "__main__":
    main()
