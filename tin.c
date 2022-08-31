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

#define TIN_VERSION "0.1.0"
#define TIN_TAB_STOP 4
#define TIN_STATUS_MSG_SECS 2
#define TIN_QUIT_TIMES 3
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

typedef struct textrow {
  ssize_t len;  // number of raw chars
  ssize_t ilen; // number of invisible raw chars (e.g. UTF extra bytes)
  char *chars;  // raw chars
  ssize_t rlen; // number of rendered chars (e.g. tabs show as spaces)
  char *render; // rendered chars
} textrow;

/* prototypes */

int read_key();

/* helpers */

void clear_tty() {
  write(STDOUT_FILENO, ESC_SEQ "2J", 4); // clear screen
  write(STDOUT_FILENO, ESC_SEQ "H", 3);  // move cursor to top left
}

void die(const char *s) {
  clear_tty();
  perror(s);
  exit(1);
}

int nplaces(ssize_t n) {
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

/* config */

struct config {
  ssize_t cx, cy;           // cursor position
  ssize_t rx;               // horizontal cursor render position
  ssize_t winrows, wincols; // window size
  ssize_t rowoff, coloff;   // scroll offsets
  ssize_t numoff;           // line number offset
  ssize_t nrows;            // number of text rows
  textrow *rows;            // text lines
  char *filename;           // filename
  char statusmsg[128];      // status message
  time_t statusmsg_time;    // time status message was last updated
  size_t dirty;             // number of changes since last save
  struct termios orig_tty;
};

struct config cfg;

int cursor_pos(ssize_t *rows, ssize_t *cols) {
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
  if (sscanf(&buf[2], "%zd;%zd", rows, cols) != 2)
    return -1;

  return 0;
}

int measure_window(ssize_t *rows, ssize_t *cols) {
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

void set_numoff() {
  cfg.wincols += cfg.numoff;
  cfg.numoff = nplaces(cfg.nrows) + 1;
  cfg.wincols -= cfg.numoff;
}

void set_editor_size() {
  if (measure_window(&cfg.winrows, &cfg.wincols) == -1)
    die("measure_window");
  cfg.winrows -= 2; // for status bar and status message
  set_numoff();
}

void init_config() {
  cfg.cx = cfg.cy = cfg.rx = 0;
  cfg.rowoff = cfg.coloff = 0;
  cfg.nrows = 0;
  cfg.rows = NULL;
  cfg.filename = NULL;
  cfg.statusmsg[0] = '\0';
  cfg.statusmsg_time = 0;
  cfg.dirty = 0;
  set_editor_size();
}

/* terminal control */

void disable_raw_tty() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &cfg.orig_tty) == -1)
    die("tcsetattr");
}

void enable_raw_tty() {
  if (tcgetattr(STDIN_FILENO, &cfg.orig_tty) == -1)
    die("tcgetattr");

  atexit(disable_raw_tty);
  struct termios tty = cfg.orig_tty;

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
  char *fname = cfg.filename ? cfg.filename : "[New]";
  char *dirty = cfg.dirty ? "*" : " ";
  ssize_t row = cfg.rows ? cfg.cy + 1 : 0;
  ssize_t col = cfg.rx + 1;
  ssize_t nrows = cfg.nrows;
  ssize_t ncols = (cfg.rows && cfg.cy < cfg.nrows ? cfg.rows[cfg.cy].rlen : 0);

  // build status bar
  ssize_t barlen = cfg.wincols + cfg.numoff;
  char *lfmt = "[%s] %.20s";
  char *rfmt = "line %zd/%zd, col %zd/%zd";
  char lmsg[barlen + 1], rmsg[barlen + 1];
  ssize_t rlen = barlen;
  rlen = snprintf(rmsg, rlen, rfmt, row, nrows, col, ncols);
  ssize_t llen = barlen - rlen;
  llen = snprintf(lmsg, llen, lfmt, dirty, fname);

  // write status bar
  ab_strcat(ab, lmsg, llen);
  ssize_t nspaces = barlen - rlen - llen;
  while (nspaces-- > 0)
    ab_charcat(ab, ' ');
  ab_strcat(ab, rmsg, rlen);
  ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
}

