#!/usr/bin/env python3
"""Runs a shell on this computer with the calculator as its terminal.

The ESP32 (esp32_tilink_term) is put in raw mode and its serial port is
joined to a pseudo-terminal of 60 x 21 characters with TERM=vt220. Start
term() on the calculator first.

    tools/shell.py                     the login shell
    tools/shell.py top                 any other command
    tools/shell.py --port /dev/cu.xxx  when the port is not found by itself
    tools/shell.py --mirror            share the session with this window

With --mirror the output is also copied to this window, and keys typed here
go to the same shell, alongside the calculator's.

Ends when the command exits, or with Ctrl-C here (Ctrl-] with --mirror, where
Ctrl-C goes to the shell).
"""
import argparse
import fcntl
import glob
import os
import pty
import select
import signal
import struct
import sys
import termios
import tty

COLS, ROWS = 60, 21
MARK = 0x1E
CONTROL = bytes([MARK] * 3)
MAX_QUEUE = 512  # bytes waiting for the ESP32 before the command is paused
QUIT_KEY = 0x1D  # Ctrl-], as in telnet: Ctrl-C has to reach the shell

# The link is far slower than USB and the ESP32 drops serial bytes it has no
# room for. So at most WINDOW bytes are outstanding; the ESP32 answers every
# ACK_EVERY bytes it has dealt with by sending MARK 'a'. A real MARK byte from
# the calculator arrives doubled.
WINDOW = 128
ACK_EVERY = 32


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", help="ESP32 serial port (default: first /dev/cu.usbmodem*)")
    parser.add_argument("--mirror", action="store_true", help="show the output in this window and send its keys too")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run (default: login shell)")
    args = parser.parse_args()

    port = args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        sys.exit("No ESP32 found on /dev/cu.usbmodem*. Plug it in or pass --port.")
    command = args.command or [os.environ.get("SHELL", "/bin/sh"), "-l"]

    serial = open_serial(port)
    write_all(serial, CONTROL + b"raw\n")
    write_all(serial, b"\x1bc")  # reset the calculator's terminal

    pid, master = pty.fork()
    if pid == 0:
        os.environ.update(TERM="vt220", LANG="en_US.UTF-8")
        for name in ("LC_ALL", "COLUMNS", "LINES", "TERM_PROGRAM", "ITERM_SESSION_ID"):
            os.environ.pop(name, None)
        try:
            os.execvp(command[0], command)
        except OSError as err:
            os.write(2, f"{command[0]}: {err.strerror}\r\n".encode())
            os._exit(127)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    os.kill(pid, signal.SIGWINCH)

    # With --mirror this window's keyboard joins in. It is switched to raw
    # mode so that every key, Ctrl-C included, goes to the shell as typed.
    keyboard = sys.stdin.fileno() if args.mirror and sys.stdin.isatty() else None
    stop_key = "Ctrl-]" if keyboard is not None else "Ctrl-C"
    print(f"{' '.join(command)} is running on the calculator via {port}. {stop_key} here to stop.")
    if keyboard is not None:
        keyboard_attrs = termios.tcgetattr(keyboard)
        tty.setraw(keyboard)
    to_calc = b""
    in_flight = 2  # the reset sequence sent above
    after_mark = False
    exited = False  # the command is gone; what is queued still has to go out
    try:
        while not (exited and not to_calc):
            readers = [serial] + ([master] if not exited and len(to_calc) < MAX_QUEUE else [])
            if keyboard is not None and not exited:
                readers.append(keyboard)
            may_send = to_calc and in_flight < WINDOW
            readable, writable, _ = select.select(readers, [serial] if may_send else [], [])
            if serial in readable:
                keys = bytearray()
                for byte in os.read(serial, 1024):
                    if after_mark:
                        after_mark = False
                        if byte == MARK:
                            keys.append(byte)
                        elif byte == ord("a"):
                            in_flight = max(0, in_flight - ACK_EVERY)
                    elif byte == MARK:
                        after_mark = True
                    else:
                        keys.append(byte)
                if keys and not exited:
                    write_all(master, bytes(keys))
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
                    os.write(1, data)
            if serial in writable:
                sent = os.write(serial, to_calc[:WINDOW - in_flight])
                to_calc = to_calc[sent:]
                in_flight += sent
    except KeyboardInterrupt:
        pass
    finally:
        if keyboard is not None:
            termios.tcsetattr(keyboard, termios.TCSADRAIN, keyboard_attrs)
        try:
            os.kill(pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        write_all(serial, b"\r\n[session ended]\r\n" + CONTROL + b"monitor\n")
        os.close(serial)
    print("\nSession ended.")


if __name__ == "__main__":
    main()
