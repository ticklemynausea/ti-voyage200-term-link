/*
 * tlink.c - see tlink.h
 */
#include <string.h>
#include "tlink.h"

#define CTL_SEQ 0x40

/* CRC-16/CCITT a nibble at a time: a 16-entry table is small enough for the
   calculator and still quick. */
static const unsigned short CRC_TABLE[16] = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
  0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
};

static unsigned short crcByte(unsigned short crc, unsigned char b)
{
  crc = (unsigned short)((crc << 4) ^ CRC_TABLE[((crc >> 12) ^ (b >> 4)) & 15]);
  crc = (unsigned short)((crc << 4) ^ CRC_TABLE[((crc >> 12) ^ b) & 15]);
  return crc;
}

/* a is later than b, on a clock that wraps */
static short after(unsigned long a, unsigned long b)
{
  return (long)(a - b) > 0;
}

static unsigned char inFlight(const tl_link *t)
{
  return (unsigned char)((t->next - t->base) & 7);
}

static void resetSeq(tl_link *t)
{
  t->base = t->next = t->resend = t->expect = 0;
  t->waitPeer = 0;
  t->ackDue = 0;
  t->txLen = t->txPos = 0;
  t->txSeq = 0;
}

void tl_init(tl_link *t, unsigned char role, unsigned short rtoMs, unsigned short keepaliveMs)
{
  memset(t, 0, sizeof(*t));
  t->role = role;
  t->rto = rtoMs;
  t->keepalive = keepaliveMs;
  t->rxOpen = 1;
}

void tl_set_id(tl_link *t, const unsigned char *id, short len)
{
  if (len > TL_MAX_ID) len = TL_MAX_ID;
  memcpy(t->id, id, len);
  t->idLen = (unsigned char)len;
}

void tl_hello(tl_link *t)
{
  resetSeq(t);
  t->started = 0;
  t->ctlDue = TL_HELLO;
}

short tl_send(tl_link *t, unsigned char type, const void *data, short len)
{
  tl_frame *f;
  if (len < 0 || len > TL_MAX_PAYLOAD || inFlight(t) >= TL_WINDOW) return 0;
  f = &t->win[t->next & (TL_WINDOW - 1)];
  f->type = type;
  f->len = (unsigned char)len;
  memcpy(f->data, data, len);
  t->next = (t->next + 1) & 7;
  return 1;
}

short tl_window_free(const tl_link *t)
{
  return TL_WINDOW - inFlight(t);
}

short tl_all_acked(const tl_link *t)
{
  return t->base == t->next;
}

short tl_tx_idle(const tl_link *t)
{
  return t->txPos >= t->txLen && t->resend == t->next;
}

short tl_heard_within(const tl_link *t, unsigned long now, unsigned long ms)
{
  return t->heardOnce && now - t->heardAt < ms;
}

/* Sends again from the oldest frame not yet acknowledged. */
static void goBack(tl_link *t, unsigned long now)
{
  t->framesResent += (t->resend - t->base) & 7;
  t->resend = t->base;
  t->waitPeer = 0;
  t->progressAt = now;
}

// ---------- sending ----------

static void put(tl_link *t, unsigned char b)
{
  if (b == TL_FLAG || b == TL_ESC) {
    t->tx[t->txLen++] = TL_ESC;
    b ^= 0x20;
  }
  t->tx[t->txLen++] = b;
}

static void build(tl_link *t, unsigned char ctl, unsigned char type, const unsigned char *data, short len)
{
  unsigned short crc = 0xFFFF;
  short i;

  t->txLen = t->txPos = 0;
  t->tx[t->txLen++] = TL_FLAG;
  put(t, ctl);
  crc = crcByte(crc, ctl);
  put(t, type);
  crc = crcByte(crc, type);
  put(t, (unsigned char)len);
  crc = crcByte(crc, (unsigned char)len);
  for (i = 0; i < len; i++) {
    put(t, data[i]);
    crc = crcByte(crc, data[i]);
  }
  put(t, (unsigned char)(crc & 0xFF));
  put(t, (unsigned char)(crc >> 8));
  t->tx[t->txLen++] = TL_FLAG;
  t->ackDue = 0;  // every frame carries the ack
}

/* Puts the next frame in t->tx. Returns 0 if there is nothing to send. */
static short fill(tl_link *t, unsigned long now)
{
  unsigned char ack = (unsigned char)(t->expect << 3);
  unsigned char type = t->ctlDue;

  t->txSeq = 0;
  if (type) {
    t->ctlDue = 0;
    if (type == TL_HELLO) t->helloAt = now;
    build(t, ack, type, t->id, type == TL_HELLO_ACK ? t->idLen : 0);
    return 1;
  }
  if (t->started && !t->hold && !t->waitPeer && t->resend != t->next) {
    const tl_frame *f = &t->win[t->resend & (TL_WINDOW - 1)];
    build(t, (unsigned char)(CTL_SEQ | ack | t->resend), f->type, f->data, f->len);
    t->resend = (t->resend + 1) & 7;
    t->txSeq = 1;
    t->framesSent++;
    return 1;
  }
  if (t->ackDue) {
    build(t, ack, TL_ACK, 0, 0);
    return 1;
  }
  return 0;
}

