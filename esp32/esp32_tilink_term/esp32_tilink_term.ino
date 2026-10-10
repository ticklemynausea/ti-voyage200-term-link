/*
 * esp32_tilink_term.ino - byte bridge between USB serial and the calculator
 *
 * The calculator runs calc/term (a dumb terminal). This sketch carries raw
 * bytes between it and Serial, on top of TiLink's bit handshake. There are no
 * packets: TI's protocol is not involved.
 *
 * Two modes:
 *   monitor (at power-up)  for the Serial Monitor. A typed line goes to the
 *                          calculator followed by CR LF. Lines starting with
 *                          '/' are commands. Bytes from the calculator are
 *                          printed, control codes as \xNN.
 *   raw (after /raw)       every byte passes through untouched, both ways,
 *                          and nothing else is printed. Reset to leave it.
 *
 * Serial Monitor: 115200 baud, newline line ending.
 * Needs TiLink.h / TiLink.cpp in the same folder.
 */
#include "TiLink.h"

// Classic ESP32: 25 / 26.  M5Stack Stamp S3: 1 / 5.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 5;

// How long a byte may wait for the calculator to start taking it.
const uint32_t SEND_PATIENCE_MS = 5000;

const char HELP[] =
  "Commands: /stats | /burst [bytes] | /raw | /help   (anything else is sent)";

// Internal pull-ups on: with the calculator unplugged the lines then read as
// idle instead of floating, which would otherwise arrive as garbage bytes.
TiLink ti(PIN_TIP, PIN_RING, true);

bool rawMode = false;
bool linkDown = false;  // both lines held low: calculator off or half plugged
uint32_t toCalc = 0, fromCalc = 0, dropped = 0, retries = 0;
String line;

// ---------- calculator -> Serial ----------

void forwardFromCalc(uint8_t b) {
  fromCalc++;
  if (rawMode) Serial.write(b);
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
    forwardFromCalc((uint8_t)b);
  }
}

// ---------- Serial -> calculator ----------

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
  } else if (text == "/raw") {
    Serial.println("Raw mode: every byte now passes through. Reset the ESP32 to leave it.");
    Serial.flush();
    rawMode = true;
  } else {
    Serial.println(HELP);
  }
}

// ---------- main ----------

void setup() {
  Serial.begin(115200);
  ti.begin();
  delay(500);
  Serial.println("\nTI link terminal bridge ready. Run term() on the calculator.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.println(HELP);
  Serial.println();
}

void loop() {
  pumpFromCalc();

  while (Serial.available()) {
    char c = Serial.read();
    if (rawMode) {
      sendToCalc((uint8_t)c);
    } else if (c == '\n' || c == '\r') {
      if (line.length()) handleLine(line);
      line = "";
    } else {
      line += c;
    }
  }
}
