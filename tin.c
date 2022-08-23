#include "tin.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */

#define ESC_SEQ "\x1b["
#define CTRL_KEY(key) ((key)&0x1f)

/* helpers */

void clear_tty() {
  write(STDOUT_FILENO, ESC_SEQ "2J", 4); // clear whole screen
  write(STDOUT_FILENO, ESC_SEQ "H", 3);  // move cursor to top left
}

void die(const char *s) {
  clear_tty();
  perror(s);
  exit(1);
}

/* append buffer */

struct abuf {
  char *buf;
  size_t len;
  size_t size;
};

void abuf_init(struct abuf *ab) {
  ab->buf = NULL;
  ab->len = 0;
  ab->size = 0;
}

void abuf_append(struct abuf *ab, const char *s, size_t len) {
  size_t new_len = ab->len + len;
  if (new_len > ab->size) {
    ab->size = !ab->size ? new_len : ab->size * 2;
    ab->buf = realloc(ab->buf, ab->size);
    if (!ab->buf)
      die("realloc");
  }
  memcpy(&ab->buf[ab->len], s, len);
  ab->len = new_len;
}

void abuf_free(struct abuf *ab) { free(ab->buf); }

/* editor config */

struct config {
  int n_rows;
  int n_cols;
  struct termios orig_tty;
};

struct config cfg;

int cursor_pos(int *rows, int *cols) {
  if (write(STDOUT_FILENO, ESC_SEQ "6n", 4) != 4)
    return -1;

  char buf[32] = {'\0'};
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
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int measure_screen(int *rows, int *cols) {
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

void init_config() {
  if (measure_screen(&cfg.n_rows, &cfg.n_cols) == -1)
    die("measure_screen");
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

void draw_rows(struct abuf *ab) {
  for (int y = 0; y < cfg.n_rows; y++) {
    abuf_append(ab, "~", 1);
    if (y < cfg.n_rows - 1)
      abuf_append(ab, "\r\n", 2);
  }
}

void refresh_screen() {
  struct abuf ab;
  abuf_init(&ab);
  abuf_append(&ab, ESC_SEQ "2J", 4); // clear screen
  abuf_append(&ab, ESC_SEQ "H", 3);  // move cursor to top left

  draw_rows(&ab);

  abuf_append(&ab, ESC_SEQ "H", 3);
  write(STDOUT_FILENO, ab.buf, ab.len);
  abuf_free(&ab);
}

/* text processing */

char read_key() {
  ssize_t n_read;
  char c;
  while ((n_read = read(STDIN_FILENO, &c, 1)) != 1) {
    if (n_read == -1 && errno != EAGAIN)
      die("read");
  }
  return c;
}

void handle_key() {
  char c = read_key();
  switch (c) {
  case CTRL_KEY('w'):
    clear_tty();
    exit(0);
    break;
  }
}

int main() {
  enable_raw_tty();
  init_config();

  while (1) {
    refresh_screen();
    handle_key();
  }

  return 0;
}