void draw_bot_status(abuf *ab) {
  ab_strcat(ab, ESC_SEQ "K", 3);  // clear message bar
  ab_strcat(ab, ESC_SEQ "7m", 4); // reverse colors

  // clear message after timeout
  if (time(NULL) - cfg.statusmsg_time >= TIN_STATUS_MSG_SECS)
    cfg.statusmsg[0] = '\0';

  ssize_t barlen = cfg.wincols + cfg.numoff;
  ssize_t msglen = strlen(cfg.statusmsg);
  if (msglen > barlen)
    msglen = barlen;
  if (msglen)
    ab_strcat(ab, cfg.statusmsg, msglen);
  size_t nspaces = barlen - msglen;
  while (nspaces-- > 0)
    ab_strcat(ab, " ", 1);

  ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
}

void set_status_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cfg.statusmsg, sizeof(cfg.statusmsg), fmt, ap);
  va_end(ap);
  cfg.statusmsg_time = time(NULL);
}

/* main interface */

ssize_t cx_to_rx(textrow *row, ssize_t cx) {
  ssize_t rx = 0;
  for (ssize_t j = 0; j < cx; j++) {
    char c = row->chars[j];
    if (c == TAB_KEY)
      rx += TIN_TAB_STOP - (rx % TIN_TAB_STOP);
    else if (UTF_HEAD_BYTE(c))
      rx += 1;
    else if (UTF_BODY_BYTE(c))
      continue;
    else
      rx++;
  }
  return rx;
}

ssize_t rx_to_cx(textrow *row, int rx) {
  ssize_t cx, cur_rx = 0;
  for (cx = 0; cx < row->len; cx++) {
    char c = row->chars[cx];
    if (c == TAB_KEY)
      cur_rx += TIN_TAB_STOP - (cur_rx % TIN_TAB_STOP);
    else if (UTF_HEAD_BYTE(c))
      cur_rx += 1;
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
    len = snprintf(msg, sizeof(msg), "^X exit   ^S save   ^F find");
    break;
  default:
    len = 0;
  }

  len = (len > cfg.wincols) ? cfg.wincols : len;
  int pad = (cfg.wincols - len) / 2;
  if (pad) {
    ab_strcat(ab, "~", 1);
    pad--;
  }
  while (pad-- > 0)
    ab_strcat(ab, " ", 1);
  ab_strcat(ab, msg, len);
}

void scroll() {
  // TODO: FIX SCROLLING BEHAVIOR SO YOU ONLY EVER MOVE WHOLE UNICODE CHARS AT A
  // TIME

  // calculate index into render buffer
  // differs from cx if line contains tabs
  cfg.rx = 0;
  if (cfg.cy < cfg.nrows) {
    cfg.rx = cx_to_rx(&cfg.rows[cfg.cy], cfg.cx);
  }

  // adjust offsets if cursor if off screen
  if (cfg.cy < cfg.rowoff)
    cfg.rowoff = cfg.cy;
  if (cfg.cy >= cfg.rowoff + cfg.winrows)
    cfg.rowoff = cfg.cy - cfg.winrows + 1;
  if (cfg.rx < cfg.coloff)
    cfg.coloff = cfg.rx;
  if (cfg.rx >= cfg.coloff + cfg.wincols)
    cfg.coloff = cfg.rx - cfg.wincols + 1;
}

void draw_rows(abuf *ab) {
  ab_strcat(ab, "\r\n", 2); // keep first line empty for status bar

  for (int y = 0; y < cfg.winrows; y++) {
    ssize_t filerow = y + cfg.rowoff;
    if (filerow >= cfg.nrows) {
      if (cfg.nrows == 0 && y >= cfg.winrows / 3) {
        draw_welcome(ab, y - cfg.winrows / 3);
      } else {
        ab_strcat(ab, "~", 1);
      }
    } else {
      textrow *row = &cfg.rows[filerow];
      ssize_t printlen = row->rlen - cfg.coloff;
      if (printlen < 0)
        printlen = 0;
      else if (printlen > cfg.wincols + row->ilen)
        printlen = cfg.wincols;

      // draw line number
      char numstr[cfg.numoff];
      int numlen = snprintf(numstr, cfg.numoff, "%zd", filerow + 1);
      ab_strcat(ab, ESC_SEQ "31m", 5); // color line numbers
      while (numlen++ < cfg.numoff - 1) {
        ab_charcat(ab, ' ');
      }
      ab_strcat(ab, numstr, cfg.numoff - 1);
      ab_strcat(ab, ESC_SEQ "m", 3); // reset colors
      ab_charcat(ab, ' ');

      // draw row
      ab_strcat(ab, row->render + cfg.coloff, printlen);
    }

    ab_strcat(ab, ESC_SEQ "K", 3); // clear line being drawn
    ab_strcat(ab, "\r\n", 2);      // keep last line empty for status msg
  }
}

