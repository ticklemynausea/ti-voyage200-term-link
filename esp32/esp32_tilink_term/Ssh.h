/*
 * Ssh.h - one SSH shell session over Wi-Fi, for the terminal bridge
 *
 * Wraps LibSSH-ESP32. The shell is asked for a 60 x 21 vt220 terminal, which
 * is what calc/term draws. The login password is asked for at each
 * connection and never stored.
 *
 * The host's key is remembered the first time and checked from then on. If
 * it ever differs, the connection is refused until forgetHostKey() is called.
 */
#pragma once
#include <Arduino.h>

namespace Ssh {

struct Target {
  String wifiName, wifiPassword;
  String host;  // name or address
  uint16_t port;
  String user;
};

// Prints one line of progress or error.
typedef void (*SayFn)(const char *line);
// Asks the user for a line of text. Returns false if they gave up.
typedef bool (*AskFn)(const char *prompt, bool hidden, String &answer);

// Joins Wi-Fi if needed, connects, logs in and opens the shell. Blocks for
// as long as that takes, including the password prompt.
bool open(const Target &target, SayFn say, AskFn ask);

void close();

// Never blocks. Returns the bytes read, 0 for none, -1 once the far end has
// closed the session.
int read(uint8_t *buf, size_t size);

void write(const uint8_t *data, size_t len);

// Joins the network, dropping a connection to a different one.
bool joinWifi(const Target &target, SayFn say);

void forgetHostKey();

}  // namespace Ssh
