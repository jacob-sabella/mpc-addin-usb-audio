/* test_fmt.c: format round trips, saturation, ALSA enum mapping. */
#include "../src/fmt.h"
#include "t.h"
#include <string.h>

int main(void) {
  CHECK_EQ(mpcua_fmt_from_alsa(10), MPCUA_FMT_S32);
  CHECK_EQ(mpcua_fmt_from_alsa(32), MPCUA_FMT_S24_3);
  CHECK_EQ(mpcua_fmt_from_alsa(999), MPCUA_FMT_UNKNOWN);
  CHECK_EQ(mpcua_fmt_to_alsa(MPCUA_FMT_S16), 2);
  CHECK_EQ(mpcua_fmt_from_ssize(3), MPCUA_FMT_S24_3);
  CHECK_EQ(mpcua_fmt_bytes(MPCUA_FMT_S24_3), 3);

  const int32_t v[] = {0, 1 << 8, -(1 << 8), 0x7FFFFF00, (int32_t)0x80000000, 0x12345600, -0x12345600};
  const int n = sizeof v / sizeof v[0];
  mpcua_fmt fs[] = {MPCUA_FMT_S24_3, MPCUA_FMT_S24_4, MPCUA_FMT_S32};
  for (unsigned k = 0; k < 3; k++) {          /* 24/32-bit: exact for 24-bit-precision input */
    unsigned char buf[64]; int32_t back[16];
    mpcua_fmt_from_s32(v, buf, fs[k], n);
    mpcua_fmt_to_s32(buf, fs[k], back, n);
    for (int i = 0; i < n; i++) CHECK_EQ(back[i], v[i]);
  }
  { /* S24_LE stores a sign-extended 24-bit value in 32 bits */
    unsigned char buf[4]; int32_t raw;
    mpcua_fmt_put(buf, MPCUA_FMT_S24_4, 0, -(1 << 8));
    memcpy(&raw, buf, 4);
    CHECK_EQ(raw, -1);
  }
  { /* S16 keeps the top 16 bits */
    unsigned char buf[4];
    mpcua_fmt_put(buf, MPCUA_FMT_S16, 0, 0x12345678);
    CHECK_EQ(mpcua_fmt_get(buf, MPCUA_FMT_S16, 0), 0x12340000);
    mpcua_fmt_put(buf, MPCUA_FMT_S16, 1, (int32_t)0x80000000);
    CHECK_EQ(mpcua_fmt_get(buf, MPCUA_FMT_S16, 1), (int32_t)0x80000000);
  }
  { /* float: clamps, NaN is silence, half scale round trip */
    float f[4] = {2.0f, -3.0f, 0.5f, 0.0f};
    f[3] = f[3] / f[3];
    CHECK_EQ(mpcua_fmt_get(f, MPCUA_FMT_F32, 0), INT32_MAX);
    CHECK_EQ(mpcua_fmt_get(f, MPCUA_FMT_F32, 1), INT32_MIN);
    CHECK_EQ(mpcua_fmt_get(f, MPCUA_FMT_F32, 2), 1 << 30);
    CHECK_EQ(mpcua_fmt_get(f, MPCUA_FMT_F32, 3), 0);
    float o; mpcua_fmt_put(&o, MPCUA_FMT_F32, 0, 1 << 30);
    CHECK(o == 0.5f);
  }
  { /* saturating mix in place */
    int32_t s[3] = {INT32_MAX - 5, INT32_MIN + 5, 100};
    mpcua_mix_add(s, MPCUA_FMT_S32, 0, 100);
    mpcua_mix_add(s, MPCUA_FMT_S32, 1, -100);
    mpcua_mix_add(s, MPCUA_FMT_S32, 2, -50);
    CHECK_EQ(s[0], INT32_MAX); CHECK_EQ(s[1], INT32_MIN); CHECK_EQ(s[2], 50);
    int16_t h[1] = {32000};
    mpcua_mix_add(h, MPCUA_FMT_S16, 0, 0x10000000);
    CHECK_EQ(h[0], 32767);
  }
  T_DONE("fmt");
}
