#!/usr/bin/env python3
"""Runs a shell on this computer with the calculator as its terminal.

The ESP32 (esp32_tilink_term) is put in raw mode and its serial port is
joined to a pseudo-terminal of 60 x 21 characters with TERM=vt100. Start
term() on the calculator first.

    tools/shell.py                     the login shell
    tools/shell.py top                 any other command
    tools/shell.py --port /dev/cu.xxx  when the port is not found by itself
    tools/shell.py --mirror            also copy the output to this window

Ends when the command exits, or with Ctrl-C here.
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

COLS, ROWS = 60, 21
CONTROL = b"\x1e\x1e\x1e"
MAX_QUEUE = 4096  # bytes waiting for the ESP32 before the shell is paused


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
    parser.add_argument("--mirror", action="store_true", help="copy the output to this window")
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
        os.environ.update(TERM="vt100", LANG="C", LC_ALL="C")
        for name in ("COLUMNS", "LINES", "TERM_PROGRAM", "ITERM_SESSION_ID"):
            os.environ.pop(name, None)
        try:
            os.execvp(command[0], command)
        except OSError as err:
            os.write(2, f"{command[0]}: {err.strerror}\r\n".encode())
            os._exit(127)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    os.kill(pid, signal.SIGWINCH)

    print(f"{' '.join(command)} is running on the calculator via {port}. Ctrl-C here to stop.")
    to_calc = b""
    try:
        while True:
            readers = [serial] + ([master] if len(to_calc) < MAX_QUEUE else [])
            readable, writable, _ = select.select(readers, [serial] if to_calc else [], [])
            if serial in readable:
                write_all(master, os.read(serial, 1024))
            if master in readable:
                try:
                    data = os.read(master, 1024)
                except OSError:  # the command exited
                    break
                if not data:
                    break
                to_calc += data
                if args.mirror:
                    os.write(1, data)
            if serial in writable:
                to_calc = to_calc[os.write(serial, to_calc):]
    except KeyboardInterrupt:
        pass
    finally:
        try:
            os.kill(pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        write_all(serial, b"\r\n[session ended]\r\n" + CONTROL + b"monitor\n")
        os.close(serial)
    print("\nSession ended.")


if __name__ == "__main__":
    main()
