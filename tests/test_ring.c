/* test_ring.c: SPSC ring semantics plus a two-thread stress run (ASan/UBSan, optionally TSan). */
#include "../src/ring.h"
#include "t.h"
#include <pthread.h>

static int32_t store[1024];

static void basics(void) {
  mpcua_ring q;
  CHECK(mpcua_ring_init(&q, store, 1000) < 0);           /* not a power of two */
  CHECK(mpcua_ring_init(&q, store, 1024) == 0);
  CHECK_EQ(mpcua_ring_fill(&q), 0);
  CHECK_EQ(mpcua_ring_space(&q), 1024);

  int32_t a[600], b[600];
  for (int i = 0; i < 600; i++) a[i] = i;
  CHECK_EQ(mpcua_ring_write_frames(&q, a, 300, 2), 300);
  CHECK_EQ(mpcua_ring_fill(&q), 600);
  /* only 424 samples of space: 3-channel frames round down to 141 */
  CHECK_EQ(mpcua_ring_write_frames(&q, a, 200, 3), 141);
  CHECK_EQ(mpcua_ring_fill(&q), 1023);
  CHECK_EQ(mpcua_ring_read_frames(&q, b, 300, 2), 300);
  for (int i = 0; i < 600; i++) CHECK_EQ(b[i], i);
  /* wrap-around: write 500 more, they straddle the end */
  CHECK_EQ(mpcua_ring_read_frames(&q, b, 141, 3), 141);
  CHECK_EQ(mpcua_ring_fill(&q), 0);
  CHECK_EQ(mpcua_ring_write_frames(&q, a, 250, 2), 250);
  CHECK_EQ(mpcua_ring_read_frames(&q, b, 250, 2), 250);
  for (int i = 0; i < 500; i++) CHECK_EQ(b[i], i);

  /* trim keeps whole frames */
  CHECK_EQ(mpcua_ring_write_frames(&q, a, 100, 2), 100);
  mpcua_ring_trim(&q, 30, 2);
  CHECK_EQ(mpcua_ring_fill(&q), 60);
  CHECK_EQ(mpcua_ring_read_frames(&q, b, 30, 2), 30);
  CHECK_EQ(b[0], 140);
  CHECK_EQ(mpcua_ring_read_frames(&q, b, 1, 2), 0);   /* empty */
  CHECK_EQ(mpcua_ring_write_frames(&q, a, 1, 0), 0);  /* zero channels */
}

/* Producer writes a counter sequence in 2-channel frames of random sizes; the consumer checks
 * every sample arrives in order. Free-running indices are started near the uint32 wrap. */
#define TOTAL 3000000u
static mpcua_ring sq;
static int32_t sstore[256];

static void *producer(void *arg) {
  (void)arg;
  uint32_t next = 0, seed = 1;
  int32_t buf[64];
  while (next < TOTAL) {
    seed = seed * 1103515245u + 12345u;
    uint32_t frames = 1 + (seed >> 16) % 32;
    for (uint32_t i = 0; i < frames * 2; i++) buf[i] = (int32_t)(next + i);
    uint32_t w = mpcua_ring_write_frames(&sq, buf, frames, 2);
    next += w * 2;
  }
  return NULL;
}

static void stress(void) {
  mpcua_ring_init(&sq, sstore, 256);
  atomic_store(&sq.w, 0xFFFFFF00u);
  atomic_store(&sq.r, 0xFFFFFF00u);
  pthread_t th;
  pthread_create(&th, NULL, producer, NULL);
  uint32_t expect = 0, seed = 7;
  int32_t buf[64];
  int bad = 0;
  while (expect < TOTAL) {
    seed = seed * 1103515245u + 12345u;
    uint32_t frames = 1 + (seed >> 16) % 32;
    uint32_t r = mpcua_ring_read_frames(&sq, buf, frames, 2);
    for (uint32_t i = 0; i < r * 2; i++) if (buf[i] != (int32_t)expect++) bad++;
  }
  pthread_join(th, NULL);
  CHECK_EQ(bad, 0);
}

int main(void) {
  basics();
  stress();
  T_DONE("ring");
}
