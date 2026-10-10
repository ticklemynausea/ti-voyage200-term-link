// A dumb terminal for the TI Voyage 200, built with TIGCC / GCC4TI.
//
// 60 x 21 characters in a 4x6 cell, with a subset of the VT100 escape codes.
// Keys go out of the link port as raw bytes, and whatever comes in is drawn.
// The other end is the ESP32 running esp32/esp32_tilink_term.
//
// Keys:
//   diamond + letter   Ctrl + letter
//   F1                 test pattern, with the time a full screen takes
//   F2                 switch between online and local echo
//   2nd + ESC (QUIT)   quit; diamond + ESC does the same

#define USE_V200
#define MIN_AMS 207
#define SAVE_SCREEN

#include <tigcclib.h>
#include "font.h"

#define COLS 60
#define ROWS 21
#define CELL_H 6
#define LCD_ROW_BYTES 30
#define TEXT_BYTES (ROWS * CELL_H * LCD_ROW_BYTES)

#define BLINK_TICKS 10  // timer ticks of 50 ms

#define KEY_2ND 4096
#define KEY_QUIT (KEY_2ND + KEY_ESC)

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

static void drawCell(short col, short row, unsigned char ch, short rev)
{
  const unsigned char *glyph = font[(ch < 32 || ch > 127) ? 95 : ch - 32];
  unsigned char *p = cellAddr(col, row);
  short shift = (col & 1) ? 0 : 4;
  unsigned char mask = 0xF << shift;
  short y;

  for (y = 0; y < CELL_H; y++, p += LCD_ROW_BYTES) {
    unsigned char bits = glyph[y];
    if (rev) bits ^= 0xF;
    *p = (*p & ~mask) | (bits << shift);
  }
}

static void toggleCursor(void)
{
  unsigned char *p = cellAddr(curCol, curRow);
  unsigned char mask = (curCol & 1) ? 0x0F : 0xF0;
  short y;

  for (y = 0; y < CELL_H; y++, p += LCD_ROW_BYTES) *p ^= mask;
  cursorShown = !cursorShown;
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

static void clearRows(short row0, short row1)
{
  if (row0 <= row1)
    memset(cellAddr(0, row0), 0, (row1 - row0 + 1) * (CELL_H * LCD_ROW_BYTES));
}

static void scrollUp(void)
{
  memmove(lcd, lcd + CELL_H * LCD_ROW_BYTES, TEXT_BYTES - CELL_H * LCD_ROW_BYTES);
  clearRows(ROWS - 1, ROWS - 1);
}

static void lineFeed(void)
{
  if (curRow < ROWS - 1) curRow++;
  else scrollUp();
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

enum { ST_TEXT, ST_ESC, ST_CSI, ST_SKIP };

#define MAX_PARAMS 4

static short state;
static short params[MAX_PARAMS];
static short paramCount;

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
    case 'm': setAttributes(); break;
  }
}

static void putText(unsigned char c)
{
  switch (c) {
    case '\r': curCol = 0; wrapPending = 0; return;
    case '\n': lineFeed(); return;
    case '\b': moveTo(curCol - 1, curRow); return;
    case '\t': moveTo((curCol | 7) + 1, curRow); return;
    case 0x1B: state = ST_ESC; return;
  }
  if (c < 32) return;

  if (wrapPending) {
    curCol = 0;
    wrapPending = 0;
    lineFeed();
  }
  drawCell(curCol, curRow, c, reverse);
  if (curCol < COLS - 1) curCol++;
  else wrapPending = 1;
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
        memset(params, 0, sizeof(params));
      } else if (c == '(' || c == ')') {
        state = ST_SKIP;  // character set selection: one more byte follows
      } else {
        state = ST_TEXT;
      }
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
        csiFinal(c);
        state = ST_TEXT;
      }
      // anything else ('?', '>', spaces) belongs to the sequence: ignore it
      break;

    case ST_SKIP:
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

// ---------- host ----------

