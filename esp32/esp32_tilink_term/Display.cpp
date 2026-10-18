#if defined(ARDUINO_M5STACK_CARDPUTER)

#include "Display.h"
#include <M5Cardputer.h>

// The Makefile writes it in the build folder; a build from elsewhere has none.
#if __has_include("version.h")
#include "version.h"
#else
#define VERSION "unknown"
#define BUILT __DATE__ " " __TIME__
#endif

namespace Display {

namespace {

const uint32_t REDRAW_MS = 250;
const uint32_t KEYS_MS = 50;
const uint32_t RATE_MS = 1000;
const uint32_t BATTERY_MS = 5000;

const int PAGES = 4;
const char *const PAGE_NAMES[PAGES] = {"STATUS", "LINK", "LOG", "BUILD"};

// The page is text: up to MAX_LINES lines, each in the small font (40
// columns) or the same font doubled (20 columns). A page is only pushed to
// the screen when its lines differ from the ones last drawn.
const int MAX_LINES = 13;
const int LINE_CHARS = 41;
struct Line {
  char text[LINE_CHARS];
  uint8_t size;
  uint16_t colour;
};
Line lines[MAX_LINES], drawn[MAX_LINES];
int lineCount, drawnCount = -1;

const int LOG_LINES = 11;
struct LogLine {
  uint32_t at;  // seconds since power-up
  char text[32];
};
LogLine logLines[LOG_LINES];
int logCount;

M5Canvas canvas(&M5Cardputer.Display);
int page;
bool forced = true;
uint32_t lastDraw, lastKeys, lastRate, lastBattery, lastByteAt, seenBytes;

// Bytes and frames a second, for the counts that have a rate.
struct Rate {
  Status::Count last, perSecond;
  void take(const Status::Count &now, uint32_t ms) {
    perSecond.bytes = (uint32_t)((uint64_t)(now.bytes - last.bytes) * 1000 / ms);
    perSecond.frames = (uint32_t)((uint64_t)(now.frames - last.frames) * 1000 / ms);
    last = now;
  }
};
Rate toRate, fromRate, outRate, inRate;
int battery = -1;

// The G0 button steps the backlight through these, then back to off.
const uint8_t BRIGHTNESS[] = {0, 64, 128, 191, 255};  // 0 to 100% in quarters
const int BRIGHTNESS_STEPS = sizeof(BRIGHTNESS);
int brightness = 1;  // 25%

void add(uint8_t size, uint16_t colour, const char *format, ...) {
  if (lineCount == MAX_LINES) return;
  Line &line = lines[lineCount++];
  memset(&line, 0, sizeof(line));  // so that two lines compare equal byte for byte
  va_list args;
  va_start(args, format);
  vsnprintf(line.text, sizeof(line.text), format, args);
  va_end(args);
  line.size = size;
  line.colour = colour;
}

void clock(char *out, size_t size, uint32_t seconds) {
  snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
           (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
}

// A count in at most 7 characters: as it is up to a million, then in M and G.
void amount(char *out, size_t size, uint32_t n) {
  if (n < 1000000) snprintf(out, size, "%lu", (unsigned long)n);
  else if (n < 1000000000) snprintf(out, size, "%lu.%luM", (unsigned long)(n / 1000000), (unsigned long)(n % 1000000 / 100000));
  else snprintf(out, size, "%lu.%luG", (unsigned long)(n / 1000000000), (unsigned long)(n % 1000000000 / 100000000));
}

// A rate in at most 5 characters: as it is up to 999, then in k.
void rate(char *out, size_t size, uint32_t perSecond) {
  if (perSecond < 1000) snprintf(out, size, "%lu", (unsigned long)perSecond);
  else snprintf(out, size, "%lu.%luk", (unsigned long)(perSecond / 1000), (unsigned long)(perSecond % 1000 / 100));
}

// A row of the link page's table: a label, the bytes and frames, and with
// a rate, those a second.
void countRow(uint16_t colour, const char *label, const Status::Count &c, const Rate *r) {
  char bytes[12], frames[12], bytesPs[8] = "", framesPs[8] = "";
  amount(bytes, sizeof(bytes), c.bytes);
  amount(frames, sizeof(frames), c.frames);
  if (r) {
    rate(bytesPs, sizeof(bytesPs), r->perSecond.bytes);
    rate(framesPs, sizeof(framesPs), r->perSecond.frames);
  }
  add(1, colour, "%-10s%8s%7s%6s%5s", label, bytes, frames, bytesPs, framesPs);
}

void statusPage(const Status &s) {
  const char *mode = s.mode == MODE_SSH ? "SSH" : s.mode == MODE_SERIAL ? "SERIAL" : "MENU";
  add(2, s.mode == MODE_MENU ? TFT_WHITE : TFT_GREEN, "%s%s", mode, s.asking ? " asking" : "");
  if (s.mode == MODE_SERIAL) {
    add(1, s.computerAttached ? TFT_WHITE : TFT_ORANGE, "%s",
        s.computerAttached ? "computer: attached" : "computer: run \"make shell\"");
  } else {
    add(1, TFT_LIGHTGREY, "%s", s.sshTarget.length() ? s.sshTarget.c_str() : "SSH not set up: F3 on the calc");
  }
  add(1, TFT_BLACK, "");

  if (!s.wifiName.length()) {
    add(2, TFT_ORANGE, "No Wi-Fi set");
    add(1, TFT_LIGHTGREY, "Settings: F3 on the calculator");
  } else {
    add(2, s.wifiUp ? TFT_GREEN : TFT_RED, "%.20s", s.wifiName.c_str());
    if (s.wifiUp) add(1, TFT_LIGHTGREY, "%d dBm   %s", s.rssi, s.ip.c_str());
    else add(1, TFT_RED, "not connected");
  }
  add(1, TFT_BLACK, "");

  char to[8], from[8];
  rate(to, sizeof(to), toRate.perSecond.bytes);
  rate(from, sizeof(from), fromRate.perSecond.bytes);
  if (!s.calcPresent) add(1, TFT_RED, "calculator: off or unplugged");
  else if (!s.calcHeard) add(1, TFT_ORANGE, "calculator: waiting");
  else add(1, TFT_WHITE, "calculator: connected");
  add(1, TFT_LIGHTGREY, "to calc %s B/s   from calc %s B/s", to, from);
}

// The frames' contents to and from the calculator, everything on the line
// (acknowledgements and framing too), and what went wrong. Bytes on the
// line and in bad frames are as they went over the wire. The handshake row
// counts single bytes whose bit handshake failed: the link layer sends their
// frames again, so they cost time, not data.
void linkPage(const Status &s) {
  char up[16], errors[12];
  clock(up, sizeof(up), millis() / 1000);

  if (!s.calcPresent) add(1, TFT_RED, "calculator   off or unplugged");
  else if (!s.calcHeard) add(1, TFT_ORANGE, "calculator   waiting, lines %s", s.linesIdle ? "idle" : "busy");
  else add(1, TFT_WHITE, "calculator   connected, lines %s", s.linesIdle ? "idle" : "busy");
  add(1, TFT_LIGHTGREY, "%-10s%8s%7s%6s%5s", "", "bytes", "frames", "B/s", "fr/s");
  countRow(TFT_WHITE, "to calc", s.toCalc, &toRate);
  countRow(TFT_WHITE, "from calc", s.fromCalc, &fromRate);
  countRow(TFT_LIGHTGREY, "line out", s.lineOut, &outRate);
  countRow(TFT_LIGHTGREY, "line in", s.lineIn, &inRate);
  countRow(s.resent.frames ? TFT_ORANGE : TFT_LIGHTGREY, "sent again", s.resent, nullptr);
  countRow(s.bad.frames ? TFT_ORANGE : TFT_LIGHTGREY, "bad", s.bad, nullptr);
  countRow(s.dropped.frames ? TFT_ORANGE : TFT_LIGHTGREY, "dropped", s.dropped, nullptr);
  amount(errors, sizeof(errors), s.handshakeErrors);
  add(1, s.handshakeErrors ? TFT_ORANGE : TFT_LIGHTGREY, "%-10s%8s%7s", "handshake", errors, "-");
  char last[16] = "none yet";
  if (seenBytes) snprintf(last, sizeof(last), "%lu s ago", (unsigned long)((millis() - lastByteAt) / 1000));
  add(1, TFT_LIGHTGREY, "last byte %-12s uptime %s", last, up);
}

void logPage() {
  if (!logCount) add(1, TFT_LIGHTGREY, "nothing yet");
  for (int i = 0; i < logCount; i++) {
    char at[16];
    clock(at, sizeof(at), logLines[i].at);
    add(1, TFT_WHITE, "%s %s", at, logLines[i].text);
  }
}

void buildPage() {
  add(1, TFT_LIGHTGREY, "built");
  add(2, TFT_WHITE, "%s", BUILT);
  add(1, TFT_BLACK, "");
  add(1, TFT_LIGHTGREY, "commit (+: with changes not committed)");
  add(2, TFT_WHITE, "%s", VERSION);
}

void draw() {
  canvas.fillSprite(TFT_BLACK);
  canvas.fillRect(0, 0, canvas.width(), 12, TFT_DARKGREY);
  int y = 2;
  for (int i = 0; i < lineCount; i++) {
    canvas.setTextSize(lines[i].size);
    canvas.setTextColor(lines[i].colour);
    canvas.setCursor(2, y);
    canvas.print(lines[i].text);
    y += 8 * lines[i].size + 2;
    if (i == 0) y += 3;  // room under the header bar
  }
  canvas.pushSprite(0, 0);
}

void nextPage(int to) {
  page = to % PAGES;
  forced = true;
}

void readKeys() {
  M5Cardputer.update();
  if (M5Cardputer.BtnA.wasPressed()) {
    brightness = (brightness + 1) % BRIGHTNESS_STEPS;
    M5Cardputer.Display.setBrightness(BRIGHTNESS[brightness]);
  }
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();
  for (char c : state.word) {
    if (c >= '1' && c < '1' + PAGES) {
      nextPage(c - '1');
      return;
    }
  }
  nextPage(page + 1);
}

}  // namespace

void begin() {
  auto cfg = M5.config();
  // Only the screen, the keyboard and the battery gauge are used.
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(BRIGHTNESS[brightness]);
  canvas.setColorDepth(8);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setFont(&fonts::Font0);
  canvas.setTextWrap(false);
}

bool due() {
  uint32_t now = millis();
  if (now - lastKeys >= KEYS_MS) {
    lastKeys = now;
    readKeys();
  }
  return forced || now - lastDraw >= REDRAW_MS;
}

void update(const Status &s) {
  uint32_t now = millis();
  lastDraw = now;

  if (s.lineOut.bytes + s.lineIn.bytes != seenBytes) {
    seenBytes = s.lineOut.bytes + s.lineIn.bytes;
    lastByteAt = now;
  }
  if (now - lastRate >= RATE_MS) {
    toRate.take(s.toCalc, now - lastRate);
    fromRate.take(s.fromCalc, now - lastRate);
    outRate.take(s.lineOut, now - lastRate);
    inRate.take(s.lineIn, now - lastRate);
    lastRate = now;
  }
  if (battery < 0 || now - lastBattery >= BATTERY_MS) {
    battery = M5Cardputer.Power.getBatteryLevel();
    lastBattery = now;
  }

  lineCount = 0;
  add(1, TFT_WHITE, "%-6s %d/%d                  battery %3d%%", PAGE_NAMES[page], page + 1, PAGES, battery);
  if (page == 0) statusPage(s);
  else if (page == 1) linkPage(s);
  else if (page == 2) logPage();
  else buildPage();

  if (forced || lineCount != drawnCount || memcmp(lines, drawn, sizeof(Line) * lineCount)) {
    draw();
    memcpy(drawn, lines, sizeof(Line) * lineCount);
    drawnCount = lineCount;
    forced = false;
  }
}

void log(const char *text) {
  if (logCount == LOG_LINES) {
    memmove(logLines, logLines + 1, sizeof(LogLine) * (LOG_LINES - 1));
    logCount--;
  }
  LogLine &line = logLines[logCount++];
  line.at = millis() / 1000;
  snprintf(line.text, sizeof(line.text), "%s", text);
}

}  // namespace Display

#endif
