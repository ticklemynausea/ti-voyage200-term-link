// A dumb terminal for the TI Voyage 200, built with TIGCC / GCC4TI.
//
// 60 x 21 characters in a 4x6 cell, with a subset of the VT220 escape codes.
// Keys go out of the link port as raw bytes, and whatever comes in is drawn.
// The other end is the ESP32 running esp32/esp32_tilink_term, which the menu
// tells where to carry the bytes: to its USB serial port or to an SSH host.
//
// Keys in a session:
//   diamond + letter   Ctrl + letter
//   diamond + up/down  Page Up / Page Down
//   2nd + up/down      Home / End (2nd + left/right too)
//   2nd + plus (CHAR)  symbols the keyboard lacks: pick one by its letter
//   2nd + ESC (QUIT)   back to the menu; diamond + ESC does the same
//   2nd + APPS         sends the screen to the computer, as a screenshot

#define USE_V200
#define MIN_AMS 207
#define SAVE_SCREEN

#include <tigcclib.h>
#include "font.h"
#include "help.h"

#define COLS 60
#define ROWS 21
#define CELL_H 6
#define LCD_ROW_BYTES 30
#define TEXT_BYTES (ROWS * CELL_H * LCD_ROW_BYTES)

#define BLINK_TICKS 10  // timer ticks of 50 ms

#define KEY_2ND 4096
#define KEY_QUIT (KEY_2ND + KEY_ESC)
#define KEY_SCREENSHOT (KEY_2ND + KEY_APPS)

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
  const unsigned char *glyph = font[(ch < 32 || ch >= 32 + GLYPHS) ? GLYPH_UNKNOWN - 32 : ch - 32];
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

enum { ST_TEXT, ST_ESC, ST_CSI, ST_SKIP, ST_OSC, ST_OSC_ESC, ST_MARK, ST_STATUS };

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
    case 0x1E: state = ST_MARK; return;  // a late answer meant for the menu
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

    case ST_MARK:
      state = (c == 'w') ? ST_STATUS : ST_TEXT;
      break;

    case ST_STATUS:
      if (c == '\n') state = ST_TEXT;
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

// ---------- host ----------

// The link is interrupt driven: AMS moves bytes between the port and its
// transmit and receive queues in the background, and these calls only touch
// the queues. Nothing here uses TI's packet protocol.
//
// A session starts by telling the ESP32 where the bytes should go, with LINK_MARK
// and a letter, and ends with LINK_MARK 'x'. A real LINK_MARK byte is sent twice.

#define LINK_MARK 0x1E

enum {
  MODE_SERIAL, MODE_SSH, MODE_WIFI, MODE_SSH_HOST, MODE_SSH_KEY, MODE_SSH_COPY,  // through the link
  MODE_LOCAL, MODE_KEY_CODES
};

// The letter that starts each of the sessions that go through the link.
static const char MODE_LETTERS[] = "shnkgi";

static short online;  // the session's bytes go out of the link

static void linkCommand(char letter)
{
  char command[2];
  command[0] = LINK_MARK;
  command[1] = letter;
  OSWriteLinkBlock(command, 2);
}

