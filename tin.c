#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "abuf.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* defines */

#define TIN_VERSION "0.2.1"
#define TIN_TAB_STOP 4
#define TIN_STATUS_MSG_SECS 2
#define TIN_QUIT_TIMES 2
#define ESC_SEQ "\x1b["
#define CTRL_KEY(key) (0x1f & (key))
#define REPORT_ERR(msg) (set_status_msg(msg ": %s", strerror(errno)))

// UTF encoding format
// 0xxxxxxx   ASCII (normal char range)
// 1xxxxxxx   UTF (i.e. (unsigned) c > 127)
// 10xxxxxx   internal byte in a multi-byte sequence
// 11xxxxxx   head of multi-byte sequence, indicates sequence length
// 110xxxxx   head of two-byte sequence
// 1110xxxx   head of three-byte sequence
// 11110xxx   head of four-byte sequence
#define UTF_HEAD_BYTE(c) (!((c & 0xC0) ^ 0xC0))
#define UTF_BODY_BYTE(c) (!((c & 0xC0) ^ 0x80))
#define VISIBLE_BYTE(c) (UTF_HEAD_BYTE(c) || !UTF_BODY_BYTE(c))

enum named_key {
  TAB_KEY = '\t',
  RETURN = '\r',
  ESC = '\x1b',
  BACKSPACE = 127,

  // for non-printable/escaped keys use values that can't fit in 8 bits
  ARROW_UP = 1000,
  ARROW_DOWN,
  ARROW_RIGHT,
  ARROW_LEFT,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

typedef long long llong_t;
typedef unsigned long long ullong_t;

typedef struct textrow {
  llong_t len;   // number of raw chars
  char *chars;   // raw chars
  llong_t rlen;  // number of rendered chars (e.g. tabs show as spaces)
  char *render;  // rendered chars
  llong_t ndisp; // number of VISIBLE_BYTEs left of given index [0, rlen]
} textrow;

struct config {
  struct termios orig_tty;
  llong_t cx, cy;           // cursor position
  llong_t rx;               // horizontal cursor render position
  llong_t winrows, wincols; // window size
  llong_t rowoff, coloff;   // scroll offsets
  llong_t lnmargin;         // line number margin
  llong_t nrows;            // number of text rows
  textrow *rows;            // text lines
  char *filename;           // filename
  char statusmsg[128];      // status message
  time_t statusmsg_time;    // time status message was last updated
  ullong_t dirty;           // number of changes since last save
};

struct config E; // global editor config

/* prototypes */

int read_key();
void clear_tty();

/* helpers */

// keel over dead due to failure in function named s
void die(const char *s) {
  clear_tty();
  perror(s);
  exit(1);
}

// write formatted string to bottom status bar
void set_status_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

// return the number of characters needed to represent n
int nplaces(llong_t n) {
  if (n < 0)
    n = (n == INT_MIN) ? INT_MAX : -n;
  if (n < 10)
    return 1;
  if (n < 100)
    return 2;
  if (n < 1000)
    return 3;
  if (n < 10000)
    return 4;
  if (n < 100000)
    return 5;
  if (n < 1000000)
    return 6;
  if (n < 10000000)
    return 7;
  if (n < 100000000)
    return 8;
  if (n < 1000000000)
    return 9;
  if (n < 10000000000)
    return 10;
  if (n < 100000000000)
    return 11;
  if (n < 1000000000000)
    return 12;
  if (n < 10000000000000)
    return 13;
  if (n < 100000000000000)
    return 14;
  if (n < 1000000000000000)
    return 15;
  if (n < 10000000000000000)
    return 16;
  if (n < 100000000000000000)
    return 17;
  if (n < 1000000000000000000)
    return 18;
  return 19;
}

/* terminal config */

// set rows, cols to current cursor position
int cursor_pos(llong_t *rows, llong_t *cols) {
  if (write(STDOUT_FILENO, ESC_SEQ "6n", 4) != 4)
    return -1;

  char buf[64] = "";
  for (unsigned int i = 0; i < sizeof(buf) - 1; i++) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
  }

