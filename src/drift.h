/* drift.h: PI controller that keeps a sample queue at its target fill by steering a USB pitch.
 *
 * The gadget exposes "Playback Pitch 1000000" (how fast it sends to the computer) and
 * "Capture Pitch 1000000" (the rate it asks the computer to send at, via the feedback endpoint).
 * 1000000 means nominal and one unit is one ppm. The controller returns an offset in ppm; the caller
 * writes 1000000 + offset to the control.
 *
 * Direction (sign):
 *   +1  queue fed by the codec clock, drained by USB  (MPC -> computer): fill rising => pitch up
 *   -1  queue fed by USB, drained by the codec clock  (computer -> MPC): fill rising => pitch down
 *
 * Pure arithmetic: no I/O, no allocation. Tested offline in tests/test_drift.c.
 */
#ifndef MPCUA_DRIFT_H
#define MPCUA_DRIFT_H

typedef struct {
  double kp, ki;        /* ppm per frame, ppm per frame-second */
  double sign;
  double target;        /* frames; latched from measurement when configured as auto (< 0) */
  int auto_target;
  double ema, ema_tau;  /* smoothed fill, smoothing time constant (s) */
  double integ;         /* integral of error (frame-seconds) */
  double max_ppm;
  double settle_s, t;   /* time spent since reset; output is held at 0 until settle_s */
  int primed;
  double ppm;           /* last output */
} mpcua_drift;

/* rate: sample rate; target_frames < 0 picks the target automatically from the first settle period;
 * bw_hz: loop bandwidth (0.005..0.05 sensible; default 0.01 = ~16 s time constant). */
void mpcua_drift_init(mpcua_drift *d, unsigned rate, double target_frames, double max_ppm,
                      double bw_hz, int sign);
void mpcua_drift_reset(mpcua_drift *d);
/* Feed one measurement of queued frames taken dt_s after the previous one; returns ppm offset. */
int mpcua_drift_update(mpcua_drift *d, double fill_frames, double dt_s);

#endif
