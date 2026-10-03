/* drift.c: see drift.h.
 *
 * Plant: a pitch offset of p ppm changes the queue's slope by rate * 1e-6 * p frames/s. With
 * g = rate * 1e-6 and p = sign * (kp * e + ki * integral(e)), the error obeys
 *   e'' + g*kp*e' + g*ki*e = 0
 * so kp = 2*zeta*w/g and ki = w^2/g place the closed loop at natural frequency w = 2*pi*bw.
 */
#include "drift.h"

#include <math.h>

#define ZETA 0.9

void mpcua_drift_init(mpcua_drift *d, unsigned rate, double target, double max_ppm, double bw_hz,
                      int sign) {
  double g = (rate ? rate : 48000) * 1e-6;
  double w = 2.0 * M_PI * (bw_hz > 0 ? bw_hz : 0.01);
  d->kp = 2.0 * ZETA * w / g;
  d->ki = w * w / g;
  d->sign = sign < 0 ? -1.0 : 1.0;
  d->auto_target = target < 0;
  d->target = target < 0 ? 0 : target;
  d->ema_tau = 0.5;
  d->max_ppm = max_ppm > 0 ? max_ppm : 1000;
  d->settle_s = 1.0;
  mpcua_drift_reset(d);
}

void mpcua_drift_reset(mpcua_drift *d) {
  d->integ = 0; d->t = 0; d->primed = 0; d->ppm = 0; d->ema = 0;
  if (d->auto_target) d->target = 0;
}

int mpcua_drift_update(mpcua_drift *d, double fill, double dt) {
  if (dt <= 0 || dt > 5.0) dt = 0.05;   /* ignore bogus intervals (e.g. after a stall) */
  if (!d->primed) { d->ema = fill; d->primed = 1; }
  else d->ema += (fill - d->ema) * (1.0 - exp(-dt / d->ema_tau));
  d->t += dt;
  if (d->t < d->settle_s) return 0;
  if (d->auto_target && d->target == 0) d->target = d->ema > 1 ? d->ema : 1;

  double e = d->ema - d->target;
  double lim = d->max_ppm / (d->ki > 0 ? d->ki : 1);
  d->integ += e * dt;
  if (d->integ > lim) d->integ = lim;      /* anti-windup: the integral alone may not exceed max */
  if (d->integ < -lim) d->integ = -lim;
  double p = d->sign * (d->kp * e + d->ki * d->integ);
  if (p > d->max_ppm) p = d->max_ppm;
  if (p < -d->max_ppm) p = -d->max_ppm;
  d->ppm = p;
  return (int)lrint(p);
}