  // response is of the form: <ESC_SEQ>row;colR
  // see https://vt100.net/docs/vt100-ug/chapter3.html#CPR
  if (buf[0] != ESC_SEQ[0] || buf[1] != ESC_SEQ[1])
    return -1;
  if (sscanf(&buf[2], "%lld;%lld", rows, cols) != 2)
    return -1;

  return 0;
}

// try to get window size using ioctl first
// otherwise, move cursor to bottom right and get position
int measure_window(llong_t *rows, llong_t *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // try to check window size by moving cursor to bottom right
    if (write(STDOUT_FILENO, ESC_SEQ "999C" ESC_SEQ "999B", 12) != 12)
      return -1;
    return cursor_pos(rows, cols);
  } else {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
  }
  return 0;
}

void set_editor_size() {
  if (measure_window(&E.winrows, &E.wincols) == -1)
    die("measure_window");
  E.winrows -= 2; // for status bar and status message
}

void init_config() {
  E.cx = E.cy = E.rx = 0;
  E.rowoff = E.coloff = 0;
  E.nrows = 0;
  E.rows = NULL;
  E.lnmargin = 2; // single digit line number to start, plus one space
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.dirty = 0;
  set_editor_size();
}

/* tty control */

void clear_tty() {
  write(STDOUT_FILENO, ESC_SEQ "2J", 4); // clear screen
  write(STDOUT_FILENO, ESC_SEQ "H", 3);  // move cursor to top left
}

void disable_raw_tty() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_tty) == -1)
    die("tcsetattr");
}

void enable_raw_tty() {
  if (tcgetattr(STDIN_FILENO, &E.orig_tty) == -1)
    die("tcgetattr");

  atexit(disable_raw_tty);
  struct termios tty = E.orig_tty;

  // input flags
  tty.c_iflag &= ~IXON;   // fix ^S, ^Q
  tty.c_iflag &= ~ICRNL;  // fix ^M (CR)
  tty.c_iflag &= ~BRKINT; // breaks still send SIGINT
  tty.c_iflag &= ~INPCK;  // disable parity checking
  tty.c_iflag &= ~ISTRIP; // disable stripping of 8th bit

  // output flags
  tty.c_oflag &= ~OPOST; // disable output processing

  // control flags and chars
  tty.c_cflag |= CS8;  // 8-bit chars
  tty.c_cc[VMIN] = 0;  // min bytes to read before returning
  tty.c_cc[VTIME] = 1; // time to wait for min bytes (* 0.1s)

  // local flags
  tty.c_lflag &= ~ECHO;   // disable character echo
  tty.c_lflag &= ~ICANON; // disable canonical mode (line buffering)
  tty.c_lflag &= ~IEXTEN; // fix ^V, ^O
  tty.c_lflag &= ~ISIG;   // fix ^C, ^Z

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tty) == -1)
    die("tcsetattr");
}

/* status bar */

void draw_top_status(abuf *ab) {
  ab_strcat(ab, ESC_SEQ "7m", 4); // reverse colors

  // calculate components
  char *fname = E.filename ? E.filename : "[New]";
  char *dirty = E.dirty ? "*" : " ";
  llong_t row = E.rows ? E.cy + 1 : 0;
  llong_t col = E.rx + 1;
  llong_t nrows = E.nrows;
  llong_t ncols = (E.rows && E.cy < E.nrows ? E.rows[E.cy].ndisp : 0);

  // build status bar
  llong_t barlen = E.wincols;
  char lmsg[barlen + 1], rmsg[barlen + 1];
  llong_t rlen = barlen;
  rlen = snprintf(rmsg, rlen, "L%lld/%lld : C%lld/%lld (%lldx%lld)", row, nrows,
                  col, ncols, E.winrows, E.wincols);
  llong_t llen = barlen - rlen;
  llen = snprintf(lmsg, llen, "[%s] %.20s", dirty, fname);

  // write status bar
  ab_strcat(ab, lmsg, llen);
  llong_t nspaces = barlen - rlen - llen;
  while (nspaces-- > 0)
    ab_charcat(ab, ' ');
  ab_strcat(ab, rmsg, rlen);
  ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
}