void refresh_screen() {
  scroll();

  abuf ab;
  ab_init(&ab);
  ab_strcat(&ab, ESC_SEQ "?25l", 6); // hide cursor
  ab_strcat(&ab, ESC_SEQ "H", 3);    // move cursor to top left

  set_numoff();
  draw_top_status(&ab);
  draw_rows(&ab);
  draw_bot_status(&ab);

  // position cursor
  char buf[64] = "";
  ssize_t crow = cfg.cy - cfg.rowoff + 2; // extra 1 for top status bar
  ssize_t ccol = cfg.rx - cfg.coloff + cfg.numoff + 1;
  snprintf(buf, sizeof(buf), ESC_SEQ "%zd;%zdH", crow, ccol);
  ab_strcat(&ab, buf, strlen(buf));

  ab_strcat(&ab, ESC_SEQ "?25h", 6);    // show cursor
  write(STDOUT_FILENO, ab.buf, ab.len); // write buffer to stdout
  ab_free(&ab);
}

/* row logic */

void update_row(textrow *row) {
  ssize_t rsize = row->len;
  row->ilen = 0;
  for (ssize_t j = 0; j < row->len; j++) {
    char c = row->chars[j];
    if (c == TAB_KEY)
      rsize += TIN_TAB_STOP - 1;
    if (UTF_BODY_BYTE(c))
      row->ilen++;
  }

  free(row->render);
  row->render = malloc(rsize + 1);
  if (!row->render)
    die("malloc");

  // render tabs as spaces
  ssize_t i = 0;
  for (ssize_t j = 0; j < row->len; j++) {
    switch (row->chars[j]) {
    case TAB_KEY:
      row->render[i++] = ' ';
      while (i % TIN_TAB_STOP != 0)
        row->render[i++] = ' ';
      break;
    default:
      row->render[i++] = row->chars[j];
      break;
    }
  }

  row->render[i] = '\0';
  row->rlen = i;
}

void insert_row(ssize_t at, char *s, size_t len) {
  if (at < 0 || at > cfg.nrows)
    return;
  if (!(cfg.rows = realloc(cfg.rows, sizeof(textrow) * (cfg.nrows + 1))))
    die("realloc");
  memmove(&cfg.rows[at + 1], &cfg.rows[at], sizeof(textrow) * (cfg.nrows - at));

  cfg.rows[at].len = len;
  if (!(cfg.rows[at].chars = malloc(len + 1)))
    die("malloc");
  memcpy(cfg.rows[at].chars, s, len);
  cfg.rows[at].chars[len] = '\0';

  cfg.rows[at].rlen = 0;
  cfg.rows[at].render = NULL;
  update_row(&cfg.rows[at]);

  cfg.nrows++;
  cfg.dirty++;
}

void del_row(ssize_t at) {
  if (at < 0 || at >= cfg.nrows)
    return;
  free(cfg.rows[at].chars);
  free(cfg.rows[at].render);
  size_t rowsize = sizeof(textrow) * (cfg.nrows - at - 1);
  memmove(&cfg.rows[at], &cfg.rows[at + 1], rowsize);
  cfg.nrows--;
  cfg.dirty++;
}

void row_strcat(textrow *row, char *s, size_t len) {
  if (!(row->chars = realloc(row->chars, row->len + len + 1)))
    die("realloc");
  memcpy(&row->chars[row->len], s, len);
  row->len += len;
  row->chars[row->len] = '\0';
  update_row(row);
  cfg.dirty++;
}

/* char logic */

