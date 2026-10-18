/*
 * test_link.c - runs tlink between a simulated calculator and ESP32
 *
 *   make link:test      (from the repo root)
 *   build/test_link 7   the same with another random seed
 *
 * Each scenario streams bytes both ways for a stretch of simulated time,
 * 1 ms per step, over a line that can corrupt and drop bytes, be unplugged,
 * and slip by a bit after a replug the way the calculator's link does: every
 * byte after the slip then reads shifted, and the calculator cannot send,
 * until the line from the ESP32 has been quiet for a while. Every byte that
 * arrives must be the next one of its stream, and at the end, with the line
 * clean again, everything sent must have arrived.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlink.h"

#define RATE 3            /* bytes per ms each way, about the real link's speed */
#define SLIP_QUIET 1500   /* ms of quiet that put a slipped line back in step */
#define HEARD_MS 5000     /* the ESP32 sends only while it has heard the calculator */
#define KEYS_MAX 16       /* the calculator's frames are short, the ESP32's full */

static unsigned long seed, firstSeed = 12345;
static unsigned long rnd(void)
{
  seed = seed * 1103515245UL + 12345UL;
  return (seed >> 16) & 0x7FFF;
}
static int chance(double p) { return rnd() < p * 32768.0; }

/* The bytes of a stream, with runs of FLAG and ESC in them. */
static unsigned char streamByte(int dir, unsigned long i)
{
  if ((i / 97) % 7 == 0) return (unsigned char)(TL_FLAG + (i & 1));
  return (unsigned char)((i * 2654435761UL + dir * 0x9E37UL) >> 11);
}

typedef struct {
  const char *name;
  unsigned long ms;
  double corrupt, drop;   /* per byte */
  unsigned long unplugEvery, unplugMax;  /* mean ms between unplugs, longest unplug */
  double slipOnReplug;
  unsigned long closeEvery, closeFor;    /* the calculator's receive window shut */
  unsigned long restartAt;               /* the ESP32 restarts */
} scenario;

typedef struct {
  tl_link calc, esp;
  unsigned long now;
  unsigned long sentUp, gotUp;      /* calculator -> ESP32 stream */
  unsigned long sentDown, gotDown;  /* ESP32 -> calculator stream */
  unsigned long totalUp, totalDown;
  int unplugged, slipped;
  unsigned long unplugUntil, lastDown, closedUntil;
  unsigned shiftReg;
  int helloAcks, restartSeen;
  unsigned char firstId[TL_MAX_ID];
  int failed;
} sim;

static void fail(sim *s, const char *what, unsigned long at, int want, int got)
{
  if (!s->failed)
    printf("    FAIL at %lu ms: %s at byte %lu: want %02X got %02X\n", s->now, what, at, want, got);
  s->failed = 1;
}

static void calcGot(sim *s, short type)
{
  short i;
  if (type == TL_HELLO_ACK) {
    if (s->helloAcks++ == 0) memcpy(s->firstId, s->calc.in.data, TL_MAX_ID);
    else if (memcmp(s->firstId, s->calc.in.data, s->calc.in.len)) s->restartSeen = 1;
    s->sentUp = 0;   /* a new link: both streams start again */
    s->gotDown = 0;
  } else if (type == TL_DATA) {
    for (i = 0; i < s->calc.in.len; i++, s->gotDown++) {
      unsigned char want = streamByte(1, s->gotDown);
      if (s->calc.in.data[i] != want) fail(s, "calculator got a wrong byte", s->gotDown, want, s->calc.in.data[i]);
    }
  }
}

static void espGot(sim *s, short type)
{
  short i;
  if (type == TL_HELLO) {
    s->sentDown = 0;
    s->gotUp = 0;
  } else if (type == TL_DATA) {
    for (i = 0; i < s->esp.in.len; i++, s->gotUp++) {
      unsigned char want = streamByte(0, s->gotUp);
      if (s->esp.in.data[i] != want) fail(s, "ESP32 got a wrong byte", s->gotUp, want, s->esp.in.data[i]);
    }
  }
}

static void startEsp(sim *s, const char *id)
{
  tl_init(&s->esp, TL_RESPONDER, 500, 0);
  tl_set_id(&s->esp, (const unsigned char *)id, 2);
}

