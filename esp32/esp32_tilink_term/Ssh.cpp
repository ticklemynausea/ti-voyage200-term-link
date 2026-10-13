/*
 * Ssh.cpp - see Ssh.h
 */
#include "Ssh.h"

#include <Preferences.h>
#include <WiFi.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

namespace {

const int TERM_COLS = 60;
const int TERM_ROWS = 21;
const uint32_t WIFI_TIMEOUT_MS = 20000;
const long SSH_TIMEOUT_S = 15;
const char PREFS_NAMESPACE[] = "tilink";
const char PREFS_HOST_KEY[] = "hostkey";
const char PREFS_USER_KEY[] = "sshkey";
const char KEY_COMMENT[] = "voyage200";
const int PASSWORD_TRIES = 3;

ssh_session session = nullptr;
ssh_channel channel = nullptr;
bool libraryStarted = false;

void startLibrary() {
  if (libraryStarted) return;
  libssh_begin();
  libraryStarted = true;
}

void sayf(Ssh::SayFn say, const char *format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  say(line);
}

String hostKeyName(uint8_t slot) {
  return String(PREFS_HOST_KEY) + slot;
}

// Trust on first use: the key's fingerprint is stored with the host's name
// and has to match on every later connection.
bool hostKeyAccepted(const String &host, uint8_t slot, Ssh::SayFn say) {
  ssh_key key = nullptr;
  unsigned char *hash = nullptr;
  size_t hashLen = 0;
  if (ssh_get_server_publickey(session, &key) != SSH_OK) return false;
  int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen);
  ssh_key_free(key);
  if (rc != SSH_OK) return false;

  char *hex = ssh_get_hexa(hash, hashLen);
  String seen = host + " " + hex;
  ssh_string_free_char(hex);
  ssh_clean_pubkey_hash(&hash);

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  String name = hostKeyName(slot);
  // Before there were several hosts the only one's key had no number.
  if (slot == 1 && !prefs.isKey(name.c_str()) && prefs.isKey(PREFS_HOST_KEY)) {
    prefs.putString(name.c_str(), prefs.getString(PREFS_HOST_KEY, ""));
    prefs.remove(PREFS_HOST_KEY);
  }
  String known = prefs.getString(name.c_str(), "");
  bool ok = true;
  if (known.length() == 0 || !known.startsWith(host + " ")) {
    prefs.putString(name.c_str(), seen);
    say("First connection to this host: its key is now remembered.");
  } else if (known != seen) {
    say("THE HOST'S KEY HAS CHANGED. Not connecting.");
    sayf(say, "If that is expected, type /forgethost %u in the Serial Monitor.", slot);
    ok = false;
  }
  prefs.end();
  return ok;
}

bool fail(Ssh::SayFn say, const char *what) {
  sayf(say, "%s: %s", what, session ? ssh_get_error(session) : "no session");
  Ssh::close();
  return false;
}

// Joins Wi-Fi, connects and checks the host's key. Leaves the session open
// and not logged in.
bool connect(const Ssh::Target &target, Ssh::SayFn say) {
  Ssh::close();
  if (!Ssh::joinWifi(target, say)) return false;
  startLibrary();

  sayf(say, "Connecting to %s@%s...", target.user.c_str(), target.host.c_str());
  session = ssh_new();
  if (!session) return fail(say, "SSH");
  unsigned int port = target.port;
  long timeout = SSH_TIMEOUT_S;
  ssh_options_set(session, SSH_OPTIONS_HOST, target.host.c_str());
  ssh_options_set(session, SSH_OPTIONS_PORT, &port);
  ssh_options_set(session, SSH_OPTIONS_USER, target.user.c_str());
  ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

  if (ssh_connect(session) != SSH_OK) return fail(say, "Connection failed");
  if (!hostKeyAccepted(target.host, target.slot, say)) { Ssh::close(); return false; }
  return true;
}

bool loginWithPassword(const Ssh::Target &target, Ssh::SayFn say, Ssh::AskFn ask) {
  String prompt = target.user + "@" + target.host + "'s password: ";
  for (int attempt = 1;; attempt++) {
    String password;
    if (!ask(prompt.c_str(), true, password)) { Ssh::close(); return false; }
    int rc = ssh_userauth_password(session, nullptr, password.c_str());
    if (rc == SSH_AUTH_SUCCESS) return true;
    if (rc != SSH_AUTH_DENIED || attempt == PASSWORD_TRIES) return fail(say, "Login failed");
    say("Permission denied, try again.");
  }
}

// The stored private key, or nullptr. The caller frees it.
ssh_key loadKey() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  String text = prefs.getString(PREFS_USER_KEY, "");
  prefs.end();
  if (!text.length()) return nullptr;
  startLibrary();
  ssh_key key = nullptr;
  if (ssh_pki_import_privkey_base64(text.c_str(), nullptr, nullptr, nullptr, &key) != SSH_OK) return nullptr;
  return key;
}

}  // namespace

bool Ssh::joinWifi(const Target &target, SayFn say) {
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == target.wifiName) return true;
  sayf(say, "Joining Wi-Fi %s...", target.wifiName.c_str());
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(target.wifiName.c_str(), target.wifiPassword.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_TIMEOUT_MS) {
      say("Wi-Fi: no connection. Check the name and password in Settings.");
      WiFi.disconnect(true);
      return false;
    }
    delay(100);
  }
  sayf(say, "Wi-Fi up, address %s", WiFi.localIP().toString().c_str());
  return true;
}

