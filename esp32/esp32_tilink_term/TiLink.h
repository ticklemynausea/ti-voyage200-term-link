/*
 * TiLink.h - TI-89 / TI-92+ / Voyage 200 link-port driver for ESP32 (and other Arduinos)
 *
 * The TI bit-level handshake on the two link wires (tip + ring):
 * sendByte / recvByte. The terminal sketch uses it as a raw byte pipe.
 *
 * Wiring (Voyage 200 lines idle at 3.3 V, so an ESP32 connects directly):
 *   jack tip  (red)   -> 220 ohm -> tipPin
 *   jack ring (white) -> 220 ohm -> ringPin
 *   jack sleeve       -> GND
 * If your calculator's lines idle at about 5 V, use a bidirectional level shifter.
 */
#pragma once
#include <Arduino.h>

class TiLink {
public:
  // internalPullups: use INPUT_PULLUP when releasing a line.
  // Leave false when a level-shifter board (which has its own pull-ups) is fitted.
  TiLink(uint8_t tipPin, uint8_t ringPin, bool internalPullups = false);

  void begin();

  // Returns true on success. On failure the lines are released and error count bumped.
  bool sendByte(uint8_t b);
  // firstBitTimeoutUs = 0 -> just poll.
  // Returns 0..255 on success, -1 if nothing started, -2 on a mid-byte error.
  int recvByte(uint32_t firstBitTimeoutUs);

  bool lineIdle();

  // ---- tuning / stats ----
  uint32_t edgeTimeoutUs = 100000;   // max wait for any handshake edge
  uint32_t ackSettleUs = 50;         // time allowed for our released ack line to rise
  uint32_t errors = 0;

  // ---- diagnostics: details of the most recent failure ----
  // stage: 1 = recv, line went idle/timed out between bits
  //        2 = recv, both lines low at the start of a bit
  //        3 = recv, sender never released its line after our ack
  //        4 = (unused; a low ack line after release is the next bit)
  //        5 = send, lines not idle before a bit
  //        6 = send, receiver never acked
  //        7 = send, receiver never released its ack
  uint8_t lastErrStage = 0;
  uint8_t lastErrBit = 0;      // bit index 0..7 (LSB first)
  uint8_t lastErrValue = 0;    // bits collected/remaining at the time
  uint8_t lastErrTip = 1, lastErrRing = 1;  // line states when it failed
  const char *lastErrText();

private:
  void noteErr(uint8_t stage, uint8_t bit, uint8_t value);
  uint8_t _tip, _ring;
  bool _pullups;

  void pullLow(uint8_t pin);
  void release(uint8_t pin);
  bool isLow(uint8_t pin);
  bool waitFor(uint8_t pin, bool wantLow, uint32_t timeoutUs);
  bool fail();
};