void draw_bot_status(abuf *ab) {
  ab_strcat(ab, ESC_SEQ "K", 3);  // clear message bar
  ab_strcat(ab, ESC_SEQ "7m", 4); // reverse colors

  // clear message after timeout
  if (time(NULL) - E.statusmsg_time >= TIN_STATUS_MSG_SECS)
    E.statusmsg[0] = '\0';

  llong_t barlen = E.wincols;
  llong_t msglen = strlen(E.statusmsg);
  if (msglen > barlen)
    msglen = barlen;
  if (msglen)
    ab_strcat(ab, E.statusmsg, msglen);
  ullong_t nspaces = barlen - msglen;
  while (nspaces-- > 0)
    ab_strcat(ab, " ", 1);

  ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
}

/* main interface */

llong_t cx_to_rx(textrow *row, llong_t cx) {
  llong_t rx = 0;
  for (llong_t j = 0; j < cx; j++) {
    char c = row->chars[j];
    if (c == TAB_KEY)
      rx += TIN_TAB_STOP - (rx % TIN_TAB_STOP);
    else if (UTF_BODY_BYTE(c))
      continue;
    else
      rx++;
  }
  return rx;
}

llong_t rx_to_cx(textrow *row, int rx) {
  llong_t cx, cur_rx = 0;
  for (cx = 0; cx < row->len; cx++) {
    char c = row->chars[cx];
    if (c == TAB_KEY)
      cur_rx += TIN_TAB_STOP - (cur_rx % TIN_TAB_STOP);
    else if (UTF_BODY_BYTE(c))
      continue;
    else
      cur_rx++;

    if (cur_rx > rx)
      return cx;
  }
  return cx;
}

void draw_welcome(abuf *ab, int line) {
  char msg[80];
  int len;
  switch (line) {
  case 0:
    len = snprintf(msg, sizeof(msg), "TIN - TIN Isn't Nano");
    break;
  case 1:
    len = snprintf(msg, sizeof(msg), "version %s", TIN_VERSION);
    break;
  case 2:
    len = snprintf(msg, sizeof(msg), "^X exit    ^S save    ^F find");
    break;
  default:
    len = 0;
  }

  len = (len > E.wincols) ? E.wincols : len;
  int pad = (E.wincols - len) / 2;
  if (pad) {
    ab_strcat(ab, "~", 1);
    pad--;
  }
  while (pad-- > 0)
    ab_strcat(ab, " ", 1);
  ab_strcat(ab, msg, len);
}

void scroll() {
  // calculate index into render buffer
  // differs from cx if line contains tabs
  E.rx = 0;
  if (E.cy < E.nrows) {
    E.rx = cx_to_rx(&E.rows[E.cy], E.cx);
  }

  // adjust offsets if cursor is off screen
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.winrows) {
    E.rowoff = E.cy - E.winrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  // extra math here to handle line numbers
  if (E.rx + E.lnmargin >= E.coloff + E.wincols) {
    E.coloff = E.rx + E.lnmargin - E.wincols + 1;
  }
}

