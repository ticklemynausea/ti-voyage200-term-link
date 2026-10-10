/*
 * esp32_tilink_term.ino - carries the calculator terminal's bytes
 *
 * The calculator runs calc/term (a dumb terminal). This sketch carries raw
 * bytes between it and a backend, on top of TiLink's bit handshake. There are
 * no packets: TI's protocol is not involved.
 *
 * The calculator picks the backend from its menu, with 0x1E and a letter:
 *   's'  Serial: the USB serial port, described below
 *   'h'  SSH: a shell on the host in the settings, over Wi-Fi (see Ssh.h)
 *   'c'  Settings: asks on the calculator for the Wi-Fi network and the SSH
 *        host and user, and stores them in the ESP32's flash (Preferences)
 *   'x'  the session is over
 *   'w'  asks for the Wi-Fi status, for the menu's status bar. The answer is
 *        0x1E 'w', a line of text and a newline. It leaves the backend alone.
 * A real 0x1E byte from the calculator arrives doubled.
 *
 * Serial has two modes:
 *   monitor (at power-up)  for the Serial Monitor. A typed line goes to the
 *                          calculator followed by CR LF. Lines starting with
 *                          '/' are commands. Bytes from the calculator are
 *                          printed, control codes as \xNN.
 *   raw (after /raw)       every byte passes through untouched, both ways,
 *                          and nothing else is printed. Reset to leave it.
 *
 * A program on the host (tools/shell.py) switches modes with a control
 * line that works in either mode: three 0x1E bytes, then "raw" or "monitor",
 * then a newline.
 *
 * Raw mode entered that way is paced. The link is far slower than USB, and
 * the ESP32 drops serial bytes it has no room for, so the host may only have
 * a small window of bytes outstanding. For every ACK_EVERY bytes passed on
 * (or given up on) the sketch sends 0x1E 'a'. A real 0x1E from the calculator
 * is sent twice.
 *
 * Serial Monitor: 115200 baud, newline line ending.
 * Needs TiLink.h / TiLink.cpp and Ssh.h / Ssh.cpp in the same folder, and the
 * LibSSH-ESP32 library.
 */
#include <Preferences.h>
#include <WiFi.h>
#include "Ssh.h"
#include "TiLink.h"

// libssh needs far more stack than the default 8 KB.
SET_LOOP_TASK_STACK_SIZE(48 * 1024);

// The pins follow the board the sketch is built for.
#if defined(ARDUINO_M5STACK_CARDPUTER)
// M5Stack Cardputer: the two signal pins of its Grove port.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 2;
#else
// M5Stack Stamp S3. A classic ESP32 would use 25 / 26.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 5;
#endif

// How long a byte may wait for the calculator to start taking it.
const uint32_t SEND_PATIENCE_MS = 5000;

const char HELP[] =
  "Commands: /stats | /burst [bytes] | /raw | /settings | /forgethost | /help   (anything else is sent)";

// Internal pull-ups on: with the calculator unplugged the lines then read as
// idle instead of floating, which would otherwise arrive as garbage bytes.
TiLink ti(PIN_TIP, PIN_RING, true);

bool rawMode = false;
bool linkDown = false;  // both lines held low: calculator off or half plugged
uint32_t toCalc = 0, fromCalc = 0, dropped = 0, retries = 0;
String line;

// Control lines from the host: CONTROL_MARK three times, a word, a newline.
const uint8_t CONTROL_MARK = 0x1E;
uint8_t marksSeen = 0;
bool inControlLine = false;
String controlLine;

// Where the calculator's session goes. Set by the calculator's commands,
// which are queued while bytes are being moved and run from loop().
enum Backend : uint8_t { BACKEND_NONE, BACKEND_SERIAL, BACKEND_SSH };
Backend backend = BACKEND_NONE;
bool markFromCalc = false;
char pendingCommands[8];
uint8_t pendingCount = 0;

// While a question is being asked on the calculator, its keys are kept here
// instead of going to a backend.
bool asking = false;
uint8_t typed[32];
uint8_t typedCount = 0;

// Kept in flash under these names. The SSH password is never stored.
const char PREFS_NAMESPACE[] = "tilink";
Ssh::Target target;

// Pacing of raw mode, see the top of the file.
const uint8_t ACK_EVERY = 32;
bool paced = false;
uint8_t unacked = 0;

// ---------- calculator -> backend ----------