void insert_char(textrow *row, ssize_t at, int c) {
  if (at < 0 || at > row->len)
    at = row->len;
  // realloc for new char + nul byte
  if (!(row->chars = realloc(row->chars, row->len + 2)))
    die("realloc");
  memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
  row->len++;
  row->chars[at] = c;
  update_row(row);
  cfg.dirty++;
}

void delete_char(textrow *row, ssize_t at) {
  if (at < 0 || at >= row->len)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
  row->len--;
  update_row(row);
  cfg.dirty++;
}

/* editor logic */

void insert_at_cursor(int c) {
  // add new row if at end of last row
  if (cfg.cy == cfg.nrows)
    insert_row(cfg.nrows, "", 0);
  insert_char(&cfg.rows[cfg.cy], cfg.cx++, c);
}

void backspace_at_cursor() {
  if (cfg.cx == 0 && cfg.cy == 0)
    return;
  if (cfg.cy == cfg.nrows)
    return;

  textrow *row = &cfg.rows[cfg.cy];
  if (cfg.cx > 0) {
    // backspace multiple times to get rid of full unicode chars
    while (UTF_BODY_BYTE(row->chars[cfg.cx - 1])) {
      delete_char(row, cfg.cx - 1);
      cfg.cx--;
    }
    delete_char(row, cfg.cx - 1);
    cfg.cx--;
  } else {
    cfg.cx = cfg.rows[cfg.cy - 1].len;
    row_strcat(&cfg.rows[cfg.cy - 1], row->chars, row->len);
    del_row(cfg.cy);
    cfg.cy--;
  }
}

void newline_at_cursor() {
  if (cfg.cx == 0) {
    insert_row(cfg.cy, "", 0);
  } else {
    textrow *row = &cfg.rows[cfg.cy];
    insert_row(cfg.cy + 1, &row->chars[cfg.cx], row->len - cfg.cx);
    row = &cfg.rows[cfg.cy];
    row->len = cfg.cx;
    row->chars[row->len] = '\0';
    update_row(row);
  }
  cfg.cy++;
  cfg.cx = 0;
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
  textrow *row = (cfg.cy < cfg.nrows) ? &cfg.rows[cfg.cy] : NULL;
  switch (key) {
  case ARROW_UP:
    if (cfg.cy)
      cfg.cy--;
    break;
  case ARROW_DOWN:
    if (cfg.cy < cfg.nrows)
      cfg.cy++;
    break;
  case ARROW_LEFT:
    if (cfg.cx) {
      cfg.cx--;
      // always move cursor to head of full unicode char
      while (cfg.cx > 1 && UTF_BODY_BYTE(row->chars[cfg.cx]))
        cfg.cx--;
    } else if (cfg.cy > 0) {
      // don't move up if at top
      cfg.cy--;
      cfg.cx = cfg.rows[cfg.cy].len;
    }
    break;
  case ARROW_RIGHT:
    if (row && cfg.cx < row->len) {
      cfg.cx++;
      // always move cursor past full unicode char
      while (cfg.cx < row->len && UTF_BODY_BYTE(row->chars[cfg.cx]))
        cfg.cx++;
    } else if (row && cfg.cx == row->len) {
      // don't move down if at bottom
      cfg.cy++;
      cfg.cx = 0;
    }
    break;
  }

  row = (cfg.cy < cfg.nrows) ? &cfg.rows[cfg.cy] : NULL;
  ssize_t len = row ? row->len : 0;
  if (cfg.cx > len)
    cfg.cx = len;
}

void page_cursor(int key) {
  int times = cfg.winrows;
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

  ssize_t current = last_match;
  if (last_match == -1)
    direction = 1;

  for (ssize_t i = 0; i < cfg.nrows; i++) {
    current += direction;
    if (current == -1)
      current = cfg.nrows - 1;
    else if (current == cfg.nrows)
      current = 0;

    textrow *row = &cfg.rows[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      cfg.cy = current;
      cfg.cx = rx_to_cx(row, match - row->render);
      cfg.rowoff = cfg.nrows;
      break;
    }
  }
}

void find() {
  int orig_cx = cfg.cx;
  int orig_cy = cfg.cy;
  int orig_coloff = cfg.coloff;
  int orig_rowoff = cfg.rowoff;

  char *query = prompt("find (next/prev with arrow keys): %s", find_callback);

  // jump to original cursor position
  if (!query || query[0] == '\0') {
    cfg.cx = orig_cx;
    cfg.cy = orig_cy;
    cfg.coloff = orig_coloff;
    cfg.rowoff = orig_rowoff;
  }

  free(query);
}

