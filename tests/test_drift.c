/* test_drift.c: closed-loop simulation of both directions against a clock offset.
 *
 * MPC -> computer: the codec adds 128-frame periods at rate*(1+e); USB removes rate*(1+p)/1000
 * frames every 1 ms. Computer -> MPC: USB adds per 1 ms at rate*(1+p), the codec removes 128-frame
 * periods. The controller runs every 50 ms on the raw (sawtooth) fill, like the forwarder does.
 * Checks: the pitch converges to the offset, and the queue never strays far from its target.
 */
#include "../src/drift.h"
#include "t.h"
#include <math.h>

typedef struct { double ppm_end, max_dev, late_dev; } result;

static result sim(int sign, double e_ppm, double target, double seconds) {
  const double rate = 44100, step = 1e-4;
  mpcua_drift d;
  mpcua_drift_init(&d, (unsigned)rate, target, 1000, 0.01, sign);
  double fill = target > 0 ? target : 384, codec_acc = 0, usb_acc = 0, ctl_acc = 0;
  double p = 0, max_dev = 0, late_dev = 0, ppm_sum = 0; int ppm_n = 0;
  double tgt = target > 0 ? target : -1;
  for (double t = 0; t < seconds; t += step) {
    codec_acc += rate * (1 + e_ppm * 1e-6) * step;
    while (codec_acc >= 128) { codec_acc -= 128; fill += sign > 0 ? 128 : -128; }
    usb_acc += step;
    if (usb_acc >= 1e-3) {
      usb_acc -= 1e-3;
      double n = rate * (1 + p * 1e-6) / 1000.0;
      fill += sign > 0 ? -n : n;
    }
    ctl_acc += step;
    if (ctl_acc >= 0.05) {
      ctl_acc -= 0.05;
      p = mpcua_drift_update(&d, fill, 0.05);
      if (tgt < 0 && d.target > 0) tgt = d.target;
      if (tgt > 0) {
        double dev = fabs(d.ema - tgt);
        if (dev > max_dev) max_dev = dev;
        if (t > seconds * 0.6 && dev > late_dev) late_dev = dev;
      }
      if (t > seconds - 30) { ppm_sum += p; ppm_n++; }
    }
  }
  result r = {ppm_n ? ppm_sum / ppm_n : 0, max_dev, late_dev};
  return r;
}

int main(void) {
  const double offs[] = {300, -300, 80, -25, 0};
  for (int s = -1; s <= 1; s += 2) {
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) {
      result r = sim(s, offs[i], 256, 300);
      printf("     sign %+d offset %+5.0f ppm: pitch %+7.1f ppm, max dev %5.1f, late dev %4.1f frames\n",
             s, offs[i], r.ppm_end, r.max_dev, r.late_dev);
      CHECK(fabs(r.ppm_end - offs[i]) < 5);
      CHECK(r.max_dev < 250);      /* never more than one ring target away */
      CHECK(r.late_dev < 20);
    }
  }
  /* auto target: latches whatever fill it settles at */
  result r = sim(1, 120, -1, 300);
  CHECK(fabs(r.ppm_end - 120) < 5);
  CHECK(r.late_dev < 20);

  /* output is clamped and held at zero during settling */
  mpcua_drift d;
  mpcua_drift_init(&d, 44100, 100, 50, 0.01, 1);
  CHECK_EQ(mpcua_drift_update(&d, 100000, 0.05), 0);
  int p = 0;
  for (int i = 0; i < 100; i++) p = mpcua_drift_update(&d, 100000, 0.05);
  CHECK_EQ(p, 50);
  mpcua_drift_init(&d, 44100, 100, 50, 0.01, -1);
  for (int i = 0; i < 100; i++) p = mpcua_drift_update(&d, 100000, 0.05);
  CHECK_EQ(p, -50);
  T_DONE("drift");
}
