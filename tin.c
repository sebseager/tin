#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "abuf.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */

#define TIN_VERSION "0.1.0"
#define ESC_SEQ "\x1b["
#define CTRL_KEY(key) ((key)&0x1f)

enum special_key {
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
  ssize_t len;
  char *buf;
  ssize_t rlen;
  char *render;
} textrow;

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

/* editor config */

struct config {
  ssize_t cx, cy;           // cursor position
  ssize_t winrows, wincols; // window size
  ssize_t rowoff, coloff;   // scroll offsets
  ssize_t nrows;            // number of text rows
  textrow *rows;            // text lines
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

void set_editor_size() {
  if (measure_window(&cfg.winrows, &cfg.wincols) == -1)
    die("measure_screen");
}

void init_config() {
  cfg.cx = cfg.cy = 0;
  cfg.rowoff = cfg.coloff = 0;
  cfg.nrows = 0;
  cfg.rows = NULL;
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

/* interface */

void draw_welcome(abuf *ab, int line) {
  char msg[80];
  int len;
  switch (line) {
  case 0:
    len = snprintf(msg, sizeof(msg), "TIN - TIN's Not Nano");
    break;
  case 1:
    len = snprintf(msg, sizeof(msg), "version %s", TIN_VERSION);
    break;
  default:
    len = 0;
  }

  len = (len > cfg.wincols) ? cfg.wincols : len;
  int pad = (cfg.wincols - len) / 2;
  if (pad) {
    ab_append(ab, "~", 1);
    pad--;
  }
  while (pad-- > 0)
    ab_append(ab, " ", 1);
  ab_append(ab, msg, len);
}

void scroll() {
  if (cfg.cy < cfg.rowoff)
    cfg.rowoff = cfg.cy;
  if (cfg.cy >= cfg.rowoff + cfg.winrows)
    cfg.rowoff = cfg.cy - cfg.winrows + 1;
  if (cfg.cx < cfg.coloff)
    cfg.coloff = cfg.cx;
  if (cfg.cx >= cfg.coloff + cfg.wincols)
    cfg.coloff = cfg.cx - cfg.wincols + 1;
}

void draw_rows(abuf *ab) {
  for (int y = 0; y < cfg.winrows; y++) {
    ssize_t filerow = y + cfg.rowoff;
    if (y >= cfg.nrows) {
      if (cfg.nrows == 0 && y >= cfg.winrows / 3) {
        draw_welcome(ab, y - cfg.winrows / 3);
      } else {
        ab_append(ab, "~", 1);
      }
    } else {
      ssize_t len = cfg.rows[filerow].len - cfg.coloff;
      if (len < 0)
        len = 0;
      else if (len > cfg.wincols)
        len = cfg.wincols;
      ab_append(ab, cfg.rows[filerow].buf + cfg.coloff, len);
    }

    ab_append(ab, ESC_SEQ "K", 3); // clear line being drawn
    if (y < cfg.winrows - 1) {
      ab_append(ab, "\r\n", 2);
    }
  }
}

void refresh_screen() {
  scroll();

  abuf ab;
  ab_init(&ab);
  ab_append(&ab, ESC_SEQ "?25l", 6); // hide cursor
  ab_append(&ab, ESC_SEQ "H", 3);    // move cursor to top left

  draw_rows(&ab);
  char buf[64] = "";
  ssize_t crow = cfg.cy - cfg.rowoff + 1;
  ssize_t ccol = cfg.cx - cfg.coloff + 1;
  snprintf(buf, sizeof(buf), ESC_SEQ "%zd;%zdH", crow, ccol);
  ab_append(&ab, buf, strlen(buf));

  ab_append(&ab, ESC_SEQ "?25h", 6); // show cursor
  write(STDOUT_FILENO, ab.buf, ab.len);
  ab_free(&ab);
}

/* file i/o */

void update_row(textrow *row) {
  free(row->buf);
  row->buf = malloc(row->len + 1);

  ssize_t i;
  for (ssize_t j = 0; j < row->len; j++) {
    row->render[i++] = row->buf[j];
  }

  row->render[i] = '\0';
  row->rlen = i;
}

void append_row(char *s, size_t len) {
  cfg.rows = realloc(cfg.rows, sizeof(textrow) * (cfg.nrows + 1));
  size_t n = cfg.nrows;

  cfg.rows[n].len = len;
  cfg.rows[n].buf = malloc(len + 1);
  if (!cfg.rows[n].buf)
    die("malloc");
  memcpy(cfg.rows[n].buf, s, len);
  cfg.rows[n].buf[len] = '\0';

  cfg.rows[n].rlen = 0;
  cfg.rows[n].render = NULL;

  cfg.nrows++;
}

void open_file(char *fname) {
  FILE *fp = fopen(fname, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t size = 0;
  ssize_t len = 0;

  // read lines until EOF
  while ((len = getline(&line, &size, fp)) != -1) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      len--;
    append_row(line, len);
  }

  free(line);
  fclose(fp);
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
    if (cfg.cx)
      cfg.cx--;
    else if (cfg.cy > 0) { // don't move up if at top
      cfg.cy--;
      cfg.cx = cfg.rows[cfg.cy].len;
    }
    break;
  case ARROW_RIGHT:
    if (row && cfg.cx < row->len)
      cfg.cx++;
    else if (row && cfg.cx == row->len) {
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

/* text processing */

int read_key() {
  ssize_t nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    // ignore EINTR to retry after interrupt
    if (nread == -1 && errno != EAGAIN && errno != EINTR)
      die("read");
  }

  if (c == ESC_SEQ[0]) {
    char seq[3] = "";

    // read up to 3 bytes: [<char1><char2>
    // return ESC key on failure
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return ESC_SEQ[0];
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return ESC_SEQ[0];

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9' && read(STDIN_FILENO, &seq[2], 1) != 1)
        return ESC_SEQ[0];

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

    return ESC_SEQ[0];
  }

  return c;
}

void handle_key() {
  int c = read_key();
  switch (c) {
  case CTRL_KEY('w'):
    clear_tty();
    exit(0);
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
    cfg.cx = cfg.wincols - 1;
    break;
  case PAGE_UP:
  case PAGE_DOWN:
    page_cursor(c);
    break;
  }
}

int main(int argc, char **argv) {
  enable_raw_tty();
  init_config();

  if (argc >= 2) {
    open_file(argv[1]);
  }

  // handle terminal resize
  struct sigaction sa;
  sa.sa_handler = set_editor_size;
  sa.sa_flags = 0;
  sigaction(SIGWINCH, &sa, NULL);

  while (1) {
    refresh_screen();
    handle_key();
  }

  return 0;
}
