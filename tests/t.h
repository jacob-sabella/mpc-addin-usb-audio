/* t.h: minimal test helpers for the offline host tests. */
#ifndef MPCUA_T_H
#define MPCUA_T_H
#include <stdio.h>
#include <stdlib.h>

static int t_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); t_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
  if (_a != _b) { fprintf(stderr, "%s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); t_fail++; } } while (0)
#define T_DONE(name) do { if (t_fail) { fprintf(stderr, "FAIL %s (%d)\n", name, t_fail); return 1; } \
  printf("ok   %s\n", name); return 0; } while (0)
#endif