/* One ms. impair = 0 once the scenario is over and the line is clean. */
static void step(sim *s, const scenario *sc, int impair)
{
  unsigned char buf[TL_MAX_PAYLOAD];
  short i, len, b, type;

  s->now++;
  tl_poll(&s->calc, s->now);
  tl_poll(&s->esp, s->now);

  if (impair && sc->unplugEvery && !s->unplugged && chance(1.0 / sc->unplugEvery)) {
    s->unplugged = 1;
    s->unplugUntil = s->now + 1 + rnd() * (unsigned long)rnd() % sc->unplugMax;
  }
  if (s->unplugged && (s->now >= s->unplugUntil || !impair)) {
    s->unplugged = 0;
    if (impair && chance(sc->slipOnReplug)) {
      s->slipped = 1;  /* the quiet that cures it starts now */
      s->lastDown = s->now;
    }
  }
  if (s->slipped && s->now - s->lastDown >= SLIP_QUIET) s->slipped = 0;
  if (impair && sc->closeEvery && s->now >= s->closedUntil && chance(1.0 / sc->closeEvery))
    s->closedUntil = s->now + sc->closeFor;
  s->calc.rxOpen = s->now >= s->closedUntil;
  if (impair && sc->restartAt && s->now == sc->restartAt) startEsp(s, "QZ");

  /* What each side's program gives the link. */
  if (s->calc.started && s->sentUp < s->totalUp && tl_window_free(&s->calc)) {
    len = 1 + rnd() % KEYS_MAX;
    for (i = 0; i < len; i++) buf[i] = streamByte(0, s->sentUp + i);
    tl_send(&s->calc, TL_DATA, buf, len);
    s->sentUp += len;
  }
  if (s->esp.started && s->sentDown < s->totalDown && tl_window_free(&s->esp)) {
    len = 1 + rnd() % TL_MAX_PAYLOAD;
    for (i = 0; i < len; i++) buf[i] = streamByte(1, s->sentDown + i);
    tl_send(&s->esp, TL_DATA, buf, len);
    s->sentDown += len;
  }

  /* Calculator -> ESP32. A slipped calculator cannot send. */
  for (i = 0; i < RATE && !s->slipped; i++) {
    if ((b = tl_tx_next(&s->calc, s->now)) < 0) break;
    if (s->unplugged) continue;
    if (impair && chance(sc->drop)) continue;
    if (impair && chance(sc->corrupt)) b ^= 1 << (rnd() % 8);
    if ((type = tl_rx_byte(&s->esp, (unsigned char)b, s->now))) espGot(s, type);
  }

  /* ESP32 -> calculator, only while it hears the calculator. */
  for (i = 0; i < RATE && tl_heard_within(&s->esp, s->now, HEARD_MS); i++) {
    if ((b = tl_tx_next(&s->esp, s->now)) < 0) break;
    if (s->unplugged) continue;
    s->lastDown = s->now;
    if (s->slipped) {
      s->shiftReg = (s->shiftReg << 8) | (unsigned)b;
      b = (s->shiftReg >> 1) & 0xFF;
    } else {
      if (impair && chance(sc->drop)) continue;
      if (impair && chance(sc->corrupt)) b ^= 1 << (rnd() % 8);
    }
    if ((type = tl_rx_byte(&s->calc, (unsigned char)b, s->now))) calcGot(s, type);
  }
}

static int run(const scenario *sc)
{
  static sim s;
  unsigned long settle;

  memset(&s, 0, sizeof(s));
  seed = firstSeed;
  tl_init(&s.calc, TL_INITIATOR, 500, 2000);
  startEsp(&s, "AB");
  tl_hello(&s.calc);
  s.totalUp = s.totalDown = (unsigned long)-1;  /* stream for the whole scenario */

  while (s.now < sc->ms && !s.failed) step(&s, sc, 1);

  /* Then the line is clean, and what is under way must all arrive. */
  s.totalUp = s.sentUp;
  s.totalDown = s.sentDown;
  for (settle = 0; settle < 60000 && !s.failed; settle++) {
    step(&s, sc, 0);
    if (s.gotUp == s.totalUp && s.gotDown == s.totalDown &&
        tl_all_acked(&s.calc) && tl_all_acked(&s.esp)) break;
  }
  if (!s.failed && (s.gotUp != s.totalUp || s.gotDown != s.totalDown)) {
    printf("    FAIL: not all delivered: up %lu of %lu, down %lu of %lu\n",
           s.gotUp, s.totalUp, s.gotDown, s.totalDown);
    s.failed = 1;
  }
  if (!s.failed && sc->restartAt && !s.restartSeen) {
    printf("    FAIL: the ESP32's restart was not noticed\n");
    s.failed = 1;
  }
  if (!s.failed && !sc->restartAt && s.helloAcks != 1) {
    printf("    FAIL: %d HELLO_ACKs, expected 1\n", s.helloAcks);
    s.failed = 1;
  }

  printf("%-5s %-28s down %7lu B/s, up %5lu B/s; resent %lu+%lu, bad %lu+%lu of %lu+%lu frames\n",
         s.failed ? "FAIL" : "ok", sc->name,
         s.gotDown * 1000 / s.now, s.gotUp * 1000 / s.now,
         s.esp.framesResent, s.calc.framesResent, s.calc.framesBad, s.esp.framesBad,
         s.esp.framesSent, s.calc.framesSent);
  return s.failed;
}

int main(int argc, char **argv)
{
  static const scenario scenarios[] = {
    /* name                ms       corrupt drop   unplug        slip  closed        restart */
    { "clean line",        60000,   0,      0,     0,     0,     0,    0,     0,     0 },
    { "noisy line",        300000,  1e-3,   1e-3,  0,     0,     0,    0,     0,     0 },
    { "very noisy line",   300000,  1e-2,   5e-3,  0,     0,     0,    0,     0,     0 },
    { "replugs",           900000,  0,      0,     15000, 30000, 0,    0,     0,     0 },
    { "replugs, slipping", 900000,  0,      0,     15000, 30000, 1.0,  0,     0,     0 },
    { "quick replugs",     900000,  0,      0,     8000,  300,   1.0,  0,     0,     0 },
    { "window shut",       300000,  0,      0,     0,     0,     0,    10000, 20000, 0 },
    { "ESP32 restart",     60000,   0,      0,     0,     0,     0,    0,     0,     30000 },
    { "everything",        1800000, 1e-3,   1e-3,  20000, 20000, 0.5,  15000, 10000, 0 },
  };
  unsigned i;
  int failures = 0;

  if (argc > 1) firstSeed = strtoul(argv[1], 0, 10);  /* another run of the same scenarios */
  for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) failures += run(&scenarios[i]);
  printf(failures ? "%d scenario(s) failed\n" : "all passed\n", failures);
  return failures != 0;
}
