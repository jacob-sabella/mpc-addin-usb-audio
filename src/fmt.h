/* fmt.h: PCM sample formats seen on MPC hardware, converted to and from left-justified int32.
 *
 * Internal sample format everywhere in the addin is int32 full scale (S32_LE semantics), which is
 * what MPC writes to the codec on current models. Other formats are handled so the addin does not
 * silently misbehave on hardware that opens the codec differently.
 */
#ifndef MPCUA_FMT_H
#define MPCUA_FMT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  MPCUA_FMT_UNKNOWN = 0,
  MPCUA_FMT_S16,     /* S16_LE */
  MPCUA_FMT_S24_3,   /* S24_3LE, packed 3 bytes */
  MPCUA_FMT_S24_4,   /* S24_LE, low 24 bits of a 32-bit word */
  MPCUA_FMT_S32,     /* S32_LE */
  MPCUA_FMT_F32,     /* FLOAT_LE, +-1.0 */
} mpcua_fmt;

/* ALSA snd_pcm_format_t values (stable ABI, alsa/pcm.h). */
enum {
  MPCUA_ALSA_S16_LE = 2, MPCUA_ALSA_S24_LE = 6, MPCUA_ALSA_S32_LE = 10,
  MPCUA_ALSA_FLOAT_LE = 14, MPCUA_ALSA_S24_3LE = 32,
};

mpcua_fmt mpcua_fmt_from_alsa(int alsa_fmt);
int mpcua_fmt_to_alsa(mpcua_fmt f);
unsigned mpcua_fmt_bytes(mpcua_fmt f);
/* Gadget sample size in bytes (2/3/4) to the format the gadget PCM is opened with. */
mpcua_fmt mpcua_fmt_from_ssize(unsigned bytes);

int32_t mpcua_fmt_get(const void *base, mpcua_fmt f, size_t idx);
void mpcua_fmt_put(void *base, mpcua_fmt f, size_t idx, int32_t v);

/* Contiguous buffer of n samples <-> int32. */
void mpcua_fmt_to_s32(const void *src, mpcua_fmt f, int32_t *dst, size_t n);
void mpcua_fmt_from_s32(const int32_t *src, void *dst, mpcua_fmt f, size_t n);

/* Sample dst_idx of dst = saturate(that sample + v), in dst's format. */
void mpcua_mix_add(void *dst, mpcua_fmt f, size_t dst_idx, int32_t v);

static inline int32_t mpcua_sat_add(int32_t a, int32_t b) {
  int64_t s = (int64_t)a + b;
  if (s > INT32_MAX) return INT32_MAX;
  if (s < INT32_MIN) return INT32_MIN;
  return (int32_t)s;
}

#endif