void forwardFromCalc(uint8_t b) {
  fromCalc++;
  if (asking) {
    if (typedCount < sizeof(typed)) typed[typedCount++] = b;
  } else if (backend == BACKEND_SSH) {
    Ssh::write(&b, 1);
  } else if (rawMode) {
    if (paced && b == CONTROL_MARK) Serial.write(b);
    Serial.write(b);
  }
  else if (b == '\r') Serial.println();
  else if (b >= 32 && b < 127) Serial.write(b);
  else Serial.printf("\\x%02X", b);
}

void reportLinkError(const char *what) {
  if (rawMode) return;
  Serial.printf("\n! %s at bit %u: %s (tip=%u ring=%u)\n", what, ti.lastErrBit,
                ti.lastErrText(), ti.lastErrTip, ti.lastErrRing);
}

// Takes everything the calculator is sending right now.
void pumpFromCalc() {
  if (linkDown) {
    if (!ti.lineIdle()) return;
    linkDown = false;
    if (!rawMode) Serial.println("Link is back.");
  }
  for (;;) {
    int b = ti.recvByte(0);
    if (b == -1) return;
    if (b < 0) {
      if (ti.lastErrStage == 2 && ti.lastErrBit == 0) {
        linkDown = true;
        if (!rawMode) Serial.println("\n! Both lines are low: is the calculator on and plugged in?");
      } else {
        reportLinkError("receive error");
      }
      return;
    }
    if (markFromCalc) {
      markFromCalc = false;
      if (b == CONTROL_MARK) forwardFromCalc((uint8_t)b);
      else if (pendingCount < sizeof(pendingCommands)) pendingCommands[pendingCount++] = (char)b;
    } else if (b == CONTROL_MARK) {
      markFromCalc = true;
    } else {
      forwardFromCalc((uint8_t)b);
    }
  }
}

// ---------- backend -> calculator ----------

// The link is half duplex, so the calculator's bytes are taken first. If the
// calculator does not start taking ours (busy drawing, or it began a byte of
// its own at the same moment) the byte is retried, which is safe as long as
// no bit of it went through.
bool sendToCalc(uint8_t b) {
  uint32_t t0 = millis();
  for (;;) {
    pumpFromCalc();
    if (ti.sendByte(b)) { toCalc++; return true; }
    bool nothingSent = ti.lastErrBit == 0 && ti.lastErrStage != 7;
    if (!nothingSent || millis() - t0 > SEND_PATIENCE_MS) break;
    retries++;
  }
  dropped++;
  reportLinkError("send error, byte dropped");
  return false;
}

// Gives up at the first dropped byte, so a dead link costs one wait, not one
// per character.
bool sendText(const char *s) {
  while (*s) if (!sendToCalc((uint8_t)*s++)) return false;
  return true;
}

// ---------- the calculator's commands ----------

// A status line for the calculator's screen, copied to the Serial Monitor.
void sayToCalc(const char *text) {
  if (!rawMode) Serial.println(text);
  if (sendText(text)) sendText("\r\n");
}

// Asks a question on the calculator and reads the answer, with backspace.
// A hidden answer is echoed as stars. Returns false if the user left the
// session meanwhile (the calculator then sends a command).
bool askOnCalc(const char *prompt, bool hidden, String &answer) {
  const size_t MAX_ANSWER = 64;
  bool inEscape = false;  // arrow keys and such arrive as escape sequences
  answer = "";
  asking = true;
  typedCount = 0;
  sendText(prompt);

  for (;;) {
    pumpFromCalc();
    if (pendingCount) { asking = false; return false; }
    if (!typedCount) { delay(1); continue; }

    uint8_t b = typed[0];
    typedCount--;
    memmove(typed, typed + 1, typedCount);

    if (inEscape) {
      if (b >= 0x40 && b != '[' && b != 'O') inEscape = false;
    } else if (b == 0x1B) {
      inEscape = true;
    } else if (b == '\r') {
      break;
    } else if (b == 0x7F || b == 0x08) {
      if (answer.length()) {
        answer.remove(answer.length() - 1);
        sendText("\b \b");
      }
    } else if (b >= 32 && b < 127 && answer.length() < MAX_ANSWER) {
      answer += (char)b;
      sendToCalc(hidden ? '*' : b);
    }
  }
  asking = false;
  sendText("\r\n");
  return true;
}

// ---------- settings ----------

void loadSettings() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  target.wifiName = prefs.getString("wifiname", "");
  target.wifiPassword = prefs.getString("wifipass", "");
  target.host = prefs.getString("host", "");
  target.port = prefs.getUShort("port", 22);
  target.user = prefs.getString("user", "");
  prefs.end();
}

