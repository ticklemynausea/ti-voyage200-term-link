/*
 * Display.h - status pages on the M5Stack Cardputer's screen
 *
 * Three pages (status, link, log), switched from the Cardputer's keyboard:
 * any key goes to the next page, 1 2 3 jump to one. Nothing typed there is
 * sent anywhere.
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
  uint32_t toCalc, fromCalc, dropped, resends, errors;
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
