/* ring.h: single-producer / single-consumer lock-free ring of int32 samples.
 *
 * One thread writes, one thread reads, no locks, no allocation, no syscalls. Safe to use from MPC's
 * audio thread. Indices are free-running uint32 counters (wrap is harmless while capacity is a power
 * of two <= 2^30). Storage is supplied by the caller (static buffers in the addin).
 *
 * Frames are written and read whole: callers pass sample counts that are multiples of the channel
 * count, and the ring never splits a write.
 */
#ifndef MPCUA_RING_H
#define MPCUA_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  _Atomic uint32_t w;       /* producer index (samples written, free-running) */
  char pad0[60];
  _Atomic uint32_t r;       /* consumer index */
  char pad1[60];
  int32_t *buf;
  uint32_t cap;             /* power of two, in samples */
  uint32_t mask;
} mpcua_ring;

static inline int mpcua_ring_init(mpcua_ring *q, int32_t *storage, uint32_t cap) {
  if (!storage || cap < 2 || (cap & (cap - 1)) || cap > (1u << 30)) return -1;
  atomic_store_explicit(&q->w, 0, memory_order_relaxed);
  atomic_store_explicit(&q->r, 0, memory_order_relaxed);
  q->buf = storage; q->cap = cap; q->mask = cap - 1;
  return 0;
}

/* Samples readable (consumer view). */
static inline uint32_t mpcua_ring_fill(mpcua_ring *q) {
  uint32_t w = atomic_load_explicit(&q->w, memory_order_acquire);
  uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
  return w - r;
}

/* Samples writable (producer view). */
static inline uint32_t mpcua_ring_space(mpcua_ring *q) {
  uint32_t r = atomic_load_explicit(&q->r, memory_order_acquire);
  uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
  return q->cap - (w - r);
}

/* Producer: get up to two contiguous segments for n samples (n must be <= space). */
static inline void mpcua_ring_wseg(mpcua_ring *q, uint32_t n, int32_t **p1, uint32_t *n1,
                                   int32_t **p2, uint32_t *n2) {
  uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
  uint32_t off = w & q->mask, first = q->cap - off;
  if (first > n) first = n;
  *p1 = q->buf + off; *n1 = first;
  *p2 = q->buf;       *n2 = n - first;
}
static inline void mpcua_ring_wcommit(mpcua_ring *q, uint32_t n) {
  uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
  atomic_store_explicit(&q->w, w + n, memory_order_release);
}

/* Consumer: segments for n samples (n must be <= fill). */
static inline void mpcua_ring_rseg(mpcua_ring *q, uint32_t n, const int32_t **p1, uint32_t *n1,
                                   const int32_t **p2, uint32_t *n2) {
  uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
  uint32_t off = r & q->mask, first = q->cap - off;
  if (first > n) first = n;
  *p1 = q->buf + off; *n1 = first;
  *p2 = q->buf;       *n2 = n - first;
}
static inline void mpcua_ring_rcommit(mpcua_ring *q, uint32_t n) {
  uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
  atomic_store_explicit(&q->r, r + n, memory_order_release);
}

/* Producer: copy whole frames; returns frames written (may be fewer than asked when full). */
static inline uint32_t mpcua_ring_write_frames(mpcua_ring *q, const int32_t *src, uint32_t frames,
                                               uint32_t ch) {
  if (!ch) return 0;
  uint32_t fit = mpcua_ring_space(q) / ch;
  if (frames > fit) frames = fit;
  uint32_t n = frames * ch, n1, n2; int32_t *p1, *p2;
  if (!n) return 0;
  mpcua_ring_wseg(q, n, &p1, &n1, &p2, &n2);
  memcpy(p1, src, n1 * sizeof *src);
  if (n2) memcpy(p2, src + n1, n2 * sizeof *src);
  mpcua_ring_wcommit(q, n);
  return frames;
}

/* Consumer: copy whole frames; returns frames read. */
static inline uint32_t mpcua_ring_read_frames(mpcua_ring *q, int32_t *dst, uint32_t frames,
                                              uint32_t ch) {
  if (!ch) return 0;
  uint32_t have = mpcua_ring_fill(q) / ch;
  if (frames > have) frames = have;
  uint32_t n = frames * ch, n1, n2; const int32_t *p1, *p2;
  if (!n) return 0;
  mpcua_ring_rseg(q, n, &p1, &n1, &p2, &n2);
  memcpy(dst, p1, n1 * sizeof *dst);
  if (n2) memcpy(dst + n1, p2, n2 * sizeof *dst);
  mpcua_ring_rcommit(q, n);
  return frames;
}

/* Consumer: drop samples so that at most `keep` remain (rounded down to whole frames). */
static inline void mpcua_ring_trim(mpcua_ring *q, uint32_t keep_frames, uint32_t ch) {
  uint32_t fill = mpcua_ring_fill(q), keep = keep_frames * ch;
  if (fill > keep) {
    uint32_t drop = fill - keep;
    drop -= drop % (ch ? ch : 1);
    mpcua_ring_rcommit(q, drop);
  }
}

#endif
