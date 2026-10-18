/*
 * tlink.h - the link layer between calc/term and esp32_tilink_term
 *
 * Plain C, shared by both: the calculator builds it with TIGCC (where int is
 * 16 bits, so only char, short and long are used) and the ESP32 sketch takes
 * it as an Arduino library. It does no I/O of its own: the caller hands it
 * the bytes that arrive, takes the bytes to send, and tells it the time in
 * milliseconds.
 *
 * Frames, delimited and byte-stuffed as in SLIP:
 *   FLAG | ctl | type | len | payload (len bytes, at most TL_MAX_PAYLOAD) | crc lo | crc hi | FLAG
 * FLAG (0xC0) and ESC (0xC1) can never appear in UTF-8, so terminal text
 * almost never needs stuffing; when one of them is in a frame it goes as
 * ESC, byte ^ 0x20. The CRC is CRC-16/CCITT (initial 0xFFFF) over ctl, type,
 * len and payload. A frame that is cut short, fails its CRC, has the wrong
 * length or an unknown type is dropped, and the next FLAG starts afresh, so
 * garbage on the line never gets further. The length is there because a
 * replug that slips the line makes a lot of garbage, and the CRC alone would
 * let one garbled frame in 65536 through.
 *
 * ctl: bits 0-2 seq, bits 3-5 ack (the next seq expected from the other
 * side), bit 6 set on a sequenced frame. Every frame carries an ack.
 *
 * Sequenced frames are delivered once each and in order, by go-back-N with a
 * window of TL_WINDOW frames: the receiver takes only the seq it expects, and
 * the sender sends again from the oldest frame not yet acknowledged when it
 * hears that one was skipped, or after rto ms without progress. The sender
 * resends straight away only if the other side has spoken since it last sent;
 * otherwise it waits for it to speak, so a link that has slipped out of step
 * gets the quiet it needs to recover.
 *
 * The calculator starts the link (TL_INITIATOR): HELLO, answered by HELLO_ACK
 * with the ESP32's id, resets both sides' sequence numbers. Each HELLO
 * carries a number of its own, which the answer repeats: a HELLO sent again
 * because its answer was slow only gets the answer again, not a second
 * reset, and an answer to an older HELLO is not taken. The ESP32
 * (TL_RESPONDER) answers anything before a HELLO with WHO, which makes the
 * calculator say HELLO again: that is how a restarted ESP32 is noticed. The
 * initiator also sends PING when it has sent nothing for keepalive ms, which
 * the responder answers with ACK, so each side keeps hearing the other.
 */
#ifndef TLINK_H
#define TLINK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bigger frames spend less of the line on framing and acknowledgements:
   64 gave 2013 B/s on /burst. The length byte allows up to 255. */
#define TL_MAX_PAYLOAD 128
#define TL_WINDOW 4
#define TL_FLAG 0xC0
#define TL_ESC 0xC1
#define TL_MAX_ID 4

/* Frame types. Those below TL_DATA are not sequenced. */
enum {
  TL_ACK = 1,     /* nothing but the ack */
  TL_HELLO,       /* calc -> ESP32: starts the link afresh; the HELLO's number (2 bytes) */
  TL_HELLO_ACK,   /* ESP32 -> calc: the answer: that number, then the ESP32's id */
  TL_WHO,         /* ESP32 -> calc: no HELLO yet since the ESP32 started */
  TL_PING,        /* calc -> ESP32: keepalive, answered by ACK */
  TL_DATA = 0x10, /* terminal bytes: keys, or output */
  TL_CMD,         /* calc -> ESP32: a command letter, and a slot for some */
  TL_STATUS,      /* ESP32 -> calc: '+' or '-' for Wi-Fi, and a line of text */
  TL_LIST,        /* ESP32 -> calc: list letter, entry number, entry text */
  TL_SHOT,        /* calc -> ESP32: piece number, then that piece of the screen */
  TL_MEM,         /* calc -> ESP32: its free memory, see TL_MEM_SIZE */
  TL_LAST = TL_MEM
};

/* TL_MEM's payload: unsigned numbers, least significant byte first. */
#define TL_MEM_HEAP 0       /* 4 bytes: free RAM in the heap, all blocks together */
#define TL_MEM_HANDLES 4    /* 2: free handles */
#define TL_MEM_ARCHIVE 6    /* 4: free archive memory */
#define TL_MEM_GC 10        /* 4: archive memory a garbage collection would free */
#define TL_MEM_ARCHIVED 14  /* 4: archive memory in use */
#define TL_MEM_SIZE 18

enum { TL_INITIATOR, TL_RESPONDER };

typedef struct {
  unsigned char type, len;
  unsigned char data[TL_MAX_PAYLOAD];
} tl_frame;