void draw_rows(abuf *ab) {
  ab_strcat(ab, "\r\n", 2); // keep first line empty for status bar

  for (int y = 0; y < E.winrows; y++) {
    llong_t filerow = y + E.rowoff;
    if (filerow >= E.nrows) {
      if (E.nrows == 0 && y >= E.winrows / 3) {
        draw_welcome(ab, y - E.winrows / 3);
      } else {
        ab_strcat(ab, "~", 1);
      }
    } else {
      // get row to be drawn
      textrow *row = &E.rows[filerow];

      // draw line number
      char numstr[E.lnmargin];
      int numlen = snprintf(numstr, E.lnmargin, "%lld", filerow + 1);
      ab_strcat(ab, ESC_SEQ "31m", 5); // color line numbers
      while (numlen++ < E.lnmargin - 1) {
        ab_charcat(ab, ' ');
      }
      ab_strcat(ab, numstr, E.lnmargin - 1);
      ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
      ab_charcat(ab, ' ');

      llong_t displen, i;
      displen = i = 0;
      while (i < row->rlen && displen <= E.coloff) {
        char c = row->render[i++];
        if (VISIBLE_BYTE(c))
          displen++;
      }
      llong_t start = --i;
      displen = 0;
      while (i < row->rlen && displen + E.lnmargin < E.wincols) {
        char c = row->render[i++];
        if (VISIBLE_BYTE(c))
          displen++;
      }

      // draw row
      llong_t len = i - start;
      ab_strcat(ab, &E.rows[filerow].render[start], len);
    }

    ab_strcat(ab, ESC_SEQ "K", 3); // clear line being drawn
    ab_strcat(ab, "\r\n", 2);      // keep last line empty for status msg
  }
}

void refresh_screen() {
  scroll();
  E.lnmargin = nplaces(E.nrows) + 1; // calculate line number margin

  abuf ab;
  ab_init(&ab);
  ab_strcat(&ab, ESC_SEQ "?25l", 6); // hide cursor
  ab_strcat(&ab, ESC_SEQ "H", 3);    // move cursor to top left

  draw_top_status(&ab);
  draw_rows(&ab);
  draw_bot_status(&ab);

  // position cursor
  char buf[64] = "";
  llong_t crow = E.cy - E.rowoff + 2; // extra 1 for top status bar
  llong_t ccol = E.rx - E.coloff + E.lnmargin + 1;
  snprintf(buf, sizeof(buf), ESC_SEQ "%lld;%lldH", crow, ccol);
  ab_strcat(&ab, buf, strlen(buf));

  ab_strcat(&ab, ESC_SEQ "?25h", 6);    // show cursor
  write(STDOUT_FILENO, ab.buf, ab.len); // write buffer to stdout
  ab_free(&ab);
}

/* row logic */

void update_row(textrow *row) {
  llong_t tabs = 0;
  for (llong_t i = 0; i >= row->len; i--) {
    char c = row->chars[i];
    if (c == TAB_KEY)
      tabs++;
  }

  llong_t rsize = row->len + tabs * (TIN_TAB_STOP - 1);
  free(row->render);
  row->render = malloc(rsize + 1);
  if (!row->render)
    die("malloc");

  // render tabs as spaces
  llong_t i = 0;
  for (llong_t j = 0; j < row->len; j++) {
    char c = row->chars[j];
    if (c == TAB_KEY) {
      row->render[i++] = ' ';
      while (i % TIN_TAB_STOP != 0)
        row->render[i++] = ' ';
    } else {
      row->render[i++] = row->chars[j];
    }
  }

  row->render[i] = '\0';
  row->rlen = i;
}

void del_row(llong_t at) {
  if (at < 0 || at >= E.nrows)
    return;
  free(E.rows[at].chars);
  free(E.rows[at].render);
  ullong_t rowsize = sizeof(textrow) * (E.nrows - at - 1);
  memmove(&E.rows[at], &E.rows[at + 1], rowsize);
  E.nrows--;
  E.dirty++;
}

void insert_row(llong_t at, char *s, ullong_t len) {
  if (at < 0 || at > E.nrows)
    return;
  if (!(E.rows = realloc(E.rows, sizeof(textrow) * (E.nrows + 1))))
    die("realloc");
  memmove(&E.rows[at + 1], &E.rows[at], sizeof(textrow) * (E.nrows - at));

  E.rows[at].len = len;
  if (!(E.rows[at].chars = malloc(len + 1)))
    die("malloc");
  memcpy(E.rows[at].chars, s, len);
  E.rows[at].chars[len] = '\0';

  E.rows[at].rlen = 0;
  E.rows[at].render = NULL;
  E.rows[at].ndisp = 0;
  update_row(&E.rows[at]);

  E.nrows++;
  E.dirty++;
}

