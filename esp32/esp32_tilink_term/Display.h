/*
 * Display.h - status pages on the M5Stack Cardputer's screen
 *
 * Four pages (status, link, log, build), switched from the Cardputer's
 * keyboard: any key goes to the next page, 1 to 4 jump to one. Nothing
 * typed there is sent anywhere. The G0 button steps the backlight through
 * 0, 25, 50, 75 and 100%.
 *
 * Built for any other board, everything here is empty, and the sketch needs
 * no display library.
 */
#pragma once
#include <Arduino.h>

namespace Display {

enum Mode : uint8_t { MODE_MENU, MODE_SERIAL, MODE_SSH };

struct Status {
  Mode mode;
  bool asking;            // waiting for an answer typed on the calculator
  bool computerAttached;  // a program on the computer holds the serial port
  bool calcPresent;       // the lines are not both held low
  bool calcHeard;         // its program said something lately
  bool linesIdle;
  // What crossed the link, each in bytes and frames. The bytes are the
  // frames' contents, except on the line and in bad frames, which count
  // every byte as it went over the wire.
  struct Count { uint32_t bytes, frames; };
  Count toCalc, fromCalc;  // the frames' contents, each frame once
  Count lineOut, lineIn;   // everything, acknowledgements and framing too
  Count resent;            // frames sent again
  Count bad;               // frames from the calculator that failed their check
  Count dropped;           // output given up on, never sent
  uint32_t handshakeErrors;  // bytes whose bit handshake failed
  String sshTarget;       // user@host, empty when not set up
  String wifiName;        // empty when not set up
  bool wifiUp;
  int rssi;
  String ip;
};

#if defined(ARDUINO_M5STACK_CARDPUTER)

void begin();

// True when update() has something to do: a page key was pressed, or enough
// time has passed. Cheap, so it can be asked between any two bytes.
bool due();

// Redraws the page if what it shows has changed.
void update(const Status &status);

// Adds a line to the log page.
void log(const char *text);

#else

inline void begin() {}
inline bool due() { return false; }
inline void update(const Status &) {}
inline void log(const char *) {}

#endif

}  // namespace Display
