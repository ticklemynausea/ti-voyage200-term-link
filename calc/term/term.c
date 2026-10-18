// A dumb terminal for the TI Voyage 200, built with TIGCC / GCC4TI.
//
// 60 x 21 characters in a 4x6 cell, with a subset of the VT220 escape codes.
// Keys go out of the link port, and whatever comes in is drawn, in the frames
// of the link layer in link/src/tlink.c, which is built into this program.
// The other end is the ESP32 running esp32/esp32_tilink_term, which the menu
// tells where to carry the bytes: to its USB serial port or to an SSH host.
//
// Keys in a session:
//   diamond + letter   Ctrl + letter
//   diamond + up/down  Page Up / Page Down
//   diamond + left/right  a word back / forward (Alt-B / Alt-F)
//   diamond + space    Ctrl-Space (NUL)
//   diamond + F1 to F4 F9 to F12
//   shift + Hand       Meta: the next key goes out after an ESC (MODE too)
//   APPS, CLEAR, LN    Tab, Ctrl-U, Ctrl-R
//   SIN, COS, TAN      tmux: previous window, next window, the prefix
//   2nd + SIN/COS/TAN  tmux: window list, last window, new window
//   dia. + SIN/COS/TAN tmux: a page back, paste, copy mode
//   diamond + APPS     Shift-Tab; diamond + CLEAR resets this screen
//   2nd / diamond + LN Ctrl-_ (undo) / Ctrl-backslash
//   diamond + MODE     Ctrl-]; 2nd + ^ types ~
//   Hand + arrow       Ctrl + arrow; shift + arrow sends Shift + arrow
//   2nd + up/down      Home / End (2nd + left/right too)
//   2nd + backspace    Insert
//   2nd + plus (CHAR)  symbols the keyboard lacks: pick one by its letter
//   2nd + ESC (QUIT)   back to the menu; diamond + ESC does the same
//   2nd + APPS         sends the screen to the computer, as a screenshot

#define USE_V200
#define MIN_AMS 207
#define SAVE_SCREEN

#include <tigcclib.h>
#include "font.h"
#include "help.h"
#include "version.h"
#include "tlink.h"

#define COLS 60
#define ROWS 21
#define CELL_H 6
#define LCD_ROW_BYTES 30
#define TEXT_BYTES (ROWS * CELL_H * LCD_ROW_BYTES)

#define BLINK_TICKS 10  // timer ticks of 50 ms

#define KEY_2ND 4096
#define KEY_QUIT (KEY_2ND + KEY_ESC)
#define KEY_SCREENSHOT (KEY_2ND + KEY_APPS)
// 2nd + I arrives as the character it types on the calculator, the
// imaginary i. It stands in for Shift-Tab, next to diamond + I for Tab.
#define KEY_BACK_TAB 151
// Hand adds 32768, but only to the arrows: with any other key it gives
// nothing. Shift + Hand is a key of its own, 380, which stands in for Meta.
#define KEY_HAND 32768
#define KEY_META 380
// The keys of the calculator's functions, which a terminal has no use for.
#define KEY_SIN 259
#define KEY_COS 260
#define KEY_TAN 261
#define KEY_LN 262

// ---------- screen ----------

static unsigned char *const lcd = LCD_MEM;

static short curCol, curRow;
static short wrapPending;  // cursor sits past the last column, as on a VT100
static short reverse;
static short cursorShown;

static unsigned char *cellAddr(short col, short row)
{
  return lcd + row * (CELL_H * LCD_ROW_BYTES) + (col >> 1);
}

#define CHECKERED 2  // for drawCell: reverse video on a checkered background

static void drawCell(short col, short row, unsigned char ch, short rev)
{
  const unsigned char *glyph = font[(ch < 32 || ch >= 32 + GLYPHS) ? GLYPH_UNKNOWN - 32 : ch - 32];
  unsigned char *p = cellAddr(col, row);
  short shift = (col & 1) ? 0 : 4;
  unsigned char mask = 0xF << shift;
  short y;

  for (y = 0; y < CELL_H; y++, p += LCD_ROW_BYTES) {
    unsigned char bits = glyph[y];
    if (rev) bits ^= 0xF;
    if (rev == CHECKERED) bits &= (y & 1) ? 0x5 : 0xA;
    *p = (*p & ~mask) | (bits << shift);
  }
}

static short cursorHidden;  // the program asked for no cursor

static void toggleCursor(void)
{
  unsigned char *p;
  unsigned char mask = (curCol & 1) ? 0x0F : 0xF0;
  short y;

  if (cursorHidden && !cursorShown) return;
  p = cellAddr(curCol, curRow);
  for (y = 0; y < CELL_H; y++, p += LCD_ROW_BYTES) *p ^= mask;
  cursorShown = !cursorShown;
}

static void copyCell(short dst, short src, short row)
{
  unsigned char *d = cellAddr(dst, row);
  unsigned char *s = cellAddr(src, row);
  short dShift = (dst & 1) ? 0 : 4;
  short sShift = (src & 1) ? 0 : 4;
  short y;

  for (y = 0; y < CELL_H; y++, d += LCD_ROW_BYTES, s += LCD_ROW_BYTES)
    *d = (*d & ~(0xF << dShift)) | (((*s >> sShift) & 0xF) << dShift);
}

static void hideCursor(void)
{
  if (cursorShown) toggleCursor();
}

static void clearCells(short col0, short col1, short row)
{
  short col;
  for (col = col0; col <= col1; col++) drawCell(col, row, ' ', 0);
}

// Shifts the rest of the cursor's line right, opening blank cells.
static void insertChars(short count)
{
  short col;
  if (count > COLS - curCol) count = COLS - curCol;
  for (col = COLS - 1; col - count >= curCol; col--) copyCell(col, col - count, curRow);
  clearCells(curCol, curCol + count - 1, curRow);
}

// Shifts the rest of the cursor's line left over the deleted cells.
static void deleteChars(short count)
{
  short col;
  if (count > COLS - curCol) count = COLS - curCol;
  for (col = curCol; col + count < COLS; col++) copyCell(col, col + count, curRow);
  clearCells(COLS - count, COLS - 1, curRow);
}

static void clearRows(short row0, short row1)
{
  if (row0 <= row1)
    memset(cellAddr(0, row0), 0, (row1 - row0 + 1) * (CELL_H * LCD_ROW_BYTES));
}

// Scrolling happens inside the scroll region, which full-screen programs
// narrow to keep a status line in place. It is the whole screen by default.
static short regionTop, regionBottom = ROWS - 1;

static void scrollUp(short top, short bottom)
{
  memmove(cellAddr(0, top), cellAddr(0, top + 1), (bottom - top) * (CELL_H * LCD_ROW_BYTES));
  clearRows(bottom, bottom);
}

static void scrollDown(short top, short bottom)
{
  memmove(cellAddr(0, top + 1), cellAddr(0, top), (bottom - top) * (CELL_H * LCD_ROW_BYTES));
  clearRows(top, top);
}

static void lineFeed(void)
{
  if (curRow == regionBottom) scrollUp(regionTop, regionBottom);
  else if (curRow < ROWS - 1) curRow++;
}

static void reverseLineFeed(void)
{
  if (curRow == regionTop) scrollDown(regionTop, regionBottom);
  else if (curRow > 0) curRow--;
}