/* file i/o */

int open_file(char *fname) {
  free(cfg.filename);
  cfg.filename = strdup(fname);

  FILE *fp = fopen(cfg.filename, "r");
  if (!fp)
    return -1;

  char *line = NULL;
  size_t size = 0;
  ssize_t len = 0;

  // read lines until EOF
  while ((len = getline(&line, &size, fp)) != -1) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      len--;
    insert_row(cfg.nrows, line, len);
  }

  free(line);
  fclose(fp);
  cfg.dirty = 0;
  return 0;
}

void write_file() {
  struct stat st;
  mode_t fmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH; // 0644
  uid_t uid = getuid();
  gid_t gid = getgid();
  int islink = 0;

  if (cfg.filename == NULL) {
    cfg.filename = prompt("save as: %s", NULL);
    if (cfg.filename == NULL) {
      set_status_msg("write aborted");
      return;
    }
  } else {
    if (lstat(cfg.filename, &st) == -1) {
      REPORT_ERR("stat error");
    } else {
      fmode = st.st_mode;
      uid = st.st_uid;
      gid = st.st_gid;
      islink = S_ISLNK(st.st_mode);
    }
  }

  // create tmp file to write everything to
  char *tmpname = strdup(cfg.filename);
  tmpname = strcat(tmpname, ".XXXXXX");
  int fd = mkstemp(tmpname);
  if (fd == -1) {
    REPORT_ERR("write error");
    return;
  }

  // write lines to tmp file
  for (ssize_t i = 0; i < cfg.nrows; i++) {
    ssize_t len = cfg.rows[i].len;
    if (write(fd, cfg.rows[i].chars, len) != len) {
      REPORT_ERR("write error");
      close(fd);
      return;
    }
    if (i < cfg.nrows - 1 && write(fd, "\n", 1) != 1) {
      REPORT_ERR("write error");
      close(fd);
      return;
    }
  }

  // expand target path if symlink
  char real[PATH_MAX + 1];
  if (islink) {
    ssize_t len = readlink(cfg.filename, real, PATH_MAX);
    if (len == -1) {
      REPORT_ERR("readlink error");
      close(fd);
      return;
    }
    real[len] = '\0';
  } else {
    strcpy(real, cfg.filename);
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
  if (stat(cfg.filename, &st) == -1)
    REPORT_ERR("stat error");
  set_status_msg("wrote %lld bytes", st.st_size);
  close(fd);
  cfg.dirty = 0;
}

void quit(int tries_left, int status) {
  if (cfg.dirty && tries_left) {
    char *fmt = "Unsaved changes in buffer! (press ^X %d more %s to quit)";
    char *noun = (tries_left == 1) ? "time" : "times";
    set_status_msg(fmt, tries_left, noun);
    return;
  }
  clear_tty();
  exit(status);
}

/* key processing */

int read_key() {
  ssize_t nread;
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
    cfg.cx = 0;
    break;
  case END_KEY:
    if (cfg.cy < cfg.nrows)
      cfg.cx = cfg.rows[cfg.cy].len;
    break;

  case DEL_KEY:
    move_cursor(ARROW_RIGHT);
    // fallthrough
  case BACKSPACE:
  case CTRL_KEY('h'):
    backspace_at_cursor();
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP) {
      cfg.cy = cfg.rowoff;
    } else if (c == PAGE_DOWN) {
      cfg.cy = cfg.rowoff + cfg.winrows - 1;
      if (cfg.cy > cfg.nrows)
        cfg.cy = cfg.nrows;
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

int main(int argc, char **argv) {
  enable_raw_tty();
  init_config();

  if (argc >= 2) {
    open_file(argv[1]);
  }

  // handle terminal resize
  struct sigaction sa;
  sa.sa_handler = set_editor_size;
  sa.sa_flags = SA_RESTART; // restart interrupted syscalls
  sigaction(SIGWINCH, &sa, NULL);

  while (1) {
    refresh_screen();
    handle_key();
  }

  return 0;
}
