/*
 * esp32_tilink_term.ino - carries the calculator terminal's bytes
 *
 * The calculator runs calc/term (a dumb terminal). This sketch carries raw
 * bytes between it and a backend, on top of TiLink's bit handshake. There are
 * no packets: TI's protocol is not involved.
 *
 * The calculator picks the backend from its menu, with 0x1E and a letter:
 *   's'  Serial: the USB serial port, described below
 *   'h'  SSH: a shell on the selected host, over Wi-Fi (see Ssh.h)
 *   'n'  Wi-Fi settings: asks on the calculator for a network and its
 *        password, and stores them in the ESP32's flash (Preferences). Up
 *        to 4 networks are kept; the one joined last is joined again.
 *   'k'  SSH settings: the selected host and its user, stored likewise
 *   'g'  SSH key: makes the ESP32's own key pair, or removes it
 *   'i'  copies the public key to the selected host, after asking
 *   'j'  joins the selected Wi-Fi network, which is then the one in use
 *   '1' to '8'  selects one of the stored hosts for the 'h', 'k' or 'i'
 *        that follows, or one of the networks for 'j', and does nothing else
 *   'l'  asks for the list of hosts, for the calculator's host menu. The
 *        answer is 0x1E 'l' and one line for each of the 8 hosts, user@host
 *        or empty, each ended by a newline. It leaves the backend alone.
 *   'm'  asks for the list of Wi-Fi networks, for the calculator's Wi-Fi
 *        menu. The answer is 0x1E 'm' and one line for each of the 4
 *        networks, in the same way.
 *   'x'  the session is over
 *   'w'  asks for the Wi-Fi status, for the menu's status bar. The answer is
 *        0x1E 'w', '+' or '-' for Wi-Fi up or down, a line of text and a
 *        newline. It leaves the backend alone.
 *   'p'  a screenshot: the 3840 bytes of the calculator's screen follow, as
 *        they are. It leaves the backend alone too. With a computer attached
 *        (paced raw mode, below) they are passed on as 0x1E 'p' and the bytes. * A real 0x1E byte from the calculator arrives doubled.
 *
 * Serial has two modes:
 *   monitor (at power-up)  for the Serial Monitor. A typed line goes to the
 *                          calculator followed by CR LF. Lines starting with
 *                          '/' are commands. Bytes from the calculator are
 *                          printed, control codes as \xNN.
 *   raw (after /raw)       every byte passes through untouched, both ways,
 *                          and nothing else is printed. Reset to leave it.
 *
 * A program on the host (shell/shell.py) switches modes with a control
 * line that works in either mode: three 0x1E bytes, then "raw" or "monitor",
 * then a newline. "raw" is answered with 0x1E 'r', and at power-up the sketch
 * sends 0x1E 'b'. Monitor mode prints text meant for a person, which a
 * program that believes it is in raw mode would take for the calculator's
 * keys: the two marks let it notice a restart and wait for raw mode again.
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
#include "Display.h"
#include "Ssh.h"
#include "TiLink.h"

// libssh needs far more stack than the default 8 KB.
SET_LOOP_TASK_STACK_SIZE(48 * 1024);

// The pins follow the board the sketch is built for.
#if defined(ARDUINO_M5STACK_CARDPUTER)
// M5Stack Cardputer: the two signal pins of its Grove port.
const uint8_t PIN_TIP  = 2;   // red,   on the Grove cable's yellow wire
const uint8_t PIN_RING = 1;   // white, on the Grove cable's white wire
#else
// M5Stack Stamp S3. A classic ESP32 would use 25 / 26.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 5;
#endif

// How long a byte may wait for the calculator to start taking it.
const uint32_t SEND_PATIENCE_MS = 5000;

const char HELP[] =
  "Commands: /stats | /burst [bytes] | /raw | /settings | /pubkey | /forgethost [n] | /help   (anything else is sent)";

// Internal pull-ups on: with the calculator unplugged the lines then read as
// idle instead of floating, which would otherwise arrive as garbage bytes.
TiLink ti(PIN_TIP, PIN_RING, true);

bool rawMode = false;
bool linkDown = false;  // both lines held low: calculator off or half plugged

// The lines read idle with nothing plugged in too, so the calculator counts
// as there only while it is heard from: its program asks for the Wi-Fi
// status every 3 s in a menu. A quiet session counts as waiting.
const uint32_t HEARD_MS = 5000;
uint32_t lastHeardMs = 0;
bool heardOnce = false;
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
Ssh::Target target;  // the Wi-Fi network in use only: the host comes from hosts[]

// The Wi-Fi networks that may be joined, one for each of the calculator's
// keys F1 to F4. The one joined last is the one joined again.
const uint8_t MAX_NETWORKS = 4;
struct WifiNetwork {
  String name, password;
  bool isSet() const { return name.length(); }
};
WifiNetwork networks[MAX_NETWORKS];
uint8_t lastNetwork = 0;  // the one joined last, from 1; 0 for none yet

// The SSH hosts, one for each of the calculator's keys F1 to F8.
const uint8_t MAX_HOSTS = 8;
struct Host {
  String host;  // name or address
  uint16_t port = 22;
  String user;
  bool isSet() const { return host.length() && user.length(); }
};
Host hosts[MAX_HOSTS];
uint8_t slot = 1;  // the host or network the calculator selected, from 1

// A screenshot on its way in from the calculator: 240 x 128 pixels, one bit
// each. While it arrives nothing is sent to the calculator.
const size_t SHOT_BYTES = 3840;
const uint32_t SHOT_GAP_MS = 2000;  // a longer silence means the rest is not coming
uint8_t shot[SHOT_BYTES];
size_t shotLeft = 0;
uint32_t shotLastMs = 0;

// Pacing of raw mode, see the top of the file.
const uint8_t ACK_EVERY = 32;
bool paced = false;
uint8_t unacked = 0;

// ---------- the Cardputer's screen ----------

// Does nothing on a board without a screen. Called between bytes, never in
// the middle of one.
void showStatus() {
  if (!Display::due()) return;
  Display::Status s;
  s.mode = backend == BACKEND_SSH ? Display::MODE_SSH
         : backend == BACKEND_SERIAL ? Display::MODE_SERIAL : Display::MODE_MENU;
  s.asking = asking;
  s.computerAttached = rawMode;
  s.calcPresent = !linkDown;
  s.calcHeard = heardOnce && millis() - lastHeardMs < HEARD_MS;
  s.linesIdle = ti.lineIdle();
  s.toCalc = toCalc;
  s.fromCalc = fromCalc;
  s.dropped = dropped;
  s.resends = retries;
  s.errors = ti.errors;
  uint8_t hostsSet = 0;
  for (const Host &h : hosts) hostsSet += h.isSet();
  if (backend == BACKEND_SSH) s.sshTarget = hostLabel(slot);
  else if (hostsSet) s.sshTarget = String(hostsSet) + (hostsSet == 1 ? " SSH host" : " SSH hosts");
  s.wifiName = wifiLabel();
  s.wifiUp = WiFi.status() == WL_CONNECTED;
  s.rssi = s.wifiUp ? WiFi.RSSI() : 0;
  if (s.wifiUp) s.ip = WiFi.localIP().toString();
  Display::update(s);
}

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

void finishShot() {
  if (rawMode && paced) {
    Serial.write(CONTROL_MARK);
    Serial.write('p');
    Serial.write(shot, SHOT_BYTES);
    Display::log("screenshot sent to the computer");
  } else {
    Display::log("screenshot: no computer attached");
    if (!rawMode) Serial.println("\nA screenshot arrived. Run \"make screenshots\" or \"make shell\" to save them.");
  }
}

// Takes everything the calculator is sending right now.
void pumpFromCalc() {
  showStatus();
  if (shotLeft && millis() - shotLastMs > SHOT_GAP_MS) {
    shotLeft = 0;
    Display::log("screenshot: cut short");
    if (!rawMode) Serial.println("\n! The screenshot stopped part way.");
  }
  if (linkDown) {
    if (!ti.lineIdle()) return;
    linkDown = false;
    Display::log("calculator is back");
    if (!rawMode) Serial.println("Link is back.");
  }
  for (;;) {
    int b = ti.recvByte(0);
    if (b == -1) return;
    if (b < 0) {
      if (ti.lastErrStage == 2 && ti.lastErrBit == 0) {
        linkDown = true;
        Display::log("calculator off or unplugged");
        if (!rawMode) Serial.println("\n! Both lines are low: is the calculator on and plugged in?");
      } else {
        reportLinkError("receive error");
      }
      return;
    }
    lastHeardMs = millis();
    heardOnce = true;
    if (shotLeft) {
      fromCalc++;
      shot[SHOT_BYTES - shotLeft] = (uint8_t)b;
      shotLastMs = millis();
      if (--shotLeft == 0) finishShot();
    } else if (markFromCalc) {
      markFromCalc = false;
      if (b == CONTROL_MARK) {
        forwardFromCalc((uint8_t)b);
      } else if (b == 'p') {
        shotLeft = SHOT_BYTES;
        shotLastMs = millis();
      } else if (pendingCount < sizeof(pendingCommands)) {
        pendingCommands[pendingCount++] = (char)b;
      }
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
    if (shotLeft) continue;  // a screenshot is arriving: the link is the calculator's
    if (ti.sendByte(b)) {
      toCalc++;
      lastHeardMs = millis();  // it took the byte, bit by bit
      heardOnce = true;
      return true;
    }
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
  if (text[0] != '[') Display::log(text);  // not the "[2nd+ESC ...]" hints
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

// user@host, with the port if it is not 22. Empty for a host that is not set.
String hostLabel(uint8_t n) {
  const Host &h = hosts[n - 1];
  if (!h.isSet()) return "";
  return h.user + "@" + h.host + (h.port == 22 ? String() : ":" + String(h.port));
}

uint8_t networksSet() {
  uint8_t count = 0;
  for (const WifiNetwork &n : networks) count += n.isSet();
  return count;
}

// Which of the stored networks has this name, from 0; -1 if none.
int knownNetwork(const String &name) {
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    if (networks[i].isSet() && networks[i].name == name) return i;
  }
  return -1;
}

// The network for the status displays: the one in use, or what is stored.
String wifiLabel() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
  uint8_t count = networksSet();
  if (count > 1) return String(count) + " networks";
  for (const WifiNetwork &n : networks) if (n.isSet()) return n.name;
  return "";
}

// The stored networks in the order they are tried, as indexes from 0: the
// one joined last, then the others as they are numbered. Returns how many.
uint8_t networkOrder(uint8_t *order) {
  uint8_t count = 0;
  if (lastNetwork && networks[lastNetwork - 1].isSet()) order[count++] = lastNetwork - 1;
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    if (networks[i].isSet() && i + 1 != lastNetwork) order[count++] = i;
  }
  return count;
}

// Joins the stored network with this index, from 0, and remembers it as the
// one to join from now on.
bool tryNetwork(uint8_t index, Ssh::SayFn say) {
  target.wifiName = networks[index].name;
  target.wifiPassword = networks[index].password;
  if (!Ssh::joinWifi(target, say)) return false;
  if (lastNetwork != index + 1) {
    lastNetwork = index + 1;
    Preferences prefs;
    prefs.begin(PREFS_NAMESPACE, false);
    prefs.putUChar("wifilast", lastNetwork);
    prefs.end();
  }
  return true;
}

// Makes sure a stored network is joined. One that is joined already is kept.
// Otherwise the one joined last is tried first and then the others, leaving
// out those a scan does not show; if it shows none of them, all are tried,
// since one may be hidden.
bool joinKnownWifi(Ssh::SayFn say) {
  int joined = WiFi.status() == WL_CONNECTED ? knownNetwork(WiFi.SSID()) : -1;
  if (joined >= 0) {
    target.wifiName = networks[joined].name;
    target.wifiPassword = networks[joined].password;
    return true;
  }

  uint8_t order[MAX_NETWORKS];
  uint8_t count = networkOrder(order);
  bool seen[MAX_NETWORKS] = {};
  bool anySeen = false;
  if (count > 1) {  // an only network needs no looking for
    say("Looking for a known Wi-Fi network...");
    WiFi.disconnect();  // a join still being tried would make the scan fail
    WiFi.mode(WIFI_STA);
    int found = WiFi.scanNetworks();
    for (int i = 0; i < found; i++) {
      int known = knownNetwork(WiFi.SSID(i));
      if (known >= 0) anySeen = seen[known] = true;
    }
    WiFi.scanDelete();
  }

  for (uint8_t i = 0; i < count; i++) {
    if (anySeen && !seen[order[i]]) continue;
    if (tryNetwork(order[i], say)) return true;
    pumpFromCalc();
    if (pendingCount) return false;  // the user left for the menu
  }
  return false;
}

// What Ssh needs to reach a host: the Wi-Fi settings and the host's own.
Ssh::Target targetFor(uint8_t n) {
  Ssh::Target t = target;
  t.host = hosts[n - 1].host;
  t.port = hosts[n - 1].port;
  t.user = hosts[n - 1].user;
  t.slot = n;
  return t;
}

void loadSettings() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    String n(i + 1);
    networks[i].name = prefs.getString(("wifiname" + n).c_str(), "");
    networks[i].password = prefs.getString(("wifipass" + n).c_str(), "");
  }
  lastNetwork = prefs.getUChar("wifilast", 0);
  // Before there were several networks the only one was stored without a number.
  if (!prefs.isKey("wifiname1") && prefs.isKey("wifiname")) {
    networks[0].name = prefs.getString("wifiname", "");
    networks[0].password = prefs.getString("wifipass", "");
  }
  for (uint8_t i = 0; i < MAX_HOSTS; i++) {
    String n(i + 1);
    hosts[i].host = prefs.getString(("host" + n).c_str(), "");
    hosts[i].port = prefs.getUShort(("port" + n).c_str(), 22);
    hosts[i].user = prefs.getString(("user" + n).c_str(), "");
  }
  // Before there were several hosts the only one was stored without a number.
  if (!prefs.isKey("host1") && prefs.isKey("host")) {
    hosts[0].host = prefs.getString("host", "");
    hosts[0].port = prefs.getUShort("port", 22);
    hosts[0].user = prefs.getString("user", "");
  }
  prefs.end();
}

void saveSettings() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    String n(i + 1);
    prefs.putString(("wifiname" + n).c_str(), networks[i].name);
    prefs.putString(("wifipass" + n).c_str(), networks[i].password);
  }
  prefs.remove("wifiname");  // the names from before there were several networks
  prefs.remove("wifipass");
  for (uint8_t i = 0; i < MAX_HOSTS; i++) {
    String n(i + 1);
    prefs.putString(("host" + n).c_str(), hosts[i].host);
    prefs.putUShort(("port" + n).c_str(), hosts[i].port);
    prefs.putString(("user" + n).c_str(), hosts[i].user);
  }
  prefs.remove("host");  // and from before there were several hosts
  prefs.remove("port");
  prefs.remove("user");
  prefs.end();
}

// SSH to the selected host can be tried.
bool sshReady() {
  return networksSet() && hosts[slot - 1].isSet();
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

const char BACK_HINT[] = "[2nd+ESC for the menu]";

// Joins the network picked in the calculator's Wi-Fi menu.
void joinSelectedWifi() {
  if (slot > MAX_NETWORKS || !networks[slot - 1].isSet()) {
    sayToCalc("That network is not set: Settings, then Wi-Fi (F1).");
  } else if (tryNetwork(slot - 1, sayToCalc)) {
    sayToCalc(("On " + networks[slot - 1].name + ". It is the network used from now on.").c_str());
  }
  sayToCalc(BACK_HINT);
}

// The settings dialogs, run on the calculator's screen. Nothing is stored
// unless every question of a dialog was answered.
void editWifi() {
  sayToCalc("Wi-Fi networks. The menu's Wi-Fi entry picks the one to join.");
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    sayToCalc((String(" ") + (i + 1) + "  " + (networks[i].isSet() ? networks[i].name : String("-"))).c_str());
  }
  String choice;
  String prompt = String("Set, change or remove which, 1 to ") + MAX_NETWORKS + " (ENTER: none): ";
  if (!askOnCalc(prompt.c_str(), false, choice)) return;
  if (choice.length() != 1 || choice[0] < '1' || choice[0] >= '1' + MAX_NETWORKS) {
    sayToCalc("Nothing was changed.");
    sayToCalc(BACK_HINT);
    return;
  }
  uint8_t index = choice[0] - '1';
  WifiNetwork &current = networks[index];

  if (current.isSet()) {
    sayToCalc(("  " + current.name).c_str());
    sayToCalc(" 1  change it");
    sayToCalc(" 2  remove it");
    if (!askOnCalc("1, 2, or ENTER to leave it: ", false, choice)) return;
    if (choice == "2") {
      current = WifiNetwork();
      saveSettings();
      sayToCalc("Removed.");
    } else if (choice != "1") {
      sayToCalc("The network was left as it is.");
    }
    if (choice != "1") {
      sayToCalc(BACK_HINT);
      return;
    }
  }

  WifiNetwork edited = current;
  sayToCalc("ENTER keeps the value in brackets.");
  sayToCalc("Looking for networks...");
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  if (found > 9) found = 9;
  for (int i = 0; i < found; i++) {
    String line = String(" ") + (i + 1) + "  " + WiFi.SSID(i) + "  (" + WiFi.RSSI(i) + " dBm)";
    sayToCalc(line.c_str());
  }
  if (found <= 0) sayToCalc(" none found; the name can still be typed");

  String network;
  if (!askOnCalc(("Network, number or name [" + edited.name + "]: ").c_str(), false, network)) return;
  if (network.length() == 1 && network[0] >= '1' && network[0] < '1' + found) {
    network = WiFi.SSID(network[0] - '1');
  }
  WiFi.scanDelete();
  if (network.length() && network != edited.name) {
    edited.name = network;
    edited.password = "";  // the old network's password is of no use here
  }

  if (!askSetting("Password", edited.password, true)) return;

  if (!edited.isSet()) {
    sayToCalc("A network name is needed. Nothing was saved.");
  } else {
    current = edited;
    saveSettings();
    sayToCalc("Saved. Testing:");
    tryNetwork(index, sayToCalc);
  }
  sayToCalc(BACK_HINT);
}

// Sets, changes or removes the selected host.
void editSshHost() {
  Host &current = hosts[slot - 1];
  Host edited = current;

  sayToCalc(("SSH host " + String(slot)).c_str());
  if (current.isSet()) {
    sayToCalc(("  " + hostLabel(slot)).c_str());
    sayToCalc(" 1  change it");
    sayToCalc(" 2  remove it");
    String choice;
    if (!askOnCalc("1, 2, or ENTER to leave it: ", false, choice)) return;
    if (choice == "2") {
      current = Host();
      saveSettings();
      Ssh::forgetHostKey(slot);
      sayToCalc("Removed.");
    } else if (choice != "1") {
      sayToCalc("The host was left as it is.");
    }
    if (choice != "1") {
      sayToCalc(BACK_HINT);
      return;
    }
  }

  sayToCalc("ENTER keeps the value in brackets.");
  String host = edited.port == 22 ? edited.host : edited.host + ":" + edited.port;
  if (!askSetting("Host (name or address, optionally :port)", host, false)) return;
  int colon = host.lastIndexOf(':');
  long port = colon > 0 ? host.substring(colon + 1).toInt() : 22;
  edited.host = colon > 0 ? host.substring(0, colon) : host;
  edited.port = (port > 0 && port < 65536) ? port : 22;

  if (!askSetting("User", edited.user, false)) return;

  if (!edited.isSet()) {
    sayToCalc("A host and a user are both needed. Nothing was saved.");
  } else {
    // A different machine may answer now: its key is learned anew.
    if (edited.host != current.host || edited.port != current.port) Ssh::forgetHostKey(slot);
    current = edited;
    saveSettings();
    sayToCalc("Saved.");
  }
  sayToCalc(BACK_HINT);
}

// True only for the whole word, so that a stray ENTER changes nothing.
bool confirmedOnCalc(const char *prompt) {
  String answer;
  return askOnCalc(prompt, false, answer) && answer == "yes";
}

void showSshKey() {
  sayToCalc("This ESP32's key:");
  sayToCalc(("  " + Ssh::keyFingerprint()).c_str());
}

void makeSshKey() {
  sayToCalc("Making a key...");
  if (!Ssh::makeKey(sayToCalc)) return;
  showSshKey();
  sayToCalc("No host knows it yet: Settings, F4 copies it to the host.");
}

// Makes, replaces or removes the ESP32's key. Nothing here talks to a host.
void editSshKey() {
  if (!Ssh::hasKey()) {
    sayToCalc("SSH key. This ESP32 has none.");
    sayToCalc("With one, and once the host has its public half,");
    sayToCalc("SSH logs in without a password. It is kept in the");
    sayToCalc("ESP32, readable by whoever holds the board.");
    if (confirmedOnCalc("Make one? Type yes: ")) makeSshKey();
    else if (!pendingCount) sayToCalc("No key was made.");
  } else {
    showSshKey();
    sayToCalc(" 1  replace it with a new one");
    sayToCalc(" 2  remove it");
    String choice;
    if (!askOnCalc("1, 2, or ENTER to leave it: ", false, choice)) return;
    if (choice == "1") {
      sayToCalc("Hosts that have the old key will ask for the password.");
      if (confirmedOnCalc("Replace it? Type yes: ")) makeSshKey();
      else if (!pendingCount) sayToCalc("The key was left as it is.");
    } else if (choice == "2") {
      sayToCalc("SSH will ask for the password again. The host keeps");
      sayToCalc("its copy of the public half until it is deleted there.");
      if (confirmedOnCalc("Remove it? Type yes: ")) {
        Ssh::forgetKey();
        sayToCalc("Removed.");
      } else if (!pendingCount) {
        sayToCalc("The key was left as it is.");
      }
    }
  }
  if (!pendingCount) sayToCalc(BACK_HINT);
}

// Puts the public key on the selected host, only after the user has read
// what that means and typed yes.
void copySshKey() {
  const Host &to = hosts[slot - 1];
  sayToCalc("Copy the key to the host");
  if (!Ssh::hasKey()) {
    sayToCalc("This ESP32 has no key yet: Settings, F3 makes one.");
  } else if (!sshReady()) {
    sayToCalc("Set the Wi-Fi (F1) and this SSH host (F2) first.");
  } else {
    sayToCalc("This adds the public key");
    sayToCalc(("  " + Ssh::keyFingerprint()).c_str());
    sayToCalc(("to ~/.ssh/authorized_keys of " + to.user + "@" + to.host + ".").c_str());
    sayToCalc("Whoever holds this ESP32 can then log in there");
    sayToCalc("without a password.");
    if (!confirmedOnCalc("Go ahead? Type yes: ")) {
      if (!pendingCount) sayToCalc("Nothing was copied.");
    } else if (!joinKnownWifi(sayToCalc)) {
      if (!pendingCount) sayToCalc("None of the Wi-Fi networks in Settings could be joined.");
    } else if (Ssh::copyKeyToHost(targetFor(slot), sayToCalc, askOnCalc)) {
      Display::log("SSH key copied to the host");
      sayToCalc("Copied. SSH to this host now logs in with the key.");
    } else if (!pendingCount) {
      sayToCalc("Nothing was copied.");
    }
  }
  if (!pendingCount) sayToCalc(BACK_HINT);
}

void runCalcCommand(char command) {
  if (backend == BACKEND_SSH) {
    Ssh::close();
    Display::log("SSH session closed");
  }
  backend = BACKEND_NONE;

  Display::log(command == 's' ? "calculator: Serial" : command == 'h' ? "calculator: SSH"
               : command == 'n' ? "calculator: Wi-Fi settings"
               : command == 'j' ? "calculator: join a Wi-Fi network"
               : command == 'k' ? "calculator: SSH settings"
               : command == 'g' ? "calculator: SSH key"
               : command == 'i' ? "calculator: copy the SSH key" : "calculator: menu");
  if (command == 's') {
    backend = BACKEND_SERIAL;
  } else if (command == 'h') {
    if (!sshReady()) {
      sayToCalc("SSH is not set up yet: choose Settings in the menu,");
      sayToCalc("then Wi-Fi (F1) and SSH hosts (F2).");
      sayToCalc(BACK_HINT);
    } else if (!joinKnownWifi(sayToCalc)) {
      if (!pendingCount) {
        sayToCalc("None of the Wi-Fi networks in Settings could be joined.");
        sayToCalc(BACK_HINT);
      }
    } else if (Ssh::open(targetFor(slot), sayToCalc, askOnCalc)) {
      backend = BACKEND_SSH;
      Display::log("SSH session open");
    } else if (!pendingCount) {
      sayToCalc(BACK_HINT);
    }
  } else if (command == 'j') {
    joinSelectedWifi();
  } else if (command == 'n') {
    editWifi();
  } else if (command == 'k') {
    editSshHost();
  } else if (command == 'g') {
    editSshKey();
  } else if (command == 'i') {
    copySshKey();
  }
}

// One line for the calculator menu's status bar, after '+' or '-' for Wi-Fi
// up or down: the menu entries that need Wi-Fi go by it.
void sendWifiStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  String status;
  if (!networksSet()) {
    status = "Wi-Fi: not set up (F4, then F1)";
  } else if (up) {
    status = "Wi-Fi: " + WiFi.SSID() + "  " + WiFi.RSSI() + " dBm  " + WiFi.localIP().toString();
  } else {
    status = "Wi-Fi: " + wifiLabel() + ", not connected";
  }
  sendToCalc(CONTROL_MARK);
  sendToCalc('w');
  sendToCalc(up ? '+' : '-');
  sendText(status.substring(0, 60).c_str());
  sendToCalc('\n');
}

// The hosts, for the calculator's host menu: a line each, empty if not set.
void sendHostList() {
  sendToCalc(CONTROL_MARK);
  sendToCalc('l');
  for (uint8_t n = 1; n <= MAX_HOSTS; n++) {
    sendText(hostLabel(n).c_str());
    sendToCalc('\n');
  }
}

// The networks, for the calculator's Wi-Fi menu, in the same way.
void sendNetworkList() {
  bool up = WiFi.status() == WL_CONNECTED;
  sendToCalc(CONTROL_MARK);
  sendToCalc('m');
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    String label = networks[i].name;
    if (!networks[i].isSet()) label = "";
    else if (up && WiFi.SSID() == label) label += String("  (joined, ") + WiFi.RSSI() + " dBm)";
    else if (i + 1 == lastNetwork) label += "  (used last)";
    sendText(label.c_str());
    sendToCalc('\n');
  }
}

void runPendingCommands() {
  while (pendingCount) {
    char command = pendingCommands[0];
    pendingCount--;
    memmove(pendingCommands, pendingCommands + 1, pendingCount);
    if (command == 'w') sendWifiStatus();
    else if (command == 'l') sendHostList();
    else if (command == 'm') sendNetworkList();
    else if (command >= '1' && command < '1' + MAX_HOSTS) slot = command - '0';
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
    Display::log("SSH closed by the host");
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
    for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
      if (!networks[i].isSet()) continue;
      Serial.printf("Wi-Fi network %u: \"%s\" (password %s)\n", i + 1, networks[i].name.c_str(),
                    networks[i].password.length() ? "set" : "not set");
    }
    for (uint8_t n = 1; n <= MAX_HOSTS; n++) {
      if (hosts[n - 1].isSet()) Serial.printf("SSH host %u: %s\n", n, hostLabel(n).c_str());
    }
    if (Ssh::hasKey()) Serial.printf("SSH key %s\n", Ssh::keyFingerprint().c_str());
    else Serial.println("No SSH key");
  } else if (text == "/pubkey") {
    if (Ssh::hasKey()) Serial.println(Ssh::publicKeyLine());
    else Serial.println("No SSH key. The calculator's Settings menu makes one.");
  } else if (text.startsWith("/forgethost")) {
    long only = text.substring(11).toInt();  // 0 without a number: all of them
    if (only < 0 || only > MAX_HOSTS) {
      Serial.printf("The hosts are numbered 1 to %u.\n", MAX_HOSTS);
    } else {
      for (uint8_t n = 1; n <= MAX_HOSTS; n++) if (!only || only == n) Ssh::forgetHostKey(n);
      Serial.println(only ? "That SSH host's remembered key is forgotten; the next connection stores it anew."
                          : "Every SSH host's remembered key is forgotten; the next connections store them anew.");
    }
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
  Display::begin();  // before the link: the screen's library sets up pins of its own
  ti.begin();
  loadSettings();
  // Join the network used last in the background, so it is up before it is
  // needed.
  uint8_t order[MAX_NETWORKS];
  if (networkOrder(order)) {
    target.wifiName = networks[order[0]].name;
    target.wifiPassword = networks[order[0]].password;
    WiFi.mode(WIFI_STA);
    WiFi.begin(target.wifiName.c_str(), target.wifiPassword.c_str());
  }
  delay(500);
  // For a program that was attached before the restart, see the top of the file.
  Serial.write(CONTROL_MARK);
  Serial.write('b');
  Serial.println("\nTI link terminal bridge ready. Run term() on the calculator.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.println(HELP);
  Serial.println();
}

void handleControl(const String &word) {
  if (word == "raw") {
    rawMode = paced = true;
    Serial.write(CONTROL_MARK);
    Serial.write('r');
  } else if (word == "monitor") {
    rawMode = paced = false;
  }
  Display::log(rawMode ? "computer attached" : "computer detached");
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