void saveSettings() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("wifiname", target.wifiName);
  prefs.putString("wifipass", target.wifiPassword);
  prefs.putString("host", target.host);
  prefs.putUShort("port", target.port);
  prefs.putString("user", target.user);
  prefs.end();
}

bool settingsComplete() {
  return target.wifiName.length() && target.host.length() && target.user.length();
}

// Asks for one setting, showing the current value; an empty answer keeps it.
bool askSetting(const char *name, String &value, bool hidden) {
  String shown = hidden ? (value.length() ? "unchanged" : "none") : value;
  String prompt = String(name) + " [" + shown + "]: ";
  String answer;
  if (!askOnCalc(prompt.c_str(), hidden, answer)) return false;
  if (answer.length()) value = answer;
  return true;
}

// The settings dialog, run on the calculator's screen. Nothing is stored
// unless every question was answered.
void editSettings() {
  Ssh::Target edited = target;

  sayToCalc("Settings. ENTER keeps the value in brackets.");
  sayToCalc("Looking for Wi-Fi networks...");
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  if (found > 9) found = 9;
  for (int i = 0; i < found; i++) {
    String line = String(" ") + (i + 1) + "  " + WiFi.SSID(i) + "  (" + WiFi.RSSI(i) + " dBm)";
    sayToCalc(line.c_str());
  }
  if (found <= 0) sayToCalc(" none found; the name can still be typed");

  String network;
  if (!askOnCalc(("Wi-Fi network, number or name [" + edited.wifiName + "]: ").c_str(), false, network)) return;
  if (network.length() == 1 && network[0] >= '1' && network[0] < '1' + found) {
    network = WiFi.SSID(network[0] - '1');
  }
  WiFi.scanDelete();
  if (network.length()) edited.wifiName = network;

  if (!askSetting("Wi-Fi password", edited.wifiPassword, true)) return;

  String host = edited.port == 22 ? edited.host : edited.host + ":" + edited.port;
  if (!askSetting("SSH host (name or address, optionally :port)", host, false)) return;
  int colon = host.lastIndexOf(':');
  long port = colon > 0 ? host.substring(colon + 1).toInt() : 22;
  edited.host = colon > 0 ? host.substring(0, colon) : host;
  edited.port = (port > 0 && port < 65536) ? port : 22;

  if (!askSetting("SSH user", edited.user, false)) return;

  target = edited;
  saveSettings();
  sayToCalc("Saved. Testing the Wi-Fi settings:");
  Ssh::joinWifi(target, sayToCalc);
  sayToCalc("[2nd+ESC for the menu]");
}

void runCalcCommand(char command) {
  if (backend == BACKEND_SSH) Ssh::close();
  backend = BACKEND_NONE;

  if (command == 's') {
    backend = BACKEND_SERIAL;
  } else if (command == 'h') {
    if (!settingsComplete()) {
      sayToCalc("SSH is not set up yet: choose Settings in the menu.");
      sayToCalc("[2nd+ESC for the menu]");
    } else if (Ssh::open(target, sayToCalc, askOnCalc)) {
      backend = BACKEND_SSH;
    } else if (!pendingCount) {
      sayToCalc("[2nd+ESC for the menu]");
    }
  } else if (command == 'c') {
    editSettings();
  }
}

// One line for the calculator menu's status bar.
void sendWifiStatus() {
  String status;
  if (!target.wifiName.length()) {
    status = "Wi-Fi: not set up (F3)";
  } else if (WiFi.status() == WL_CONNECTED) {
    status = "Wi-Fi: " + WiFi.SSID() + "  " + WiFi.RSSI() + " dBm  " + WiFi.localIP().toString();
  } else {
    status = "Wi-Fi: " + target.wifiName + ", not connected";
  }
  sendToCalc(CONTROL_MARK);
  sendToCalc('w');
  sendText(status.substring(0, 60).c_str());
  sendToCalc('\n');
}

void runPendingCommands() {
  while (pendingCount) {
    char command = pendingCommands[0];
    pendingCount--;
    memmove(pendingCommands, pendingCommands + 1, pendingCount);
    if (command == 'w') sendWifiStatus();
    else runCalcCommand(command);
  }
}

void pumpSsh() {
  uint8_t buf[64];
  int n = Ssh::read(buf, sizeof(buf));
  for (int i = 0; i < n; i++) {
    if (!sendToCalc(buf[i])) break;
  }
  if (n < 0) {
    Ssh::close();
    backend = BACKEND_NONE;
    sayToCalc("\r\n[connection closed, 2nd+ESC for the menu]");
  }
}