bool Ssh::open(const Target &target, SayFn say, AskFn ask) {
  if (!connect(target, say)) return false;

  bool loggedIn = false;
  ssh_key key = loadKey();
  if (key) {
    loggedIn = ssh_userauth_publickey(session, nullptr, key) == SSH_AUTH_SUCCESS;
    ssh_key_free(key);
    if (!loggedIn) say("The host did not take the key (Settings, F4 copies it).");
  }
  if (!loggedIn && !loginWithPassword(target, say, ask)) return false;

  channel = ssh_channel_new(session);
  if (!channel || ssh_channel_open_session(channel) != SSH_OK) return fail(say, "Session failed");
  if (ssh_channel_request_pty_size(channel, "vt220", TERM_COLS, TERM_ROWS) != SSH_OK)
    return fail(say, "Terminal request failed");
  if (ssh_channel_request_shell(channel) != SSH_OK) return fail(say, "Shell request failed");
  return true;
}

bool Ssh::hasKey() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  bool has = prefs.isKey(PREFS_USER_KEY);
  prefs.end();
  return has;
}

bool Ssh::makeKey(SayFn say) {
  startLibrary();
  ssh_key key = nullptr;
  char *text = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
    say("Could not make a key.");
    return false;
  }
  int rc = ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &text);
  ssh_key_free(key);
  if (rc != SSH_OK || !text) {
    say("Could not store the key.");
    return false;
  }
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  bool stored = prefs.putString(PREFS_USER_KEY, text) > 0;
  prefs.end();
  ssh_string_free_char(text);
  if (!stored) say("Could not store the key.");
  return stored;
}

void Ssh::forgetKey() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_USER_KEY);
  prefs.end();
}

String Ssh::keyFingerprint() {
  String result;
  ssh_key key = loadKey();
  if (!key) return result;
  unsigned char *hash = nullptr;
  size_t hashLen = 0;
  if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen) == SSH_OK) {
    char *text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLen);
    if (text) result = text;
    ssh_string_free_char(text);
    ssh_clean_pubkey_hash(&hash);
  }
  ssh_key_free(key);
  return result;
}

String Ssh::publicKeyLine() {
  String result;
  ssh_key key = loadKey();
  if (!key) return result;
  char *text = nullptr;
  if (ssh_pki_export_pubkey_base64(key, &text) == SSH_OK && text) {
    result = String(ssh_key_type_to_char(ssh_key_type(key))) + " " + text + " " + KEY_COMMENT;
    ssh_string_free_char(text);
  }
  ssh_key_free(key);
  return result;
}

bool Ssh::copyKeyToHost(const Target &target, SayFn say, AskFn ask) {
  String keyLine = publicKeyLine();
  if (!keyLine.length()) {
    say("There is no key to copy.");
    return false;
  }
  if (!connect(target, say)) return false;
  if (!loginWithPassword(target, say, ask)) return false;

  // The key line is base64 and a fixed comment: nothing in it needs quoting.
  String command = "umask 077; mkdir -p ~/.ssh && echo '" + keyLine + "' >> ~/.ssh/authorized_keys";
  ssh_channel exec = ssh_channel_new(session);
  if (!exec || ssh_channel_open_session(exec) != SSH_OK ||
      ssh_channel_request_exec(exec, command.c_str()) != SSH_OK) {
    if (exec) ssh_channel_free(exec);
    return fail(say, "Could not run the command");
  }
  // Read to the end, so that the exit status has arrived.
  char buf[64];
  for (int isStderr = 0; isStderr < 2; isStderr++) {
    int n;
    while ((n = ssh_channel_read_timeout(exec, buf, sizeof(buf) - 1, isStderr, SSH_TIMEOUT_S * 1000)) > 0) {
      if (!isStderr) continue;
      buf[n] = 0;
      for (char *line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) say(line);
    }
  }
  uint32_t status = 1;
  int rc = ssh_channel_get_exit_state(exec, &status, nullptr, nullptr);
  ssh_channel_close(exec);
  ssh_channel_free(exec);
  close();
  if (rc != SSH_OK || status != 0) {
    sayf(say, "The host's command failed (status %lu).", (unsigned long)status);
    return false;
  }
  return true;
}

void Ssh::close() {
  if (channel) {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    channel = nullptr;
  }
  if (session) {
    ssh_disconnect(session);
    ssh_free(session);
    session = nullptr;
  }
}

int Ssh::read(uint8_t *buf, size_t size) {
  if (!channel) return -1;
  int n = ssh_channel_read_nonblocking(channel, buf, size, 0);
  if (n > 0) return n;
  if (n < 0 || ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) return -1;
  return 0;
}

void Ssh::write(const uint8_t *data, size_t len) {
  if (channel) ssh_channel_write(channel, data, len);
}

void Ssh::forgetHostKey(uint8_t slot) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(hostKeyName(slot).c_str());
  if (slot == 1) prefs.remove(PREFS_HOST_KEY);  // from before there were several hosts
  prefs.end();
}
