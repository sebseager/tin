#include <stdlib.h>

/* append buffer */

typedef struct abuf {
  size_t len;
  size_t size;
  char *buf;
} abuf;

void ab_init(struct abuf *ab);

int ab_charcat(struct abuf *ab, char c);

int ab_strcat(struct abuf *ab, const char *s, size_t len);

void ab_pop(struct abuf *ab, ssize_t times);

void ab_free(struct abuf *ab);
