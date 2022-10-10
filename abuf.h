#include <stdlib.h>

/* append buffer */

typedef struct abuf {
  unsigned long long len;
  unsigned long long size;
  char *buf;
} abuf;

void ab_init(struct abuf *ab);

int ab_charcat(struct abuf *ab, char c);

int ab_strcat(struct abuf *ab, const char *s, unsigned long long len);

void ab_pop(struct abuf *ab, unsigned long long times);

void ab_free(struct abuf *ab);