// ---------- commands (monitor mode) ----------

void printStats() {
  Serial.printf("to calc %lu, from calc %lu, dropped %lu, retries %lu, link errors %lu, line idle: %s\n",
                (unsigned long)toCalc, (unsigned long)fromCalc, (unsigned long)dropped,
                (unsigned long)retries, (unsigned long)ti.errors, ti.lineIdle() ? "yes" : "NO");
}

// Sends numbered 60-character lines, so a lost or repeated byte shows on the
// calculator's screen, and reports the speed.
void burst(uint32_t bytes) {
  static const char FILL[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012";
  uint32_t dropped0 = dropped, fromCalc0 = fromCalc, sent = 0, n = 0;
  uint32_t t0 = millis();

  while (sent < bytes) {
    char text[64];
    int len = snprintf(text, sizeof(text), "%04lu %s\r\n", (unsigned long)(n++ % 10000), FILL);
    for (int i = 0; i < len && sent < bytes; i++, sent++) {
      if (!sendToCalc((uint8_t)text[i])) { bytes = sent; break; }
    }
  }

  uint32_t ms = millis() - t0;
  Serial.printf("\nburst: %lu bytes in %lu ms = %lu bytes/s, %lu dropped, %lu received meanwhile\n",
                (unsigned long)sent, (unsigned long)ms,
                (unsigned long)(ms ? sent * 1000UL / ms : 0),
                (unsigned long)(dropped - dropped0), (unsigned long)(fromCalc - fromCalc0));
}

void handleLine(const String &text) {
  if (!text.startsWith("/")) {
    if (sendText(text.c_str())) sendText("\r\n");
  } else if (text == "/stats") {
    printStats();
  } else if (text.startsWith("/burst")) {
    long bytes = text.substring(6).toInt();
    burst(bytes > 0 ? bytes : 2000);
  } else if (text == "/settings") {
    Serial.printf("Wi-Fi network \"%s\" (password %s), SSH %s@%s port %u\n", target.wifiName.c_str(),
                  target.wifiPassword.length() ? "set" : "not set", target.user.c_str(),
                  target.host.c_str(), target.port);
  } else if (text == "/forgethost") {
    Ssh::forgetHostKey();
    Serial.println("The SSH host's remembered key is forgotten; the next connection stores it anew.");
  } else if (text == "/raw") {
    Serial.println("Raw mode: every byte now passes through. Reset the ESP32 to leave it.");
    Serial.flush();
    rawMode = true;
    paced = false;
  } else {
    Serial.println(HELP);
  }
}

// ---------- main ----------

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  ti.begin();
  loadSettings();
  // Join the stored network in the background, so it is up before it is needed.
  if (target.wifiName.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(target.wifiName.c_str(), target.wifiPassword.c_str());
  }
  delay(500);
  Serial.println("\nTI link terminal bridge ready. Run term() on the calculator.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.println(HELP);
  Serial.println();
}

void handleControl(const String &word) {
  if (word == "raw") rawMode = paced = true;
  else if (word == "monitor") rawMode = paced = false;
  unacked = 0;
  line = "";
}

void handleSerialByte(char c) {
  if (rawMode) {
    sendToCalc((uint8_t)c);
    if (paced && ++unacked == ACK_EVERY) {
      unacked = 0;
      Serial.write(CONTROL_MARK);
      Serial.write('a');
    }
  } else if (c == '\n' || c == '\r') {
    if (line.length()) handleLine(line);
    line = "";
  } else {
    line += c;
  }
}

void loop() {
  pumpFromCalc();
  runPendingCommands();
  if (backend == BACKEND_SSH) pumpSsh();

  // A paced host is made to wait while the calculator is not in a Serial
  // session: its bytes stay unread, so nothing it prints is lost.
  while (!(rawMode && paced && backend != BACKEND_SERIAL) && Serial.available()) {
    char c = Serial.read();
    if (inControlLine) {
      if (c == '\n') {
        handleControl(controlLine);
        inControlLine = false;
      } else if (controlLine.length() < 16) {
        controlLine += c;
      }
    } else if ((uint8_t)c == CONTROL_MARK) {
      if (++marksSeen == 3) {
        marksSeen = 0;
        inControlLine = true;
        controlLine = "";
      }
    } else {
      // Marks that turned out to be ordinary data are passed on.
      for (; marksSeen; marksSeen--) handleSerialByte((char)CONTROL_MARK);
      handleSerialByte(c);
    }
  }
}