short tl_tx_next(tl_link *t, unsigned long now)
{
  if (t->txPos >= t->txLen && !fill(t, now)) return -1;
  t->lastTx = now;
  // A frame that is slow to go out (the receiver not taking it) is not late.
  if (t->txSeq) t->progressAt = now;
  return t->tx[t->txPos++];
}

void tl_tx_abort(tl_link *t)
{
  t->txPos = t->txLen;
}

void tl_poll(tl_link *t, unsigned long now)
{
  short between = t->txPos >= t->txLen;  // not in the middle of a frame

  if (!t->started) {
    if (t->role == TL_INITIATOR && !t->ctlDue && between && now - t->helloAt >= t->rto)
      t->ctlDue = TL_HELLO;
    return;
  }

  // Everything sent, not all of it acknowledged, and no progress for a while.
  if (inFlight(t) && t->resend == t->next && !t->waitPeer && between &&
      now - t->progressAt >= t->rto) {
    if (t->heardOnce && after(t->heardAt, t->progressAt)) {
      goBack(t, now);  // the other side is there: the frame was lost
    } else {
      // Silence. The line may have slipped out of step, which only quiet
      // cures: wait for the other side to speak.
      t->waitPeer = 1;
      if (t->role == TL_INITIATOR && !t->ctlDue) t->ctlDue = TL_PING;
    }
  }

  if (t->role == TL_INITIATOR && t->keepalive && !t->ctlDue && between &&
      now - t->lastTx >= t->keepalive)
    t->ctlDue = TL_PING;
}

// ---------- receiving ----------

/* A whole frame is in t->rx. */
static short frame(tl_link *t, unsigned long now)
{
  unsigned short crc = 0xFFFF;
  unsigned char ctl, type, ack, n, out;
  short len = t->rxLen - 5, i;

  for (i = 0; i < t->rxLen - 2; i++) crc = crcByte(crc, t->rx[i]);
  ctl = t->rx[0];
  type = t->rx[1];
  if (crc != (unsigned short)(t->rx[t->rxLen - 2] | (t->rx[t->rxLen - 1] << 8)) ||
      t->rx[2] != len || (ctl & 0x80) || !type || (type > TL_PING && type < TL_DATA) || type > TL_SHOT) {
    t->framesBad++;
    return 0;
  }
  t->heardOnce = 1;
  t->heardAt = now;

  if (t->role == TL_RESPONDER) {
    if (type == TL_HELLO) {
      resetSeq(t);
      t->started = 1;
      t->progressAt = now;
      t->ctlDue = TL_HELLO_ACK;
      t->in.type = TL_HELLO;
      t->in.len = 0;
      return TL_HELLO;
    }
    if (!t->started) {
      t->ctlDue = TL_WHO;
      return 0;
    }
  } else {
    if (type == TL_HELLO_ACK) {
      if (t->started) return 0;  // a second answer to the same HELLO
      t->started = 1;
      t->progressAt = now;
      t->in.type = type;
      t->in.len = (unsigned char)len;
      memcpy(t->in.data, t->rx + 3, len);
      return TL_HELLO_ACK;
    }
    if (type == TL_WHO) {
      if (t->started) tl_hello(t);  // the ESP32 has restarted
      return 0;
    }
    if (!t->started) return 0;  // from before the HELLO
  }

  if (t->waitPeer) goBack(t, now);

  // The ack: the other side expects ack next, so everything before it arrived.
  ack = (ctl >> 3) & 7;
  n = (unsigned char)((ack - t->base) & 7);
  out = inFlight(t);
  if (n && n <= out) {
    if (((t->resend - t->base) & 7) < n) t->resend = ack;
    t->base = ack;
    t->progressAt = now;
  } else if (!n && out && type == TL_ACK && t->resend == t->next) {
    goBack(t, now);  // it skipped one: a frame was lost on the way
  }

  if (ctl & CTL_SEQ) {
    if (!t->rxOpen) return 0;  // not now: it comes again later
    t->ackDue = 1;
    if ((ctl & 7) != t->expect) return 0;
    t->expect = (t->expect + 1) & 7;
    t->in.type = type;
    t->in.len = (unsigned char)len;
    memcpy(t->in.data, t->rx + 3, len);
    return type;
  }
  if (type == TL_PING) t->ackDue = 1;
  return 0;
}

short tl_rx_byte(tl_link *t, unsigned char b, unsigned long now)
{
  short r = 0;

  if (b == TL_FLAG) {
    if (t->rxLen >= 5 && !t->rxBad && !t->rxEsc) r = frame(t, now);
    else if (t->rxLen) t->framesBad++;
    t->rxLen = 0;
    t->rxEsc = t->rxBad = 0;
    return r;
  }
  if (t->rxBad) return 0;
  if (b == TL_ESC) {
    if (t->rxEsc) t->rxBad = 1;
    t->rxEsc = 1;
    return 0;
  }
  if (t->rxEsc) {
    b ^= 0x20;
    t->rxEsc = 0;
    if (b != TL_FLAG && b != TL_ESC) {
      t->rxBad = 1;
      return 0;
    }
  }
  if (t->rxLen >= (short)sizeof(t->rx)) {
    t->rxBad = 1;
    return 0;
  }
  t->rx[t->rxLen++] = b;
  return 0;
}