static short clamp(short v, short lo, short hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static void moveTo(short col, short row)
{
  curCol = clamp(col, 0, COLS - 1);
  curRow = clamp(row, 0, ROWS - 1);
  wrapPending = 0;
}

// ---------- escape codes ----------

enum { ST_TEXT, ST_ESC, ST_CSI, ST_SKIP, ST_OSC, ST_OSC_ESC };

#define MAX_PARAMS 4

static short state;
static short params[MAX_PARAMS];
static short paramCount;
static short privateSeq;  // the sequence began with '?', '>' or '<'
static short escIntermediate;
static short insertMode;   // typed characters push the rest of the line right
static short lineDrawing;  // the line-drawing character set is selected
static unsigned long codePoint;  // the UTF-8 character being put together
static short utf8Left;           // bytes of it still to come
static short savedCol, savedRow, savedReverse;

// Programs such as less switch the arrow keys to "application" codes
// (ESC O A instead of ESC [ A) and then only understand those.
static short appCursorKeys;

static short param(short i, short dflt)
{
  return (i < paramCount && params[i]) ? params[i] : dflt;
}

static void eraseDisplay(short mode)
{
  if (mode == 0) {
    clearCells(curCol, COLS - 1, curRow);
    clearRows(curRow + 1, ROWS - 1);
  } else if (mode == 1) {
    clearRows(0, curRow - 1);
    clearCells(0, curCol, curRow);
  } else {
    clearRows(0, ROWS - 1);
  }
}

static void eraseLine(short mode)
{
  if (mode == 0) clearCells(curCol, COLS - 1, curRow);
  else if (mode == 1) clearCells(0, curCol, curRow);
  else clearCells(0, COLS - 1, curRow);
}

static void setAttributes(void)
{
  short i;
  if (paramCount == 0) reverse = 0;
  for (i = 0; i < paramCount; i++) {
    if (params[i] == 0 || params[i] == 27) reverse = 0;
    else if (params[i] == 7) reverse = 1;
  }
}

static void setRegion(short top, short bottom)
{
  if (top < 0 || bottom > ROWS - 1 || top >= bottom) {
    top = 0;
    bottom = ROWS - 1;
  }
  regionTop = top;
  regionBottom = bottom;
  moveTo(0, 0);
}

// Inserts (dir > 0) or deletes blank lines at the cursor's row.
static void shiftLines(short count, short dir)
{
  if (curRow < regionTop || curRow > regionBottom) return;
  while (count-- > 0) {
    if (dir > 0) scrollDown(curRow, regionBottom);
    else scrollUp(curRow, regionBottom);
  }
}

static void resetTerminal(void)
{
  reverse = 0;
  appCursorKeys = 0;
  cursorHidden = 0;
  insertMode = 0;
  lineDrawing = 0;
  setRegion(0, ROWS - 1);
  clearRows(0, ROWS - 1);
}

static void escFinal(unsigned char c)
{
  switch (c) {
    case 'D': lineFeed(); break;
    case 'E': curCol = 0; wrapPending = 0; lineFeed(); break;
    case 'M': reverseLineFeed(); break;
    case '7': savedCol = curCol; savedRow = curRow; savedReverse = reverse; break;
    case '8': moveTo(savedCol, savedRow); reverse = savedReverse; break;
    case 'c': resetTerminal(); break;
  }
}

// Private modes kept: 1, the arrow keys' codes, and 25, the cursor.
static void setPrivateMode(short on)
{
  short i;
  for (i = 0; i < paramCount; i++) {
    if (params[i] == 1) appCursorKeys = on;
    else if (params[i] == 25) cursorHidden = !on;
  }
}

static void setMode(short on)
{
  short i;
  for (i = 0; i < paramCount; i++)
    if (params[i] == 4) insertMode = on;
}

static void csiFinal(unsigned char c)
{
  switch (c) {
    case 'A': moveTo(curCol, curRow - param(0, 1)); break;
    case 'B': moveTo(curCol, curRow + param(0, 1)); break;
    case 'C': moveTo(curCol + param(0, 1), curRow); break;
    case 'D': moveTo(curCol - param(0, 1), curRow); break;
    case 'H':
    case 'f': moveTo(param(1, 1) - 1, param(0, 1) - 1); break;
    case 'J': eraseDisplay(param(0, 0)); break;
    case 'K': eraseLine(param(0, 0)); break;
    case '@': insertChars(param(0, 1)); break;
    case 'P': deleteChars(param(0, 1)); break;
    case 'X': clearCells(curCol, clamp(curCol + param(0, 1) - 1, curCol, COLS - 1), curRow); break;
    case 'h': setMode(1); break;
    case 'l': setMode(0); break;
    case 'L': shiftLines(param(0, 1), 1); break;
    case 'M': shiftLines(param(0, 1), -1); break;
    case 'm': setAttributes(); break;
    case 'r': setRegion(param(0, 1) - 1, param(1, ROWS) - 1); break;
  }
}

// The glyph for a character outside ASCII. 0 means it takes no room.
static unsigned char glyphFor(unsigned long cp)
{
  short lo = 0, hi = OTHERS - 1;

  if (cp < LATIN_FIRST) return 0;  // control codes
  if (cp < LATIN_FIRST + sizeof(latinGlyphs)) return latinGlyphs[cp - LATIN_FIRST];
  if (cp >= BOXES_FIRST && cp < BOXES_FIRST + sizeof(boxGlyphs)) return boxGlyphs[cp - BOXES_FIRST];
  if (cp >= 0x0300 && cp < 0x0370) return 0;  // accents added to the letter before
  if (cp >= 0xFE00 && cp < 0xFE10) return 0;  // variation selectors
  if (cp >= 0x2800 && cp < 0x2900) return cp == 0x2800 ? ' ' : '.';  // Braille, as in spinners

  while (lo <= hi) {
    short mid = (lo + hi) >> 1;
    if (otherCodes[mid] == cp) return otherGlyphs[mid];
    if (otherCodes[mid] < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return GLYPH_UNKNOWN;
}

// East Asian characters and emoji take two cells on a terminal.
static short isWide(unsigned long cp)
{
  return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) || cp >= 0x1F300;
}

static void putGlyph(unsigned char glyph)
{
  if (wrapPending) {
    curCol = 0;
    wrapPending = 0;
    lineFeed();
  }
  if (insertMode) insertChars(1);
  drawCell(curCol, curRow, glyph, reverse);
  if (curCol < COLS - 1) curCol++;
  else wrapPending = 1;
}

// One byte of a UTF-8 character. Draws it once the last byte is in.
static void putUtf8(unsigned char c)
{
  unsigned char glyph;

  if (c >= 0xC0) {  // first byte: says how many follow
    utf8Left = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    codePoint = c & (0x3F >> utf8Left);
    return;
  }
  if (!utf8Left) {  // a continuation byte with nothing to continue
    putGlyph(GLYPH_UNKNOWN);
    return;
  }
  codePoint = (codePoint << 6) | (c & 0x3F);
  if (--utf8Left) return;

  glyph = glyphFor(codePoint);
  if (isWide(codePoint)) putGlyph(glyph);
  if (glyph) putGlyph(glyph);
}

static void putText(unsigned char c)
{
  if (c >= 0x80) {
    putUtf8(c);
    return;
  }
  utf8Left = 0;

  switch (c) {
    case '\r': curCol = 0; wrapPending = 0; return;
    case '\n': lineFeed(); return;
    case '\b': moveTo(curCol - 1, curRow); return;
    case '\t': moveTo((curCol | 7) + 1, curRow); return;
    case 0x1B: state = ST_ESC; return;
  }
  if (c < 32) return;

  // The VT100 line-drawing set: lines as glyphs, the rest as the nearest
  // plain character.
  if (lineDrawing && c >= 0x60 && c <= 0x7E)
    c = "+#    o+##" "\x85\x86\x8A\x89\x8F" "--" "\x8C" "__" "\x8B\x87\x8D\x8E\x83" "<>*!f."[c - 0x60];

  putGlyph(c);
}

static void termPutc(unsigned char c)
{
  switch (state) {
    case ST_TEXT:
      putText(c);
      break;

    case ST_ESC:
      if (c == '[') {
        state = ST_CSI;
        paramCount = 0;
        privateSeq = 0;
        memset(params, 0, sizeof(params));
      } else if (c == ']') {
        state = ST_OSC;   // window title and the like: skipped whole
      } else if (c >= 0x20 && c <= 0x2F) {
        state = ST_SKIP;  // character set selection and such: one more byte
        escIntermediate = c;
      } else {
        state = ST_TEXT;
        escFinal(c);
      }
      break;

    case ST_OSC:
      if (c == 0x07) state = ST_TEXT;
      else if (c == 0x1B) state = ST_OSC_ESC;
      break;

    case ST_OSC_ESC:
      state = (c == '\\') ? ST_TEXT : ST_OSC;
      break;

    case ST_CSI:
      if (c >= '0' && c <= '9') {
        if (paramCount == 0) paramCount = 1;
        if (paramCount <= MAX_PARAMS)
          params[paramCount - 1] = params[paramCount - 1] * 10 + (c - '0');
      } else if (c == ';') {
        if (paramCount == 0) paramCount = 1;
        if (paramCount <= MAX_PARAMS) paramCount++;
      } else if (c >= 0x40) {
        if (paramCount > MAX_PARAMS) paramCount = MAX_PARAMS;
        if (!privateSeq) csiFinal(c);
        else if (c == 'h' || c == 'l') setPrivateMode(c == 'h');
        state = ST_TEXT;
      } else if (c >= '<') {
        privateSeq = 1;  // a mode this terminal does not have
      }
      // anything else (spaces and such) belongs to the sequence: ignore it
      break;

    case ST_SKIP:
      if (escIntermediate == '(') lineDrawing = (c == '0');
      state = ST_TEXT;
      break;
  }
}

static void termWrite(const char *s, short len)
{
  hideCursor();
  while (len--) termPutc(*s++);
}

static void termPuts(const char *s)
{
  termWrite(s, strlen(s));
}

// ---------- status line guard ----------

// The OS draws the 2nd, shift and diamond indicators in its status line,
// over the terminal's bottom row, from auto-int 1, and later erases them to
// white, which leaves a hole in what the terminal drew there. Auto-int 1 also
// fills the keyboard queue, so it cannot be turned off. Instead the OS's
// handler runs inside one of ours, which saves the box where the indicators
// go and puts it back afterwards: they never reach the screen. Measured on a
// Voyage 200, the OS drew in pixel rows 123 to 127, x 48 to 63; the box takes
// rows 121 to 127, x 48 to 79, one long a row.
#define GUARD_FIRST 121
#define GUARD_ROWS (LCD_HEIGHT - GUARD_FIRST)
#define GUARD_BYTE 6  // even, as a long needs

#define GUARD_LONG(r) (*(volatile unsigned long *)(LCD_MEM + (GUARD_FIRST + (r)) * LCD_ROW_BYTES + GUARD_BYTE))

static INT_HANDLER oldInt1;
static unsigned long guardSaved[GUARD_ROWS];

DEFINE_INT_HANDLER(guardInt1)
{
  short r;
  for (r = 0; r < GUARD_ROWS; r++) guardSaved[r] = GUARD_LONG(r);
  ExecuteHandler(oldInt1);
  for (r = 0; r < GUARD_ROWS; r++) GUARD_LONG(r) = guardSaved[r];
}

static void guardOn(void)
{
  oldInt1 = GetIntVec(AUTO_INT_1);
  SetIntVec(AUTO_INT_1, guardInt1);
}

static void guardOff(void)
{
  SetIntVec(AUTO_INT_1, oldInt1);
}

// ---------- host ----------

// Everything to and from the ESP32 goes in the frames of the link layer
// (link/src/tlink.h): each is checked, acknowledged, and sent again if it was
// lost, so nothing arrives garbled, not even after the cable was replugged.
// The link is interrupt driven: AMS moves bytes between the port and its
// transmit and receive queues in the background, and these calls only touch
// the queues. Nothing here uses TI's packet protocol.
//
// The session's bytes go in DATA frames. A session starts by telling the
// ESP32 where they should go, with a CMD frame and a letter, and ends with
// CMD 'x'.

enum {
  MODE_SERIAL, MODE_SSH, MODE_WIFI, MODE_SSH_HOST, MODE_SSH_KEY, MODE_SSH_COPY,  // through the link
  MODE_WIFI_JOIN,
  MODE_LOCAL, MODE_KEY_CODES
};

// The letter that starts each of the sessions that go through the link.
static const char MODE_LETTERS[] = "shnkgij";

static short online;  // the session's bytes go out of the link

// The ESP32 keeps up to MAX_HOSTS SSH hosts, one for each of F1 to F8, and
// up to MAX_NETWORKS Wi-Fi networks. The one a session is about goes in its
// CMD frame, after the letter.
#define MAX_HOSTS 8
#define MAX_NETWORKS 4
static short hostSlot;  // from 1; in the Wi-Fi menu, the network

#define RTO_MS 500         // a frame not acknowledged by then is sent again
#define KEEPALIVE_MS 2000  // a PING after this long without sending: the ESP32 hears the calculator

static tl_link lk;
static unsigned char rxBuf[64];
static short rxLen, rxPos;

// The ESP32's id, from the answer to the link's HELLO. A new one means the
// ESP32 has restarted.
static unsigned char espId[2];
static short espIdKnown;

// What waits for room in the link's window, in order, as records of a frame
// type, a length and the frame's bytes. Keys typed one after another join
// the same DATA record.
#define OUT_QUEUE 128
static unsigned char outQueue[OUT_QUEUE];
static short outQueueLen;
static short lastData;  // where the last record starts while it is DATA and still queued, or -1

static unsigned long nowMs(void)
{
  return FiftyMsecTick * 50UL;
}

// The calculator's free memory goes to the ESP32 in a MEM frame every
// MEMORY_MS, for the Cardputer's memory page. Counting the archive takes
// longer, so it is counted again only every ARCHIVE_MS. HeapMax is not
// used: it compacts the heap each time.
#define MEMORY_MS 5000UL
#define ARCHIVE_MS 60000UL
static unsigned long memoryAt, archiveAt;
static unsigned long archiveFree, archiveGc, archiveUsed;

static void putLong(unsigned char *p, unsigned long v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

// Only when nothing else waits for the window, so that it never holds up a
// key.
static void reportMemory(unsigned long now)
{
  unsigned char m[TL_MEM_SIZE];
  unsigned short handles;

  if (!lk.started || outQueueLen || !tl_window_free(&lk) || now - memoryAt < MEMORY_MS) return;
  memoryAt = now;
  if (now - archiveAt >= ARCHIVE_MS) {
    archiveAt = now;
    EM_survey(&archiveUsed, &archiveGc, &archiveFree, NULL, NULL, NULL);
  }
  handles = FreeHandles();
  putLong(m + TL_MEM_HEAP, HeapAvail());
  m[TL_MEM_HANDLES] = handles;
  m[TL_MEM_HANDLES + 1] = handles >> 8;
  putLong(m + TL_MEM_ARCHIVE, archiveFree);
  putLong(m + TL_MEM_GC, archiveGc);
  putLong(m + TL_MEM_ARCHIVED, archiveUsed);
  tl_send(&lk, TL_MEM, m, sizeof(m));
}

// Returns 0 when there is no room: nothing has been taking frames for a
// while (nobody is listening), and they are dropped rather than left to
// pile up.
static short queueFrame(unsigned char type, const void *data, short len)
{
  if (type == TL_DATA && lastData >= 0 && outQueue[lastData + 1] + len <= TL_MAX_PAYLOAD &&
      outQueueLen + len <= OUT_QUEUE) {
    memcpy(outQueue + outQueueLen, data, len);
    outQueue[lastData + 1] += len;
    outQueueLen += len;
    return 1;
  }
  if (outQueueLen + 2 + len > OUT_QUEUE) return 0;
  lastData = type == TL_DATA ? outQueueLen : -1;
  outQueue[outQueueLen] = type;
  outQueue[outQueueLen + 1] = len;
  memcpy(outQueue + outQueueLen + 2, data, len);
  outQueueLen += 2 + len;
  return 1;
}

static void clearQueue(void)
{
  outQueueLen = 0;
  lastData = -1;
}

// Moves the link along: queued frames into the window, and what is due onto
// the line, as far as the OS's transmit queue has room.
static void linkTransmit(unsigned long now)
{
  unsigned char buf[32];
  short room, n, b, size;

  while (outQueueLen && tl_window_free(&lk)) {
    size = 2 + outQueue[1];
    tl_send(&lk, outQueue[0], outQueue + 2, outQueue[1]);
    outQueueLen -= size;
    memmove(outQueue, outQueue + size, outQueueLen);
    lastData = lastData >= size ? lastData - size : -1;
  }
  reportMemory(now);
  tl_poll(&lk, now);
  room = OSLinkTxQueueInquire();
  if (room > (short)sizeof(buf)) room = sizeof(buf);
  for (n = 0; n < room && (b = tl_tx_next(&lk, now)) >= 0; n++) buf[n] = b;
  if (n) OSWriteLinkBlock((char *)buf, n);
}

// Moves the link along and returns the next frame from the ESP32, with
// lk.in holding it, or 0 when none has come. While lk.rxOpen is 0 the
// ESP32's sequenced frames are left: they come again later.
static short linkNext(void)
{
  unsigned long now = nowMs();
  short type;

  for (;;) {
    if (rxPos >= rxLen) {
      rxPos = 0;
      rxLen = OSReadLinkBlock((char *)rxBuf, sizeof(rxBuf));
      if (rxLen <= 0) {
        rxLen = 0;
        break;
      }
    }
    type = tl_rx_byte(&lk, rxBuf[rxPos++], now);
    if (type == TL_HELLO_ACK && lk.in.len >= 2) {
      memcpy(espId, lk.in.data, sizeof(espId));
      espIdKnown = 1;
      memoryAt = archiveAt = now - ARCHIVE_MS;  // a new link, maybe a new ESP32: all of it at once
    }
    if (type) {
      linkTransmit(now);  // the acknowledgement goes at once
      return type;
    }
  }
  linkTransmit(now);
  return 0;
}

// While nothing can be drawn: the link carries on, and what the ESP32
// sends meanwhile comes again later.
static void linkWait(void)
{
  short wasOpen = lk.rxOpen;
  lk.rxOpen = 0;
  while (linkNext()) { }
  lk.rxOpen = wasOpen;
}

static void linkCommand(char letter, short slot)
{
  unsigned char command[2];
  command[0] = letter;
  command[1] = slot;
  queueFrame(TL_CMD, command, slot ? 2 : 1);
}

// Starts the link afresh, and the OS's side of it too: what is queued is
// dropped. Done when a menu has had no answer for a while: after the cable
// came out and went back in, the OS's side of the link can be stuck.
// The reset may have dropped the 'x' that ended the last session, so it is
// sent again; with no session going it does nothing.
static void resetLink(void)
{
  OSLinkReset();
  OSLinkOpen();
  rxLen = rxPos = 0;
  clearQueue();
  tl_hello(&lk);
  linkCommand('x', 0);
}

// Sends bytes towards the host. In a local session it echoes them the way a
// shell would, so the terminal can be tried on its own.
static void hostSend(const char *s, short len)
{
  if (online) {
    // A key sequence goes whole or not at all.
    queueFrame(TL_DATA, s, len);
    return;
  }
  while (len--) {
    char c = *s++;
    if (c == '\r') {
      termPuts("\r\n");
    } else if (c == 0x7F) {
      termPuts("\b \b");
    } else {
      termWrite(&c, 1);
    }
  }
}

// ---------- keyboard ----------

static void *keyQueue;

// Set by the Meta key: the next key's bytes go out after an ESC, which is
// how a terminal sends Alt + that key.
static short metaPending;

// Sends a key's bytes, the ESC of a pending Meta in front, in one go.
static void keySend(const char *s, short len)
{
  char buf[8];
  if (metaPending && len < (short)sizeof(buf)) {
    buf[0] = 0x1B;
    memcpy(buf + 1, s, len);
    s = buf;
    len++;
  }
  metaPending = 0;
  hostSend(s, len);
}

#define SHOT_PIECE 60                        // bytes of the screen in a SHOT frame
#define SHOT_PIECES (LCD_SIZE / SHOT_PIECE)  // 64
#define SCREEN_PATIENCE 100                  // ticks of 50 ms without room in the window

// Sends the screen as it is to the ESP32, which passes it to the computer:
// SHOT frames of a piece number and SHOT_PIECE of the LCD's bytes. Nothing
// is drawn meanwhile, so what the ESP32 sends is left for later. If the
// window stays full nobody is listening, and the rest is not sent.
static void sendScreen(void)
{
  unsigned char piece[1 + SHOT_PIECE];
  unsigned long waited = FiftyMsecTick;
  short wasOpen = lk.rxOpen;
  short i = 0;

  lk.rxOpen = 0;
  while (i < SHOT_PIECES) {
    while (linkNext()) { }
    if (!outQueueLen && tl_window_free(&lk)) {
      piece[0] = i;
      memcpy(piece + 1, lcd + i * SHOT_PIECE, SHOT_PIECE);
      tl_send(&lk, TL_SHOT, piece, sizeof(piece));
      i++;
      waited = FiftyMsecTick;
    } else if (FiftyMsecTick - waited > SCREEN_PATIENCE) {
      break;
    } else {
      idle();
    }
  }
  lk.rxOpen = wasOpen;
}

// The screenshot key is dealt with here, so that it works wherever a key is
// waited for. The link carries on meanwhile.
static unsigned short waitKey(void)
{
  unsigned short key;
  for (;;) {
    while (OSdequeue(&key, keyQueue)) {
      linkWait();
      idle();
    }
    if (key != KEY_SCREENSHOT) return key;
    sendScreen();
  }
}

#define CHAR_ROWS 2
#define CHAR_PER_ROW 11
#define CHAR_CELLS 5  // per symbol: three for its key, the symbol, a blank

// Stands in for the calculator's CHAR menu. The bottom two rows list the
// symbols, each after the letter that picks it, drawn as a key in reverse
// video; any other key closes the menu.
static void charMenu(void)
{
  static const char SYMBOLS[] = "~`|\\$%&#@_{}[]<>'\";:!?";
  unsigned char saved[CHAR_ROWS * CELL_H * LCD_ROW_BYTES];
  unsigned char *top = cellAddr(0, ROWS - CHAR_ROWS);
  unsigned char *p;
  unsigned short key;
  short i, k, row, col, y;

  hideCursor();
  memcpy(saved, top, sizeof(saved));
  for (row = ROWS - CHAR_ROWS; row < ROWS; row++) clearCells(0, COLS - 1, row);
  for (i = 0; SYMBOLS[i]; i++) {
    row = ROWS - CHAR_ROWS + i / CHAR_PER_ROW;
    col = (i % CHAR_PER_ROW) * CHAR_CELLS;
    for (k = 0; k < 3; k++) {
      drawCell(col + k, row, k == 1 ? 'a' + i : ' ', 1);
      // The key is five pixels high: the cell's last pixel row goes back to
      // normal, which keeps the two rows of keys apart. A descender shows there.
      cellAddr(col + k, row)[(CELL_H - 1) * LCD_ROW_BYTES] ^= ((col + k) & 1) ? 0x0F : 0xF0;
    }
    // The key ends one pixel early, so that the symbol does not touch it.
    p = cellAddr(col + 2, row);
    for (y = 0; y < CELL_H; y++, p += LCD_ROW_BYTES) *p &= ((col + 2) & 1) ? ~0x01 : ~0x10;
    drawCell(col + 3, row, SYMBOLS[i], 0);
  }
  key = waitKey();
  memcpy(top, saved, sizeof(saved));

  if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
  if (key >= 'a' && key < 'a' + sizeof(SYMBOLS) - 1) keySend(&SYMBOLS[key - 'a'], 1);
}

static void sendArrow(char letter)
{
  char code[3];
  code[0] = 0x1B;
  code[1] = (appCursorKeys && online) ? 'O' : '[';
  code[2] = letter;
  keySend(code, 3);
}

// An arrow with a modifier, as xterm sends it: ESC [ 1 ; mod letter, in
// application cursor mode too. mod is 2 for Shift, 5 for Ctrl.
static void sendModArrow(char mod, char letter)
{
  char code[6];
  code[0] = 0x1B;
  code[1] = '[';
  code[2] = '1';
  code[3] = ';';
  code[4] = mod;
  code[5] = letter;
  keySend(code, 6);
}

// The arrow keys' letters in their codes, or 0 for any other key.
static char arrowLetter(unsigned short key)
{
  switch (key) {
    case KEY_UP: return 'A';
    case KEY_DOWN: return 'B';
    case KEY_RIGHT: return 'C';
    case KEY_LEFT: return 'D';
  }
  return 0;
}

static short isQuitKey(unsigned short key)
{
  return key == KEY_QUIT || key == KEY_DIAMOND + KEY_ESC;
}

static void handleKey(unsigned short key)
{
  // F1 to F8 as a VT220 sends them (it has no F5; that code is xterm's).
  static const char *const FUNCTION_KEYS[] = {
    "\x1BOP", "\x1BOQ", "\x1BOR", "\x1BOS", "\x1B[15~", "\x1B[17~", "\x1B[18~", "\x1B[19~"
  };
  // F9 to F12, on diamond + F1 to F4.
  static const char *const HIGH_FUNCTION_KEYS[] = {
    "\x1B[20~", "\x1B[21~", "\x1B[23~", "\x1B[24~"
  };
  char c;

  if (key == KEY_META || key == KEY_MODE) {
    metaPending = !metaPending;  // pressed again, it is called off
    return;
  }
  if (key == KEY_2ND + '+') {
    charMenu();
  } else if (key == KEY_DIAMOND + KEY_BACKSPACE) {
    keySend("\x1B[3~", 4);  // DEL on the keyboard: deletes forwards
  } else if (key == KEY_INS) {
    keySend("\x1B[2~", 4);  // INS on the keyboard
  } else if (key == KEY_BACK_TAB) {
    keySend("\x1B[Z", 3);
  } else if (key == KEY_DIAMOND + ' ') {
    keySend("", 1);  // Ctrl-Space: NUL
  } else if (key >= KEY_DIAMOND + '@' && key <= KEY_DIAMOND + '_') {
    c = key - KEY_DIAMOND - '@';
    keySend(&c, 1);
  } else if (key >= KEY_DIAMOND + 'a' && key <= KEY_DIAMOND + 'z') {
    c = key - KEY_DIAMOND - 'a' + 1;
    keySend(&c, 1);
  } else if (key == KEY_APPS) {
    keySend("\t", 1);
  } else if (key == KEY_DIAMOND + KEY_APPS) {
    keySend("\x1B[Z", 3);  // Shift-Tab, next to Tab
  } else if (key == KEY_CLEAR) {
    keySend("\x15", 1);  // Ctrl-U: erases the line in the shell
  } else if (key == KEY_DIAMOND + KEY_CLEAR) {
    // Resets this terminal and clears its screen, sending nothing: for a
    // screen left in a mess that the program in it will not redraw.
    hideCursor();
    termPuts("\x1B" "c");
  } else if (key == KEY_DIAMOND + KEY_MODE) {
    keySend("\x1D", 1);  // Ctrl-], which diamond + 2nd + divide cannot give
  } else if (key == KEY_LN) {
    keySend("\x12", 1);  // Ctrl-R: searches the shell's history
  } else if (key == KEY_2ND + KEY_LN) {
    keySend("\x1F", 1);  // Ctrl-_: undo
  } else if (key == KEY_DIAMOND + KEY_LN) {
    keySend("\x1C", 1);  // Ctrl-backslash: quits what Ctrl-C does not stop
  } else if (key == KEY_2ND + '^') {
    keySend("~", 1);  // 2nd + ^ is pi on the calculator
  } else if (key == KEY_SIN) {
    keySend("\x02" "p", 2);  // tmux: the previous window
  } else if (key == KEY_COS) {
    keySend("\x02" "n", 2);  // tmux: the next window
  } else if (key == KEY_TAN) {
    keySend("\x02", 1);  // tmux's prefix, Ctrl-B
  } else if (key == KEY_2ND + KEY_SIN) {
    keySend("\x02" "w", 2);  // tmux: the list of windows
  } else if (key == KEY_2ND + KEY_COS) {
    keySend("\x02" "l", 2);  // tmux: the window before this one
  } else if (key == KEY_2ND + KEY_TAN) {
    keySend("\x02" "c", 2);  // tmux: a new window
  } else if (key == KEY_DIAMOND + KEY_SIN) {
    keySend("\x02" "\x1B[5~", 5);  // tmux: copy mode, a page back
  } else if (key == KEY_DIAMOND + KEY_COS) {
    keySend("\x02" "]", 2);  // tmux: pastes what copy mode copied
  } else if (key == KEY_DIAMOND + KEY_TAN) {
    keySend("\x02" "[", 2);  // tmux: copy mode
  } else if (key == KEY_ENTER) {
    keySend("\r", 1);
  } else if (key == KEY_BACKSPACE) {
    keySend("\x7F", 1);
  } else if (key == KEY_ESC) {
    keySend("\x1B", 1);
  } else if (key == KEY_DIAMOND + KEY_UP) {
    keySend("\x1B[5~", 4);
  } else if (key == KEY_DIAMOND + KEY_DOWN) {
    keySend("\x1B[6~", 4);
  } else if (key == KEY_DIAMOND + KEY_LEFT) {
    keySend("\x1B" "b", 2);  // Alt-B: a word back in the shell
  } else if (key == KEY_DIAMOND + KEY_RIGHT) {
    keySend("\x1B" "f", 2);  // Alt-F: a word forward
  } else if (key > KEY_HAND && arrowLetter(key - KEY_HAND)) {
    sendModArrow('5', arrowLetter(key - KEY_HAND));
  } else if (key > KEY_SHIFT && key < KEY_HAND && arrowLetter(key - KEY_SHIFT)) {
    sendModArrow('2', arrowLetter(key - KEY_SHIFT));
  } else if (key == KEY_2ND + KEY_UP || key == KEY_2ND + KEY_LEFT) {
    sendArrow('H');  // Home and End use the arrow keys' code family
  } else if (key == KEY_2ND + KEY_DOWN || key == KEY_2ND + KEY_RIGHT) {
    sendArrow('F');
  } else if (key == KEY_UP) {
    sendArrow('A');
  } else if (key == KEY_DOWN) {
    sendArrow('B');
  } else if (key == KEY_RIGHT) {
    sendArrow('C');
  } else if (key == KEY_LEFT) {
    sendArrow('D');
  } else if (key >= KEY_F1 && key <= KEY_F8) {
    const char *code = FUNCTION_KEYS[key - KEY_F1];
    keySend(code, strlen(code));
  } else if (key >= KEY_DIAMOND + KEY_F1 && key <= KEY_DIAMOND + KEY_F4) {
    const char *code = HIGH_FUNCTION_KEYS[key - KEY_DIAMOND - KEY_F1];
    keySend(code, strlen(code));
  } else if (key >= 32 && key < 127) {
    c = key;
    keySend(&c, 1);
  }
  metaPending = 0;  // a key that sends nothing uses it up too
}

// ---------- sessions ----------

// The menu's bottom row, in reverse video.
static void drawStatus(const char *text, short len)
{
  short col;
  for (col = 0; col < COLS; col++) drawCell(col, ROWS - 1, col < len ? text[col] : ' ', 1);
}

// The link layer sends a PING whenever the calculator has sent nothing for
// KEEPALIVE_MS, and the ESP32 acknowledges it, so each side keeps hearing
// the other. When the link slips out of step after the cable was replugged,
// the calculator cannot send, so the ESP32 stops and the quiet that follows
// puts the link back in step; what it sent meanwhile is sent again. No good
// frame for LOST_MS shows the banner over the bottom row, until the next one
// arrives. A busy ESP32 can also be silent for a while: an SSH connection to
// a host that does not answer takes 15 s to fail.
#define LOST_MS 12000UL

static const char LOST_BANNER[] = " ESP32: no answer, check the link cable    2nd+ESC: menu";
static const char RESTARTED[] = "\r\n[ESP32 restarted, session lost: 2nd+ESC for the menu]\r\n";
static unsigned char bannerSaved[CELL_H * LCD_ROW_BYTES];
static short bannerShown;

static void showBanner(void)
{
  hideCursor();
  memcpy(bannerSaved, cellAddr(0, ROWS - 1), sizeof(bannerSaved));
  drawStatus(LOST_BANNER, sizeof(LOST_BANNER) - 1);
  bannerShown = 1;
}

static void hideBanner(void)
{
  memcpy(cellAddr(0, ROWS - 1), bannerSaved, sizeof(bannerSaved));
  bannerShown = 0;
}

// Runs until the quit key. Serial, SSH and the settings sessions go through
// the link (the ESP32 runs the settings dialogs); the local ones draw what
// is typed, or each key's code.
static void session(short mode)
{
  char buf[16];
  unsigned char sessionId[sizeof(espId)];
  unsigned short key;
  unsigned long started;
  short haveId, restartTold = 0;
  short type, got;

  online = mode < MODE_LOCAL;
  bannerShown = 0;
  metaPending = 0;
  termPuts("\x1B" "c");
  if (mode == MODE_LOCAL) termPuts("[local echo, 2nd+ESC for the menu]\r\n");
  if (mode == MODE_KEY_CODES) termPuts("[key codes, 2nd+ESC for the menu]\r\n");

  // The ESP32 this session runs on. If the link has not been answered yet,
  // its first answer tells.
  haveId = espIdKnown;
  memcpy(sessionId, espId, sizeof(espId));
  if (online) {
    if (mode == MODE_SSH || mode == MODE_SSH_HOST || mode == MODE_SSH_COPY || mode == MODE_WIFI_JOIN) {
      linkCommand(MODE_LETTERS[mode], hostSlot);
    } else {
      linkCommand(MODE_LETTERS[mode], 0);
    }
    // Ctrl-L: a shell that is already waiting redraws its prompt.
    if (mode == MODE_SERIAL) hostSend("\x0C", 1);
  }
  started = nowMs();
  OSRegisterTimer(USER_TIMER, BLINK_TICKS);

  for (;;) {
    // One frame a pass, so that the keys are read during a flood too.
    type = online ? linkNext() : 0;
    got = type == TL_DATA;
    if (got) {
      termWrite((const char *)lk.in.data, lk.in.len);
      toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (type == TL_HELLO_ACK) {
      if (!haveId) {
        memcpy(sessionId, espId, sizeof(espId));
        haveId = 1;
      } else if (memcmp(sessionId, espId, sizeof(espId)) && !restartTold) {
        termPuts(RESTARTED);
        restartTold = 1;
      }
    }
    if (online) {
      unsigned long now = nowMs();
      if (tl_heard_within(&lk, now, LOST_MS)) {
        if (bannerShown) hideBanner();
      } else if (!bannerShown && now - started >= LOST_MS) {
        showBanner();
      }
    }

    // Keys are read on every pass, so that the quit key and Ctrl-C get
    // through a flood of output too.
    if (!OSdequeue(&key, keyQueue)) {
      if (isQuitKey(key)) break;
      if (key == KEY_SCREENSHOT && mode != MODE_KEY_CODES) {
        sendScreen();
      } else if (mode == MODE_KEY_CODES) {
        sprintf(buf, "%u ", key);
        termPuts(buf);
      } else {
        handleKey(key);
      }
      // The banner's row is kept as it was under it: no cursor while it shows.
      if (!cursorShown && !bannerShown) toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (!type && OSTimerExpired(USER_TIMER)) {
      if (!bannerShown) toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (!type) {
      idle();
    }
  }

  OSFreeTimer(USER_TIMER);
  if (online) linkCommand('x', 0);
  online = 0;
  state = ST_TEXT;  // a sequence cut short by the quit key
}

static void testPattern(void)
{
  char line[COLS + 1];
  unsigned long t0, ticks;
  short i;

  termPuts("\x1B" "c");
  t0 = FiftyMsecTick;
  for (i = 0; i < COLS * ROWS; i++) {
    char c = ' ' + i % 95;
    termWrite(&c, 1);
  }
  ticks = FiftyMsecTick - t0;

  termPuts("\x1B[2J\x1B[H");
  termPuts("normal \x1B[7mreverse\x1B[0m normal\r\n");
  termPuts("tabs:\tA\tB\tC\r\n");
  termPuts("erase to end of line -->XXXXXXXX\x1B[8D\x1B[K\r\n");
  termPuts("\x1B[10;20Hrow 10, column 20");
  termPuts("\x1B[21;1Hany key for the menu\x1B[1;50Htop right");
  termPuts("\x1B[16;1H┌──┬──┐  ├── tree   ▁▂▃▄▅▆▇█ ░▒▓  café ação “q” → ✓\r\n"
           "├──┼──┤  │   └── leaf\r\n"
           "└──┴──┘  └── \x1B(0lqqk x mqqj\x1B(B (VT100 lines)");
  termPuts("\x1B[12;1H");
  for (i = 0; i < COLS + 10; i++) {
    char c = '0' + i % 10;
    termWrite(&c, 1);
  }
  sprintf(line, "\r\nfull screen (%d chars): %lu ms\r\n", COLS * ROWS, ticks * 50);
  termPuts(line);
  waitKey();
}

#define STATUS_TICKS 60  // 3 s between questions to the ESP32

// The pages of help.txt, one screen each.
static void showHelp(void)
{
  char line[COLS + 1];
  short page;

  for (page = 0; page < HELP_PAGES; page++) {
    termPuts("\x1B" "c");
    termPuts(helpPages[page]);
    sprintf(line, " Help %d of %d    %s", page + 1, HELP_PAGES,
            page + 1 < HELP_PAGES ? "any key: next page    ESC: menu" : "any key: menu");
    drawStatus(line, strlen(line));
    if (waitKey() == KEY_ESC) break;
  }
}

#define FOOTER_MARGIN 2

// Text that ends at the right margin of a row (rows count from 1).
static void putRight(short row, const char *text)
{
  char seq[16];
  short len = 0;
  const char *p;

  // Characters, not bytes: the bytes that continue a UTF-8 character are not counted.
  for (p = text; *p; p++) if ((*p & 0xC0) != 0x80) len++;
  sprintf(seq, "\x1B[%d;%dH", row, COLS + 1 - FOOTER_MARGIN - len);
  termPuts(seq);
  termPuts(text);
}

#define MENU_FIRST_ROW 2  // of F1's key, counted from 0; the next is two rows down
#define MENU_KEY_COL 2

// Draws the keys of the entries in mask (bit 0 for F1) checkered when they
// cannot be chosen, and in plain reverse video otherwise.
static void shadeKeys(short entries, short mask, short greyed)
{
  char label[5];
  short i, k;

  for (i = 0; i < entries; i++) {
    if (!(mask & (1 << i))) continue;
    sprintf(label, " F%d ", i + 1);
    for (k = 0; k < 4; k++)
      drawCell(MENU_KEY_COL + k, MENU_FIRST_ROW + 2 * i, label[k], greyed ? CHECKERED : 1);
  }
}

// Waits for one of a menu's entries, F1 and on, while the bottom row shows
// the ESP32's Wi-Fi status. Its answer is a STATUS frame, '+' or '-' for
// Wi-Fi up or down and the text. The entries in needEsp cannot be
// chosen until the ESP32 answers, or once it stops answering; those in
// needWifi, also while Wi-Fi is down. Returns the entry, from 1, or 0 for ESC.
static short menuChoice(short entries, short needEsp, short needWifi)
{
  static const char NO_WIFI[] = "Wi-Fi is not connected: the main menu's F3 joins a network";
  unsigned short key;
  short choice = -1;
  short unanswered = 0;
  short answered = 0;    // the ESP32 is answering
  short wifiUp = 0, lineWifiUp;
  short type, len, bit;

  shadeKeys(entries, needEsp | needWifi, 1);
  drawStatus("Wi-Fi: asking the ESP32...", 26);
  linkCommand('w', 0);
  OSRegisterTimer(USER_TIMER, STATUS_TICKS);

  while (choice < 0) {
    // Outside a session only the Wi-Fi status is expected; the rest is dropped.
    type = linkNext();
    if (type == TL_STATUS && lk.in.len >= 1) {
      lineWifiUp = lk.in.data[0] == '+';
      len = lk.in.len - 1;
      drawStatus((const char *)lk.in.data + 1, len < COLS ? len : COLS);
      if (!answered || wifiUp != lineWifiUp) {
        answered = 1;
        wifiUp = lineWifiUp;
        shadeKeys(entries, needEsp, 0);
        shadeKeys(entries, needWifi, !wifiUp);
      }
      unanswered = 0;
    }

    if (!OSdequeue(&key, keyQueue)) {
      if (key == KEY_SCREENSHOT) sendScreen();
      if (key == KEY_ESC || isQuitKey(key)) choice = 0;
      if (key >= KEY_F1 && key < (unsigned short)(KEY_F1 + entries)) {
        bit = 1 << (key - KEY_F1);
        // Without an answer the status row already says why nothing happens.
        if (answered || !((needEsp | needWifi) & bit)) {
          if (!wifiUp && (needWifi & bit)) drawStatus(NO_WIFI, sizeof(NO_WIFI) - 1);
          else choice = key - KEY_F1 + 1;
        }
      }
    } else if (OSTimerExpired(USER_TIMER)) {
      if (++unanswered > 2) {
        drawStatus("ESP32: no answer", 16);
        if (answered) {
          answered = 0;
          shadeKeys(entries, needEsp | needWifi, 1);
        }
        resetLink();
      }
      linkCommand('w', 0);
      OSTimerRestart(USER_TIMER);
    } else if (!type) {
      idle();
    }
  }
  OSFreeTimer(USER_TIMER);
  return choice;
}

// A menu under the main one. Returns the entry chosen, from 1, or 0 for ESC.
static short submenu(const char *text, short entries, short needEsp, short needWifi)
{
  termPuts("\x1B" "c");
  termPuts(text);
  termPuts("  \x1B[7m ESC \x1B[0m Back");
  return menuChoice(entries, needEsp, needWifi);
}

#define HOST_FIRST_ROW 3  // of F1's entry, counted from 1; the next is two rows down
#define HOST_COL 9        // where an entry's text starts, counted from 1

// A menu of the ESP32's SSH hosts or Wi-Fi networks, count of them, which it
// is asked for with a CMD frame and letter. Its answer is a LIST frame for
// each entry: the letter, the entry from 0 and its text, empty if not set.
// Returns the entry chosen, from 1, or 0 for ESC. An entry that is not set
// can be chosen only with emptyToo.
static short listMenu(const char *title, const char *hint, short emptyToo, short count, char letter)
{
  char buf[32];
  unsigned short key;
  char isSet[MAX_HOSTS];  // the longest list
  short arrived = 0;      // a bit for each entry that has come
  short complete = 0;
  short unanswered = 0;
  short type, line, len, i;

  termPuts("\x1B" "c");
  termPuts(title);
  termPuts("\r\n\r\n");
  for (i = 0; i < count; i++) {
    sprintf(buf, "  \x1B[7m F%d \x1B[0m\r\n\r\n", i + 1);
    termPuts(buf);
  }
  termPuts("  \x1B[7m ESC \x1B[0m Back");
  shadeKeys(count, (1 << count) - 1, 1);  // until the list arrives

  drawStatus("Asking the ESP32...", 19);
  linkCommand(letter, 0);
  OSRegisterTimer(USER_TIMER, STATUS_TICKS);

  for (;;) {
    type = linkNext();
    if (type == TL_LIST && lk.in.len >= 2 && lk.in.data[0] == letter && lk.in.data[1] < count) {
      line = lk.in.data[1];
      sprintf(buf, "\x1B[%d;%dH\x1B[K", HOST_FIRST_ROW + 2 * line, HOST_COL);
      termPuts(buf);
      len = lk.in.len - 2;
      if (len > COLS - HOST_COL) len = COLS - HOST_COL;
      if (len) termWrite((const char *)lk.in.data + 2, len);
      else termPuts("-");
      isSet[line] = len > 0;
      arrived |= 1 << line;
      if (!complete && arrived == (1 << count) - 1) {
        complete = 1;
        drawStatus(hint, strlen(hint));
        shadeKeys(count, (1 << count) - 1, 0);
      }
    }

    if (!OSdequeue(&key, keyQueue)) {
      if (key == KEY_SCREENSHOT) sendScreen();
      if (key == KEY_ESC || isQuitKey(key)) { key = 0; break; }
      if (complete && key >= KEY_F1 && key < (unsigned short)(KEY_F1 + count) &&
          (emptyToo || isSet[key - KEY_F1])) {
        key = key - KEY_F1 + 1;
        break;
      }
    } else if (!complete && OSTimerExpired(USER_TIMER)) {
      drawStatus("ESP32: no answer", 16);
      if (++unanswered > 2) resetLink();
      linkCommand(letter, 0);
      OSTimerRestart(USER_TIMER);
    } else if (!type) {
      idle();
    }
  }
  OSFreeTimer(USER_TIMER);
  return key;
}

static short hostMenu(const char *title, const char *hint, short emptyToo)
{
  return listMenu(title, hint, emptyToo, MAX_HOSTS, 'l');
}

static void settingsMenu(void)
{
  short choice;

  for (;;) {
    choice = submenu(
      "Settings\r\n\r\n"
      "  \x1B[7m F1 \x1B[0m  Wi-Fi         up to 4 networks\r\n\r\n"
      "  \x1B[7m F2 \x1B[0m  SSH hosts     up to 8, each a host and a user\r\n\r\n"
      "  \x1B[7m F3 \x1B[0m  SSH key       make this ESP32's key, or remove it\r\n\r\n"
      "  \x1B[7m F4 \x1B[0m  Copy the key  to a host, for SSH without password\r\n\r\n",
      4, 0xF, 1 << 3);  // all through the ESP32; copying the key needs Wi-Fi
    if (!choice) return;
    if (choice == 2) {
      while ((hostSlot = hostMenu("SSH hosts", " F1 to F8: set, change or remove that host", 1)))
        session(MODE_SSH_HOST);
    } else if (choice == 4) {
      while ((hostSlot = hostMenu("Copy the key to a host", " F1 to F8: copy the ESP32's key to that host", 0)))
        session(MODE_SSH_COPY);
    } else {
      session(MODE_WIFI + choice - 1);
    }
  }
}

static void debugMenu(void)
{
  for (;;) {
    switch (submenu(
      "Debug\r\n\r\n"
      "  \x1B[7m F1 \x1B[0m  Local echo    nothing leaves the calculator\r\n\r\n"
      "  \x1B[7m F2 \x1B[0m  Test pattern\r\n\r\n"
      "  \x1B[7m F3 \x1B[0m  Key codes\r\n\r\n",
      3, 0, 0)) {
      case 1: session(MODE_LOCAL); break;
      case 2: testPattern(); break;
      case 3: session(MODE_KEY_CODES); break;
      default: return;
    }
  }
}

// Returns 0 when the choice is to leave the program.
static short menu(void)
{
  termPuts("\x1B" "c");
  termPuts("Voyage 200 terminal, built " __DATE__ " " __TIME__ ", " VERSION "\r\n\r\n");
  termPuts("  \x1B[7m F1 \x1B[0m  Serial        a shell on the computer, over USB\r\n\r\n");
  termPuts("  \x1B[7m F2 \x1B[0m  SSH           over the ESP32's Wi-Fi\r\n\r\n");
  termPuts("  \x1B[7m F3 \x1B[0m  Wi-Fi         pick the network to use\r\n\r\n");
  termPuts("  \x1B[7m F4 \x1B[0m  Settings      Wi-Fi, SSH hosts, SSH key\r\n\r\n");
  termPuts("  \x1B[7m F5 \x1B[0m  Debug         local echo, test pattern, key codes\r\n\r\n");
  termPuts("  \x1B[7m F6 \x1B[0m  Help          the keys in a session\r\n\r\n");
  termPuts("  \x1B[7m ESC \x1B[0m Exit\r\n\r\n");
  termPuts("  Exit Shell: 2nd+ESC");
  putRight(17, "2026 M\xC3\xA1rio Carneiro");  // a with an acute accent
  putRight(18, "@ticklemynausea");
  putRight(19, "mariocarneiro.info");

  // Serial, SSH, Wi-Fi and Settings go through the ESP32; SSH needs Wi-Fi too.
  switch (menuChoice(6, 0xF, 1 << 1)) {
    case 1: session(MODE_SERIAL); break;
    case 2:
      hostSlot = hostMenu("SSH", " F1 to F8: connect to that host", 0);
      if (hostSlot) session(MODE_SSH);
      break;
    case 3:
      while ((hostSlot = listMenu("Wi-Fi", " F1 to F4: use that network", 0, MAX_NETWORKS, 'm')))
        session(MODE_WIFI_JOIN);
      break;
    case 4: settingsMenu(); break;
    case 5: debugMenu(); break;
    case 6: showHelp(); break;
    default: return 0;
  }
  return 1;
}

void _main(void)
{
  // Keys are taken straight from the keyboard queue. kbhit and ngetchx would
  // also poll the link and treat incoming bytes as TI packets.
  keyQueue = kbd_queue();
  memset(lcd, 0, LCD_SIZE);

  OSLinkReset();
  OSLinkOpen();
  // Set here, not where they are declared: a program's variables can keep
  // their values from the run before.
  rxLen = rxPos = 0;
  espIdKnown = 0;
  clearQueue();
  tl_init(&lk, TL_INITIATOR, RTO_MS, KEEPALIVE_MS);
  // The ESP32 takes a HELLO with the number of the last one for that one
  // sent again, so a new run must not start from the same number.
  lk.hello = (unsigned short)FiftyMsecTick;
  tl_hello(&lk);
  OSFreeTimer(USER_TIMER);
  guardOn();

  while (menu()) { }

  guardOff();
  OSLinkReset();  // leave nothing behind for AMS to read as a packet
}