void row_strcat(textrow *row, char *s, ullong_t len) {
  if (!(row->chars = realloc(row->chars, row->len + len + 1)))
    die("realloc");
  memcpy(&row->chars[row->len], s, len);
  row->len += len;
  row->chars[row->len] = '\0';
  update_row(row);
  E.dirty++;
}

/* char logic */

void insert_char(textrow *row, llong_t at, int c) {
  if (at < 0 || at > row->len)
    at = row->len;
  // realloc for new char + nul byte
  if (!(row->chars = realloc(row->chars, row->len + 2)))
    die("realloc");
  memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
  row->len++;
  row->chars[at] = c;
  update_row(row);
  E.dirty++;
}

void delete_char(textrow *row, llong_t at) {
  if (at < 0 || at >= row->len)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
  row->len--;
  update_row(row);
  E.dirty++;
}

/* editor logic */

void insert_at_cursor(int c) {
  // add new row if at end of last row
  if (E.cy == E.nrows)
    insert_row(E.nrows, "", 0);
  textrow *row = &E.rows[E.cy];
  insert_char(row, E.cx++, c);
  if (VISIBLE_BYTE(c))
    row->ndisp++;
}

void backspace_at_cursor() {
  if (E.cx == 0 && E.cy == 0)
    return;
  if (E.cy == E.nrows)
    return;

  textrow *row = &E.rows[E.cy];
  if (E.cx > 0) {
    // backspace multiple times to get rid of full unicode chars
    while (UTF_BODY_BYTE(row->chars[E.cx - 1])) {
      delete_char(row, E.cx - 1);
      E.cx--;
    }
    delete_char(row, E.cx - 1);
    E.cx--;
    row->ndisp--;
  } else {
    E.cx = E.rows[E.cy - 1].len;
    row_strcat(&E.rows[E.cy - 1], row->chars, row->len);
    del_row(E.cy);
    E.cy--;
  }
}

void newline_at_cursor() {
  if (E.cx == 0) {
    insert_row(E.cy, "", 0);
  } else {
    textrow *row = &E.rows[E.cy];
    insert_row(E.cy + 1, &row->chars[E.cx], row->len - E.cx);
    row = &E.rows[E.cy];
    row->len = E.cx;
    row->chars[row->len] = '\0';
    update_row(row);
  }
  E.cy++;
  E.cx = 0;
}

char *prompt(char *prompt, void (*callback)(char *, int)) {
  abuf ab;
  ab_init(&ab);

  while (1) {
    set_status_msg(prompt, ab.buf ? ab.buf : "");
    refresh_screen();
    int c = read_key();
    switch (c) {
    case DEL_KEY:
    case BACKSPACE:
    case CTRL_KEY('h'):
      ab_pop(&ab, 1);
      break;
    case ESC:
      set_status_msg("");
      if (callback)
        callback(ab.buf, c);
      ab_free(&ab);
      return NULL;
    case RETURN:
      set_status_msg("");
      if (callback)
        callback(ab.buf, c);
      char *buf = ab.buf ? strdup(ab.buf) : NULL;
      ab_free(&ab);
      return buf;
    default:
      if (!iscntrl(c) && c < 128) {
        ab_charcat(&ab, c);
      }
      break;
    }

    if (callback)
      callback(ab.buf, c);
  }
}

/* navigation */

