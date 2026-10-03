/* addin.h: state shared between the hooks (addin.c) and the forwarder thread (fwd.c). */
#ifndef MPCUA_ADDIN_H
#define MPCUA_ADDIN_H

#include <stdatomic.h>
#include "alsa_api.h"
#include "config.h"
#include "fmt.h"
#include "ring.h"

/* One tapped codec stream (MPC's playback or capture PCM on the codec card). Fields other than
 * the atomics are written by the hw_params hook while `ok` is 0, then published with `ok` = 1. */
typedef struct {
  _Atomic(snd_pcm_t *) pcm;
  _Atomic int ok;
  _Atomic unsigned gen;     /* bumped on every hw_params/close, so the forwarder can resync */
  mpcua_fmt fmt;
  unsigned ch, rate;
  int interleaved;
  _Atomic unsigned long calls, frames;   /* MPC's reads/writes on this PCM, frames tapped (for the stats) */
  _Atomic uint32_t peak;                 /* largest |sample| tapped since the last stats line */
} mpcua_stream;

typedef struct {
  int active;               /* process is MPC and the addin is enabled */
  mpcua_cfg cfg;
  mpcua_alsa alsa;
  int alsa_ok;
  int tap_card;             /* codec card index, or -1 if unknown */

  mpcua_stream play, cap;
  mpcua_ring out_ring;      /* MPC main out  -> forwarder (audio thread produces) */
  mpcua_ring in_ring;       /* MPC inputs    -> forwarder (audio thread produces) */
  mpcua_ring host_ring;     /* computer      -> MPC inputs (forwarder produces) */
  int need_in;              /* to_host uses MPC inputs */

  _Atomic int fwd_running;  /* forwarder has the gadget PCMs open: audio thread may tap */
  _Atomic int host_live;    /* computer audio is flowing: audio thread may inject */

  _Atomic unsigned long ovf_out, ovf_in, ovf_host, und_host;
} mpcua_state;

extern mpcua_state g_ua;

/* Start the forwarder thread once (SCHED_OTHER, signals blocked, detached). */
void mpcua_fwd_start(void);

#endif