typedef struct {
  /* settings */
  unsigned char role;
  unsigned short rto;        /* ms without progress before resending */
  unsigned short keepalive;  /* initiator: ms of silence before a PING; 0 for none */

  /* state of the link */
  unsigned char started;     /* initiator: HELLO answered; responder: HELLO heard */
  unsigned char rxOpen;      /* sequenced frames are taken (the caller sets it) */
  unsigned char hold;        /* sequenced frames are not sent for now (the caller sets it) */
  unsigned char id[TL_MAX_ID], idLen;  /* responder: its id, sent in HELLO_ACK */
  unsigned short hello;      /* initiator: the number of its HELLO; responder: of the last one heard.
                                The initiator's caller seeds it after tl_init, so that a new run
                                does not repeat the number of the run before. */

  /* sending */
  tl_frame win[TL_WINDOW];
  unsigned char base;        /* the oldest seq not yet acknowledged */
  unsigned char next;        /* the seq of the next frame queued */
  unsigned char resend;      /* the next seq to put on the line */
  unsigned char waitPeer;    /* a resend waits until the other side speaks */
  unsigned char ackDue;
  unsigned char ctlDue;      /* an unsequenced frame to send next, or 0 */
  unsigned long progressAt;  /* the last frame sent, or the last ack that moved base */
  unsigned long lastTx;      /* the last byte handed out */
  unsigned long helloAt;
  unsigned char tx[2 * (TL_MAX_PAYLOAD + 5) + 2];
  short txLen, txPos;
  unsigned char txSeq;       /* the frame being handed out is sequenced */
  unsigned char sentUpTo;    /* the seq after the last one ever put on the line */

  /* receiving */
  unsigned char expect;      /* the seq of the next frame to take */
  unsigned char rx[TL_MAX_PAYLOAD + 5];
  short rxLen;
  short rxCount;             /* bytes of the frame so far, also those not kept */
  unsigned char rxEsc, rxBad;
  unsigned char heardOnce;
  unsigned long heardAt;     /* the last good frame */
  tl_frame in;               /* the frame tl_rx_byte returned */

  /* statistics, each in frames and in bytes: the payload's bytes, except
     for bad frames and the line's, which count the bytes as on the wire */
  unsigned long framesSent, bytesSent;      /* sequenced frames sent, the first time */
  unsigned long framesResent, bytesResent;  /* sequenced frames sent again */
  unsigned long framesRecv, bytesRecv;      /* sequenced frames taken from the other side */
  unsigned long framesBad, bytesBad;        /* dropped: cut short, failed CRC, wrong length or type */
  unsigned long lineFramesOut, lineBytesOut;  /* all frames handed out, acknowledgements too */
  unsigned long lineFramesIn, lineBytesIn;    /* all good frames and all bytes taken in */
} tl_link;

void tl_init(tl_link *t, unsigned char role, unsigned short rtoMs, unsigned short keepaliveMs);

/* Responder: the id sent in every HELLO_ACK, up to TL_MAX_ID bytes. */
void tl_set_id(tl_link *t, const unsigned char *id, short len);

/* Initiator: starts the link afresh, with a new HELLO number. Whatever is
   queued is dropped, also a frame half handed out; nothing sequenced goes
   out until HELLO_ACK. */
void tl_hello(tl_link *t);

/* Queues a sequenced frame. Returns 0 if the window is full or len too big. */
short tl_send(tl_link *t, unsigned char type, const void *data, short len);

/* How many more frames tl_send would take. */
short tl_window_free(const tl_link *t);

/* Nothing is queued or waiting for an ack. */
short tl_all_acked(const tl_link *t);

/* Nothing is being handed out and no sequenced frame is waiting to be: the
   line would go quiet now. */
short tl_tx_idle(const tl_link *t);

/* Takes one byte from the line. Returns the type of a frame for the caller,
   with t->in holding it, or 0. The caller gets the sequenced frames (each
   once, in order), HELLO_ACK on the initiator (with only the id in t->in)
   and a new link's HELLO on the responder. */
short tl_rx_byte(tl_link *t, unsigned char b, unsigned long now);

/* Timers: resends, the initiator's HELLO and PING. Call it often. */
void tl_poll(tl_link *t, unsigned long now);

/* The next byte to put on the line, or -1 if there is none. Ask only when
   the byte can be sent, since it counts as gone. */
short tl_tx_next(tl_link *t, unsigned long now);

/* Gives up on the rest of the frame being handed out. Its receiver drops
   what it got, and a sequenced frame is sent again later. */
void tl_tx_abort(tl_link *t);

/* A good frame came from the other side within the last ms. */
short tl_heard_within(const tl_link *t, unsigned long now, unsigned long ms);

#ifdef __cplusplus
}
#endif

#endif