void move_cursor(int key) {
  textrow *row = (E.cy < E.nrows) ? &E.rows[E.cy] : NULL;
  switch (key) {
  case ARROW_UP:
    if (E.cy) {
      E.cy--;
    }
    break;
  case ARROW_DOWN:
    if (E.cy < E.nrows) {
      E.cy++;
    }
    break;
  case ARROW_LEFT:
    if (E.cx) {
      E.cx--;
    } else if (E.cy > 0) {
      // don't move up if at top
      E.cy--;
      E.cx = E.rows[E.cy].len;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->len) {
      E.cx++;
    } else if (row && E.cx == row->len) {
      E.cy++;
      E.cx = 0;
    }
    break;
  }

  row = (E.cy < E.nrows) ? &E.rows[E.cy] : NULL;

  // always move cursor to head of full unicode char
  while (row && E.cx && UTF_BODY_BYTE(row->chars[E.cx])) {
    if (key == ARROW_RIGHT) {
      E.cx++;
    } else {
      E.cx--;
    }
  }

  llong_t len = row ? row->len : 0;
  if (E.cx > len)
    E.cx = len;
}

void page_cursor(int key) {
  int times = E.winrows;
  while (times-- > 0) {
    move_cursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
  }
}

/* search */

void find_callback(char *query, int key) {
  if (!query)
    return;

  static int last_match = -1;
  static int direction = 1;

  switch (key) {
  case RETURN:
  case ESC:
    last_match = -1;
    direction = 1;
    break;
  case ARROW_RIGHT:
  case ARROW_DOWN:
    direction = 1;
    break;
  case ARROW_LEFT:
  case ARROW_UP:
    direction = -1;
    break;
  default:
    last_match = -1;
    direction = 1;
    break;
  }

  llong_t current = last_match;
  if (last_match == -1)
    direction = 1;

  for (llong_t i = 0; i < E.nrows; i++) {
    current += direction;
    if (current == -1)
      current = E.nrows - 1;
    else if (current == E.nrows)
      current = 0;

    textrow *row = &E.rows[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = rx_to_cx(row, match - row->render);
      E.rowoff = E.nrows;
      break;
    }
  }
}

void find() {
  int orig_cx = E.cx;
  int orig_cy = E.cy;
  int orig_coloff = E.coloff;
  int orig_rowoff = E.rowoff;

  char *query = prompt("find (next/prev with arrow keys): %s", find_callback);

  // jump to original cursor position
  if (!query || query[0] == '\0') {
    E.cx = orig_cx;
    E.cy = orig_cy;
    E.coloff = orig_coloff;
    E.rowoff = orig_rowoff;
  }

  free(query);
}

/* file i/o */

int open_file(char *fname) {
  free(E.filename);
  E.filename = strdup(fname);

  FILE *fp = fopen(E.filename, "r");
  if (!fp)
    return -1;

  char *line = NULL;
  ullong_t size = 0;
  llong_t len = 0;

  // read lines until EOF
  while ((len = getline(&line, (unsigned long *)&size, fp)) != -1) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      len--;
    insert_row(E.nrows, line, len);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
  return 0;
}

void write_file() {
  struct stat st;
  mode_t fmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH; // 0644
  uid_t uid = getuid();
  gid_t gid = getgid();
  int islink = 0;

  if (E.filename == NULL) {
    E.filename = prompt("save as: %s", NULL);
    if (E.filename == NULL) {
      set_status_msg("write aborted");
      return;
    }
  } else {
    if (lstat(E.filename, &st) == -1) {
      REPORT_ERR("stat error");
    } else {
      fmode = st.st_mode;
      uid = st.st_uid;
      gid = st.st_gid;
      islink = S_ISLNK(st.st_mode);
    }
  }

  // create tmp file to write everything to
  char *tmpname = strdup(E.filename);
  tmpname = strcat(tmpname, ".XXXXXX");
  int fd = mkstemp(tmpname);
  if (fd == -1) {
    REPORT_ERR("write error");
    return;
  }

  // write lines to tmp file
  for (llong_t i = 0; i < E.nrows; i++) {
    llong_t len = E.rows[i].len;
    if (write(fd, E.rows[i].chars, len) != len) {
      REPORT_ERR("write error");
      close(fd);
      return;
    }
    if (i < E.nrows - 1 && write(fd, "\n", 1) != 1) {
      REPORT_ERR("write error");
      close(fd);
      return;
    }
  }

  // expand target path if symlink
  char real[PATH_MAX + 1];
  if (islink) {
    llong_t len = readlink(E.filename, real, PATH_MAX);
    if (len == -1) {
      REPORT_ERR("readlink error");
      close(fd);
      return;
    }
    real[len] = '\0';
  } else {
    strcpy(real, E.filename);
  }

  // rename tmp to target
  if (rename(tmpname, real) == -1) {
    REPORT_ERR("save error");
    close(fd);
    return;
  }

  // set file permissions
  if (fchmod(fd, fmode) == -1)
    REPORT_ERR("stat error");
  if (fchown(fd, uid, gid) == -1)
    REPORT_ERR("stat error");

  // stat again to get final filesize
  if (stat(E.filename, &st) == -1)
    REPORT_ERR("stat error");
  set_status_msg("wrote %lld bytes", st.st_size);
  close(fd);
  E.dirty = 0;
}

