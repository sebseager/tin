#include "abuf.h"
#include <string.h>
#include <unistd.h>

int ab_realloc(struct abuf *ab, size_t new_len) {
  if (new_len >= ab->size) {
    ab->size = (ab->size + new_len + 1) * 2;
    char *tmp = realloc(ab->buf, ab->size);
    if (!tmp)
      return -1;
    ab->buf = tmp;
  }
  return 0;
}

void ab_init(struct abuf *ab) {
  ab->buf = NULL;
  ab->len = 0;
  ab->size = 0;
}

int ab_charcat(struct abuf *ab, char c) {
  if (ab_realloc(ab, ab->len + 1) == -1)
    return -1;
  ab->buf[ab->len++] = c;
  ab->buf[ab->len] = '\0'; // nul terminate so can read as string
  return 0;
}

int ab_strcat(struct abuf *ab, const char *s, size_t len) {
  size_t new_len = ab->len + len;
  if (ab_realloc(ab, new_len + 1) == -1)
    return -1;
  memcpy(&ab->buf[ab->len], s, len);
  ab->len = new_len;
  ab->buf[ab->len] = '\0'; // nul terminate so can read as string
  return 0;
}

void ab_pop(struct abuf *ab, ssize_t times) {
  while (ab->len > 0 && times-- > 0) {
    ab->len--;
    ab->buf[ab->len] = '\0';
  }
}

void ab_free(struct abuf *ab) { free(ab->buf); }