// The link is interrupt driven: AMS moves bytes between the port and its
// transmit and receive queues in the background, and these calls only touch
// the queues. Nothing here uses TI's packet protocol.

static short online = 1;

// Sends bytes towards the host. In local mode it echoes them the way a shell
// would, so the terminal can be tried on its own.
static void hostSend(const char *s, short len)
{
  if (online) {
    // Fails when the transmit queue is full (nobody is listening): the keys
    // are dropped rather than left to pile up.
    OSWriteLinkBlock(s, len);
    return;
  }
  while (len--) {
    char c = *s++;
    if (c == '\r') termPuts("\r\n");
    else if (c == 0x7F) termPuts("\b \b");
    else termWrite(&c, 1);
  }
}

// ---------- keyboard ----------

static void testPattern(void)
{
  char line[COLS + 1];
  unsigned long t0, ticks;
  short i;

  termPuts("\x1B[2J\x1B[H");
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
  termPuts("\x1B[21;1Hbottom row\x1B[1;50Htop right");
  termPuts("\x1B[12;1H");
  for (i = 0; i < COLS + 10; i++) {
    char c = '0' + i % 10;
    termWrite(&c, 1);
  }
  sprintf(line, "\r\nfull screen (%d chars): %lu ms\r\n", COLS * ROWS, ticks * 50);
  termPuts(line);
}

// Returns 0 when the key asks to quit.
static short handleKey(short key)
{
  char c;

  if (key == KEY_QUIT || key == KEY_DIAMOND + KEY_ESC) return 0;

  if (key >= KEY_DIAMOND + '@' && key <= KEY_DIAMOND + '_') {
    c = key - KEY_DIAMOND - '@';
    hostSend(&c, 1);
  } else if (key >= KEY_DIAMOND + 'a' && key <= KEY_DIAMOND + 'z') {
    c = key - KEY_DIAMOND - 'a' + 1;
    hostSend(&c, 1);
  } else if (key == KEY_ENTER) {
    hostSend("\r", 1);
  } else if (key == KEY_BACKSPACE) {
    hostSend("\x7F", 1);
  } else if (key == KEY_ESC) {
    hostSend("\x1B", 1);
  } else if (key == KEY_UP) {
    hostSend("\x1B[A", 3);
  } else if (key == KEY_DOWN) {
    hostSend("\x1B[B", 3);
  } else if (key == KEY_RIGHT) {
    hostSend("\x1B[C", 3);
  } else if (key == KEY_LEFT) {
    hostSend("\x1B[D", 3);
  } else if (key == KEY_F1) {
    testPattern();
  } else if (key == KEY_F2) {
    online = !online;
    OSLinkOpen();  // drops whatever is still queued
    termPuts(online ? "\r\n[online]\r\n" : "\r\n[local echo]\r\n");
  } else if (key >= 32 && key < 127) {
    c = key;
    hostSend(&c, 1);
  }
  return 1;
}

void _main(void)
{
  // Keys are taken straight from the keyboard queue. kbhit and ngetchx would
  // also poll the link and treat incoming bytes as TI packets.
  void *keys = kbd_queue();
  char buf[64];
  unsigned short key;
  short n;

  memset(lcd, 0, LCD_SIZE);
  termPuts("Voyage 200 terminal\r\n");
  termPuts("F1 test pattern, F2 local echo, 2nd+ESC quit\r\n\r\n");

  OSLinkReset();
  OSLinkOpen();
  OSFreeTimer(USER_TIMER);
  OSRegisterTimer(USER_TIMER, BLINK_TICKS);

  for (;;) {
    n = OSReadLinkBlock(buf, sizeof(buf));
    if (n > 0 && online) {
      termWrite(buf, n);
      toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (!OSdequeue(&key, keys)) {
      if (!handleKey(key)) break;
      if (!cursorShown) toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (OSTimerExpired(USER_TIMER)) {
      toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else {
      idle();
    }
  }

  OSFreeTimer(USER_TIMER);
  OSLinkReset();  // leave nothing behind for AMS to read as a packet
}