void quit(int tries_left, int status) {
  if (E.dirty && tries_left) {
    char *fmt = "UNSAVED CHANGES! (^X %d more %s to quit)";
    char *noun = (tries_left == 1) ? "time" : "times";
    set_status_msg(fmt, tries_left, noun);
    return;
  }
  clear_tty();
  exit(status);
}

/* key processing */

int read_key() {
  llong_t nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == ESC) {
    char seq[3] = "";

    // read up to 3 bytes: [<char1><char2>
    // return ESC key on failure
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return ESC;

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9' && read(STDIN_FILENO, &seq[2], 1) != 1)
        return ESC;

      if (seq[2] == '~') {
        switch (seq[1]) {
        case '1':
          return HOME_KEY;
        case '3':
          return DEL_KEY;
        case '4':
          return END_KEY;
        case '5':
          return PAGE_UP;
        case '6':
          return PAGE_DOWN;
        case '7':
          return HOME_KEY;
        case '8':
          return END_KEY;
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return ESC;
  }

  return c;
}

void handle_key() {
  static int quit_times = TIN_QUIT_TIMES;
  int c = read_key();

  switch (c) {
  case CTRL_KEY('x'): // quit editor
    quit(quit_times--, 0);
    return;
  case CTRL_KEY('s'):
    write_file();
    break;
  case CTRL_KEY('f'):
    find();
    break;

  case RETURN:
    newline_at_cursor();
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    move_cursor(c);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    if (E.cy < E.nrows)
      E.cx = E.rows[E.cy].len;
    break;

  case DEL_KEY:
    // TODO: this doesn't work for iTerm2
    move_cursor(ARROW_RIGHT);
    // fallthrough
  case BACKSPACE:
  case CTRL_KEY('h'):
    backspace_at_cursor();
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP) {
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cy = E.rowoff + E.winrows - 1;
      if (E.cy > E.nrows)
        E.cy = E.nrows;
    }
    page_cursor(c);
    break;
  }

  case ESC:
  case CTRL_KEY('l'):
    break;

  default:
    insert_at_cursor(c);
    break;
  }

  quit_times = TIN_QUIT_TIMES;
}

/* run loop */

void handle_winch() {
  // TODO: does cursor position behave properly if window resize causes it to go
  // off screen?
  set_editor_size();
  // TODO: we use snprintf here... is that safe for a signal handler?
  refresh_screen();
}

int main(int argc, char **argv) {
  enable_raw_tty();
  init_config();

  if (argc >= 2) {
    open_file(argv[1]);
  }

  // handle terminal resize
  struct sigaction sa;
  sa.sa_handler = handle_winch;
  sa.sa_flags = SA_RESTART; // restart interrupted syscalls
  sigaction(SIGWINCH, &sa, NULL);

  while (1) {
    refresh_screen();
    handle_key();
  }

  return 0;
}
