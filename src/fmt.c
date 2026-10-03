/* fmt.c: sample format conversion (see fmt.h). No allocation; safe on the audio thread. */
#include "fmt.h"

#include <string.h>

mpcua_fmt mpcua_fmt_from_alsa(int a) {
  switch (a) {
  case MPCUA_ALSA_S16_LE: return MPCUA_FMT_S16;
  case MPCUA_ALSA_S24_3LE: return MPCUA_FMT_S24_3;
  case MPCUA_ALSA_S24_LE: return MPCUA_FMT_S24_4;
  case MPCUA_ALSA_S32_LE: return MPCUA_FMT_S32;
  case MPCUA_ALSA_FLOAT_LE: return MPCUA_FMT_F32;
  default: return MPCUA_FMT_UNKNOWN;
  }
}

int mpcua_fmt_to_alsa(mpcua_fmt f) {
  switch (f) {
  case MPCUA_FMT_S16: return MPCUA_ALSA_S16_LE;
  case MPCUA_FMT_S24_3: return MPCUA_ALSA_S24_3LE;
  case MPCUA_FMT_S24_4: return MPCUA_ALSA_S24_LE;
  case MPCUA_FMT_S32: return MPCUA_ALSA_S32_LE;
  case MPCUA_FMT_F32: return MPCUA_ALSA_FLOAT_LE;
  default: return -1;
  }
}

unsigned mpcua_fmt_bytes(mpcua_fmt f) {
  switch (f) {
  case MPCUA_FMT_S16: return 2;
  case MPCUA_FMT_S24_3: return 3;
  case MPCUA_FMT_S24_4: case MPCUA_FMT_S32: case MPCUA_FMT_F32: return 4;
  default: return 0;
  }
}

mpcua_fmt mpcua_fmt_from_ssize(unsigned b) {
  return b == 2 ? MPCUA_FMT_S16 : b == 3 ? MPCUA_FMT_S24_3 : b == 4 ? MPCUA_FMT_S32
                                                                    : MPCUA_FMT_UNKNOWN;
}

/* Little-endian loads/stores via memcpy: the device is armv7 LE, the host test is x86 LE. */
static inline int32_t ld32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static inline void st32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }

int32_t mpcua_fmt_get(const void *base, mpcua_fmt f, size_t i) {
  const uint8_t *b = base;
  switch (f) {
  case MPCUA_FMT_S16: { int16_t v; memcpy(&v, b + 2 * i, 2); return (int32_t)((uint32_t)(int32_t)v << 16); }
  case MPCUA_FMT_S24_3: {
    const uint8_t *p = b + 3 * i;
    uint32_t u = ((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24);
    return (int32_t)u;
  }
  case MPCUA_FMT_S24_4: {
    uint32_t u = (uint32_t)ld32(b + 4 * i) << 8; /* drop the unused top byte, left-justify */
    return (int32_t)u;
  }
  case MPCUA_FMT_S32: return ld32(b + 4 * i);
  case MPCUA_FMT_F32: {
    float x; memcpy(&x, b + 4 * i, 4);
    if (x != x) return 0;                /* NaN */
    if (x <= -1.0f) return INT32_MIN;
    if (x >= 1.0f) return INT32_MAX;
    double d = (double)x * 2147483648.0;
    return (int32_t)d;
  }
  default: return 0;
  }
}

void mpcua_fmt_put(void *base, mpcua_fmt f, size_t i, int32_t v) {
  uint8_t *b = base;
  switch (f) {
  case MPCUA_FMT_S16: { int16_t s = (int16_t)(v >> 16); memcpy(b + 2 * i, &s, 2); break; }
  case MPCUA_FMT_S24_3: {
    uint8_t *p = b + 3 * i; uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 8); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 24);
    break;
  }
  case MPCUA_FMT_S24_4: st32(b + 4 * i, v >> 8); break; /* sign-extended 24-bit value */
  case MPCUA_FMT_S32: st32(b + 4 * i, v); break;
  case MPCUA_FMT_F32: { float x = (float)((double)v / 2147483648.0); memcpy(b + 4 * i, &x, 4); break; }
  default: break;
  }
}

void mpcua_fmt_to_s32(const void *src, mpcua_fmt f, int32_t *dst, size_t n) {
  if (f == MPCUA_FMT_S32) { memcpy(dst, src, n * 4); return; }
  for (size_t i = 0; i < n; i++) dst[i] = mpcua_fmt_get(src, f, i);
}

void mpcua_fmt_from_s32(const int32_t *src, void *dst, mpcua_fmt f, size_t n) {
  if (f == MPCUA_FMT_S32) { memcpy(dst, src, n * 4); return; }
  for (size_t i = 0; i < n; i++) mpcua_fmt_put(dst, f, i, src[i]);
}

void mpcua_mix_add(void *dst, mpcua_fmt f, size_t i, int32_t v) {
  mpcua_fmt_put(dst, f, i, mpcua_sat_add(mpcua_fmt_get(dst, f, i), v));
}
