/* chmap.c: channel map parsing and frame assembly (see chmap.h). */
#include "chmap.h"

#include <ctype.h>
#include <string.h>

static int parse_one(const char *s, size_t len, unsigned allowed, mpcua_chsrc *out) {
  while (len && isspace((unsigned char)*s)) { s++; len--; }
  while (len && isspace((unsigned char)s[len - 1])) len--;
  if (!len) return -1;
  if ((len == 1 && (*s == '0' || *s == '-'))) { out->kind = MPCUA_SRC_NONE; out->idx = 0; return 0; }
  static const struct { const char *name; uint8_t kind; } kinds[] = {
    {"out", MPCUA_SRC_OUT}, {"in", MPCUA_SRC_IN}, {"host", MPCUA_SRC_HOST},
  };
  for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    size_t nl = strlen(kinds[k].name);
    if (len <= nl || strncmp(s, kinds[k].name, nl) != 0) continue;
    if (!(allowed & kinds[k].kind)) return -1;
    unsigned v = 0;
    for (size_t i = nl; i < len; i++) {
      if (!isdigit((unsigned char)s[i])) return -1;
      v = v * 10 + (unsigned)(s[i] - '0');
      if (v > MPCUA_MAX_CH) return -1;
    }
    if (v < 1) return -1;
    out->kind = kinds[k].kind; out->idx = (uint8_t)(v - 1);
    return 0;
  }
  return -1;
}

int mpcua_chmap_parse(const char *spec, unsigned allowed, mpcua_chsrc *map, int max) {
  if (!spec || max <= 0) return -1;
  int n = 0;
  const char *p = spec;
  for (;;) {
    const char *c = strchr(p, ',');
    size_t len = c ? (size_t)(c - p) : strlen(p);
    if (n >= max) return -1;
    if (parse_one(p, len, allowed, &map[n]) < 0) return -1;
    n++;
    if (!c) break;
    p = c + 1;
  }
  return n;
}

void mpcua_chmap_frame(const mpcua_chsrc *map, int n, const int32_t *outf, unsigned out_ch,
                       const int32_t *inf, unsigned in_ch, const int32_t *hostf, unsigned host_ch,
                       int32_t *dst) {
  for (int i = 0; i < n; i++) {
    const mpcua_chsrc *m = &map[i];
    int32_t v = 0;
    switch (m->kind) {
    case MPCUA_SRC_OUT: if (outf && m->idx < out_ch) v = outf[m->idx]; break;
    case MPCUA_SRC_IN: if (inf && m->idx < in_ch) v = inf[m->idx]; break;
    case MPCUA_SRC_HOST: if (hostf && m->idx < host_ch) v = hostf[m->idx]; break;
    default: break;
    }
    dst[i] = v;
  }
}
