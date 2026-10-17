/*
 * Ssh.h - one SSH shell session over Wi-Fi, for the terminal bridge
 *
 * Wraps LibSSH-ESP32. The shell is asked for a 60 x 21 vt220 terminal, which
 * is what calc/term draws.
 *
 * Logging in uses the ESP32's own key if it has one and the host takes it.
 * Otherwise the password is asked for, at each connection, and never stored.
 * The key is an ed25519 pair made here and kept in flash, unencrypted.
 * Nothing puts it on a host except copyKeyToHost().
 *
 * A host's key is remembered the first time and checked from then on, for
 * each of the hosts in the settings separately. If it ever differs, the
 * connection is refused until forgetHostKey() is called.
 */
#pragma once
#include <Arduino.h>

namespace Ssh {

struct Target {
  String wifiName, wifiPassword;
  String host;  // name or address
  uint16_t port;
  String user;
  uint8_t slot;  // which of the stored hosts, from 1: names its remembered key
};

// Prints one line of progress or error.
typedef void (*SayFn)(const char *line);
// Asks the user for a line of text. Returns false if they gave up.
typedef bool (*AskFn)(const char *prompt, bool hidden, String &answer);

// Joins Wi-Fi if needed, connects, logs in and opens the shell. Blocks for
// as long as that takes, including the password prompt if there is one.
bool open(const Target &target, SayFn say, AskFn ask);

void close();

// Never blocks. Returns the bytes read, 0 for none, -1 once the far end has
// closed the session.
int read(uint8_t *buf, size_t size);

void write(const uint8_t *data, size_t len);

// Joins the network, dropping a connection to a different one.
bool joinWifi(const Target &target, SayFn say);

// If set, called about every 100 ms while joinWifi waits for the network.
extern void (*waiting)();

void forgetHostKey(uint8_t slot);

// The ESP32's own key pair.
bool hasKey();
bool makeKey(SayFn say);  // replaces the one there is
void forgetKey();
String keyFingerprint();  // "SHA256:...", empty without a key
String publicKeyLine();   // as it goes in authorized_keys, empty without a key

// Logs in with the password and appends the public key to the user's
// ~/.ssh/authorized_keys on the host, then disconnects. The caller is the
// one to ask the user whether that is wanted.
bool copyKeyToHost(const Target &target, SayFn say, AskFn ask);

}  // namespace Ssh
