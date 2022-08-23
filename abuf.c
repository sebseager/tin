#include "abuf.h"
#include <string.h>
#include <unistd.h>

void ab_init(struct abuf *ab) {
  ab->buf = NULL;
  ab->len = 0;
  ab->size = 0;
}

void ab_append(struct abuf *ab, const char *s, size_t len) {
  size_t new_len = ab->len + len;
  if (new_len > ab->size) {
    ab->size = (ab->size + new_len + 1) * 2;
    char *tmp = realloc(ab->buf, ab->size);
    if (!tmp)
      return;
    ab->buf = tmp;
  }
  memcpy(&ab->buf[ab->len], s, len);
  ab->len = new_len;
}

void ab_free(struct abuf *ab) { free(ab->buf); }