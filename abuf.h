#include <stdlib.h>

/* append buffer */

typedef struct abuf {
  size_t len;
  size_t size;
  char *buf;
} abuf;

void ab_init(struct abuf *ab);

int ab_append(struct abuf *ab, const char *s, size_t len);

void ab_free(struct abuf *ab);