// Sends bytes towards the host. In a local session it echoes them the way a
// shell would, so the terminal can be tried on its own.
static void hostSend(const char *s, short len)
{
  while (len--) {
    char c = *s++;
    if (online) {
      // Fails when the transmit queue is full (nobody is listening): the
      // keys are dropped rather than left to pile up.
      if (c == LINK_MARK) OSWriteLinkBlock(&c, 1);
      OSWriteLinkBlock(&c, 1);
    } else if (c == '\r') {
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

#define SCREEN_PIECE 64
#define SCREEN_PATIENCE 1000  // wake-ups with no room in the queue: a few seconds

// Sends the screen as it is to the ESP32, which passes it to the computer:
// LINK_MARK 'p' and the LCD's bytes, unescaped since their number is fixed.
// The OS's transmit queue is small, so the bytes go in pieces as it empties.
// If it stays full nobody is listening, and what is queued is thrown away.
static void sendScreen(void)
{
  const char *p = (const char *)lcd;
  unsigned short left = LCD_SIZE;
  unsigned short waited = 0;
  short started = 0;
  short n;

  while (left) {
    n = OSLinkTxQueueInquire();
    if (!started && n >= 2) {
      linkCommand('p');
      started = 1;
      waited = 0;
    } else if (started && n > 0) {
      if (n > SCREEN_PIECE) n = SCREEN_PIECE;
      if ((unsigned short)n > left) n = left;
      OSWriteLinkBlock(p, n);
      p += n;
      left -= n;
      waited = 0;
    } else if (++waited > SCREEN_PATIENCE) {
      OSLinkOpen();
      return;
    } else {
      idle();
    }
  }
}

// The screenshot key is dealt with here, so that it works wherever a key is
// waited for.
static unsigned short waitKey(void)
{
  unsigned short key;
  for (;;) {
    while (OSdequeue(&key, keyQueue)) idle();
    if (key != KEY_SCREENSHOT) return key;
    sendScreen();
  }
}

// Stands in for the calculator's CHAR menu. The bottom row lists the symbols,
// each after the letter that picks it; any other key closes the menu.
static void charMenu(void)
{
  static const char SYMBOLS[] = "~`|\\$%&#@_{}[]<>'\";:!?";
  unsigned char saved[CELL_H * LCD_ROW_BYTES];
  unsigned char *row = cellAddr(0, ROWS - 1);
  unsigned short key;
  short i;

  hideCursor();
  memcpy(saved, row, sizeof(saved));
  for (i = 0; SYMBOLS[i]; i++) {
    drawCell(i * 2, ROWS - 1, 'a' + i, 1);
    drawCell(i * 2 + 1, ROWS - 1, SYMBOLS[i], 0);
  }
  clearCells(i * 2, COLS - 1, ROWS - 1);
  key = waitKey();
  memcpy(row, saved, sizeof(saved));

  if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
  if (key >= 'a' && key < 'a' + sizeof(SYMBOLS) - 1) hostSend(&SYMBOLS[key - 'a'], 1);
}

static void sendArrow(char letter)
{
  char code[3];
  code[0] = 0x1B;
  code[1] = (appCursorKeys && online) ? 'O' : '[';
  code[2] = letter;
  hostSend(code, 3);
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
  char c;

  if (key == KEY_2ND + '+') {
    charMenu();
  } else if (key >= KEY_DIAMOND + '@' && key <= KEY_DIAMOND + '_') {
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
  } else if (key == KEY_DIAMOND + KEY_UP) {
    hostSend("\x1B[5~", 4);
  } else if (key == KEY_DIAMOND + KEY_DOWN) {
    hostSend("\x1B[6~", 4);
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
    hostSend(code, strlen(code));
  } else if (key >= 32 && key < 127) {
    c = key;
    hostSend(&c, 1);
  }
}

// ---------- sessions ----------

// Runs until the quit key. Serial, SSH and the settings sessions go through
// the link (the ESP32 runs the settings dialogs); the local ones draw what
// is typed, or each key's code.
static void session(short mode)
{
  char buf[64];
  unsigned short key;
  short n;

  online = mode < MODE_LOCAL;
  termPuts("\x1B" "c");
  if (mode == MODE_LOCAL) termPuts("[local echo, 2nd+ESC for the menu]\r\n");
  if (mode == MODE_KEY_CODES) termPuts("[key codes, 2nd+ESC for the menu]\r\n");

  if (online) {
    OSLinkOpen();  // drops whatever is still queued
    linkCommand(MODE_LETTERS[mode]);
    // Ctrl-L: a shell that is already waiting redraws its prompt.
    if (mode == MODE_SERIAL) hostSend("\x0C", 1);
  }
  OSRegisterTimer(USER_TIMER, BLINK_TICKS);

  for (;;) {
    n = online ? OSReadLinkBlock(buf, sizeof(buf)) : 0;
    if (n > 0) {
      termWrite(buf, n);
      toggleCursor();
      OSTimerRestart(USER_TIMER);
    } else if (!OSdequeue(&key, keyQueue)) {
      if (isQuitKey(key)) break;
      if (key == KEY_SCREENSHOT && mode != MODE_KEY_CODES) {
        sendScreen();
      } else if (mode == MODE_KEY_CODES) {
        sprintf(buf, "%u ", key);
        termPuts(buf);
      } else {
        handleKey(key);
      }
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
  if (online) linkCommand('x');
  online = 0;
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

// The menu's bottom row, in reverse video.
static void drawStatus(const char *text, short len)
{
  short col;
  for (col = 0; col < COLS; col++) drawCell(col, ROWS - 1, col < len ? text[col] : ' ', 1);
}

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
  sprintf(seq, "\x1B[%d;%dH", row, COLS + 1 - FOOTER_MARGIN - (short)strlen(text));
  termPuts(seq);
  termPuts(text);
}

// A menu under the main one. Returns the entry chosen, from 1, or 0 for ESC.
static short submenu(const char *text, short entries)
{
  unsigned short key;

  termPuts("\x1B" "c");
  termPuts(text);
  termPuts("  \x1B[7m ESC \x1B[0m Back");
  for (;;) {
    key = waitKey();
    if (key == KEY_ESC || isQuitKey(key)) return 0;
    if (key >= KEY_F1 && key < (unsigned short)(KEY_F1 + entries)) return key - KEY_F1 + 1;
  }
}

static void settingsMenu(void)
{
  short choice;

  for (;;) {
    choice = submenu(
      "Settings\r\n\r\n"
      "  \x1B[7m F1 \x1B[0m  Wi-Fi         network and password\r\n\r\n"
      "  \x1B[7m F2 \x1B[0m  SSH host      host and user\r\n\r\n"
      "  \x1B[7m F3 \x1B[0m  SSH key       make this ESP32's key, or remove it\r\n\r\n"
      "  \x1B[7m F4 \x1B[0m  Copy the key  to the host, for SSH without password\r\n\r\n",
      4);
    if (!choice) return;
    session(MODE_WIFI + choice - 1);
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
      3)) {
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
  char buf[64];
  char status[COLS];
  unsigned short key;
  short choice = 0;
  short step = 0;        // where in "LINK_MARK w text newline" the input is
  short statusLen = 0;
  short unanswered = 0;
  short n, i;

  termPuts("\x1B" "c");
  termPuts("Voyage 200 terminal, built " __DATE__ " " __TIME__ "\r\n\r\n");
  termPuts("  \x1B[7m F1 \x1B[0m  Serial        a shell on the computer, over USB\r\n\r\n");
  termPuts("  \x1B[7m F2 \x1B[0m  SSH           over the ESP32's Wi-Fi\r\n\r\n");
  termPuts("  \x1B[7m F3 \x1B[0m  Settings      Wi-Fi, SSH host and user, SSH key\r\n\r\n");
  termPuts("  \x1B[7m F4 \x1B[0m  Debug         local echo, test pattern, key codes\r\n\r\n");
  termPuts("  \x1B[7m F5 \x1B[0m  Help          the keys in a session\r\n\r\n");
  termPuts("  \x1B[7m ESC \x1B[0m Exit\r\n\r\n");
  termPuts("  Exit Shell: 2nd+ESC");
  putRight(19, "2026 Mario Carneiro");
  putRight(20, "http://mariocarneiro.info");

  drawStatus("Wi-Fi: asking the ESP32...", 26);
  linkCommand('w');
  OSRegisterTimer(USER_TIMER, STATUS_TICKS);

  while (!choice) {
    // Outside a session only the Wi-Fi status is expected; the rest is dropped.
    n = OSReadLinkBlock(buf, sizeof(buf));
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (step == 0) {
        if (c == LINK_MARK) step = 1;
      } else if (step == 1) {
        step = (c == 'w') ? 2 : 0;
        statusLen = 0;
      } else if (c == '\n') {
        drawStatus(status, statusLen);
        unanswered = 0;
        step = 0;
      } else if (statusLen < COLS) {
        status[statusLen++] = c;
      }
    }

    if (!OSdequeue(&key, keyQueue)) {
      if (key == KEY_SCREENSHOT) sendScreen();
      if (isQuitKey(key)) key = KEY_ESC;
      if ((key >= KEY_F1 && key <= KEY_F5) || key == KEY_ESC) choice = key;
    } else if (OSTimerExpired(USER_TIMER)) {
      if (++unanswered > 2) drawStatus("ESP32: no answer", 16);
      linkCommand('w');
      OSTimerRestart(USER_TIMER);
    } else if (n == 0) {
      idle();
    }
  }
  OSFreeTimer(USER_TIMER);

  switch (choice) {
    case KEY_F1: session(MODE_SERIAL); break;
    case KEY_F2: session(MODE_SSH); break;
    case KEY_F3: settingsMenu(); break;
    case KEY_F4: debugMenu(); break;
    case KEY_F5: showHelp(); break;
    case KEY_ESC: return 0;
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
  OSFreeTimer(USER_TIMER);

  while (menu()) { }

  OSLinkReset();  // leave nothing behind for AMS to read as a packet
}
