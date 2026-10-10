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
const int PASSWORD_TRIES = 3;

ssh_session session = nullptr;
ssh_channel channel = nullptr;
bool libraryStarted = false;

void sayf(Ssh::SayFn say, const char *format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  say(line);
}

// Trust on first use: the key's fingerprint is stored with the host's name
// and has to match on every later connection.
bool hostKeyAccepted(const String &host, Ssh::SayFn say) {
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
  String known = prefs.getString(PREFS_HOST_KEY, "");
  bool ok = true;
  if (known.length() == 0 || !known.startsWith(host + " ")) {
    prefs.putString(PREFS_HOST_KEY, seen);
    say("First connection to this host: its key is now remembered.");
  } else if (known != seen) {
    say("THE HOST'S KEY HAS CHANGED. Not connecting.");
    say("If that is expected, type /forgethost in the Serial Monitor.");
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
  close();
  if (!joinWifi(target, say)) return false;
  if (!libraryStarted) {
    libssh_begin();
    libraryStarted = true;
  }

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
  if (!hostKeyAccepted(target.host, say)) { close(); return false; }

  String prompt = target.user + "@" + target.host + "'s password: ";
  for (int attempt = 1;; attempt++) {
    String password;
    if (!ask(prompt.c_str(), true, password)) { close(); return false; }
    int rc = ssh_userauth_password(session, nullptr, password.c_str());
    if (rc == SSH_AUTH_SUCCESS) break;
    if (rc != SSH_AUTH_DENIED || attempt == PASSWORD_TRIES) return fail(say, "Login failed");
    say("Permission denied, try again.");
  }

  channel = ssh_channel_new(session);
  if (!channel || ssh_channel_open_session(channel) != SSH_OK) return fail(say, "Session failed");
  if (ssh_channel_request_pty_size(channel, "vt220", TERM_COLS, TERM_ROWS) != SSH_OK)
    return fail(say, "Terminal request failed");
  if (ssh_channel_request_shell(channel) != SSH_OK) return fail(say, "Shell request failed");
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

void Ssh::forgetHostKey() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_HOST_KEY);
  prefs.end();
}
