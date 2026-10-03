/* fwd.c: the forwarder thread. Moves audio between the addin's rings and the UAC2 gadget's ALSA
 * card, and steers the gadget's pitch controls so both queues hold their target fill.
 *
 * Runs at SCHED_OTHER with every signal blocked. It never takes a lock the audio thread could
 * wait on; the audio thread never waits on it. If it falls behind, the USB side drops or repeats
 * audio; MPC's own outputs are never affected.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "addin.h"
#include "chmap.h"
#include "drift.h"
#include "log.h"

#define SCRATCH_FRAMES 512
#define HI_WATER_EXTRA 2048     /* frames above target before the to-computer queue is trimmed */
#define IDLE_RESET_S 0.2        /* no USB progress for this long: treat the stream as stopped */

static int32_t s_out[SCRATCH_FRAMES * MPCUA_MAX_CH];
static int32_t s_in[SCRATCH_FRAMES * MPCUA_MAX_CH];
static int32_t s_usb[SCRATCH_FRAMES * MPCUA_MAX_CH];
static unsigned char s_raw[SCRATCH_FRAMES * MPCUA_MAX_CH * 4];

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static int read_small(const char *path, char *buf, size_t n) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t r = read(fd, buf, n - 1);
  close(fd);
  if (r < 0) return -1;
  buf[r] = 0;
  return (int)r;
}

/* Our function exists in the standalone gadget, and that gadget is bound to a UDC. */
static int gadget_ready(void) {
  const mpcua_cfg *c = &g_ua.cfg;
  char p[320], udc[64];
  struct stat st;
  snprintf(p, sizeof p, "%s/%s/functions/uac2.%s", c->configfs, c->gadget, c->instance);
  if (stat(p, &st) != 0) return 0;
  snprintf(p, sizeof p, "%s/%s/UDC", c->configfs, c->gadget);
  if (read_small(p, udc, sizeof udc) <= 0) return 0;
  return udc[0] != '\n' && udc[0] != 0;
}

/* Index of the UAC2 gadget's ALSA card ("UAC2_Gadget" in /proc/asound/cards), or -1. */
static int find_gadget_card(void) {
  FILE *f = fopen("/proc/asound/cards", "re");
  if (!f) return -1;
  char line[256];
  int card = -1;
  while (fgets(line, sizeof line, f)) {
    if (!strstr(line, "UAC2_Gadget")) continue;
    char *e;
    long n = strtol(line, &e, 10);
    if (e != line) { card = (int)n; break; }
  }
  fclose(f);
  return card;
}

/* ---- pitch controls ----------------------------------------------------------------------- */

typedef struct {
  snd_ctl_elem_id_t *id;
  snd_ctl_elem_value_t *val;
  long min, max, last;
  int ok;
} pitch_ctl;

static void pitch_open(pitch_ctl *p, snd_ctl_t *ctl, const char *name) {
  const mpcua_alsa *a = &g_ua.alsa;
  memset(p, 0, sizeof *p);
  p->id = calloc(1, a->id_sizeof());
  snd_ctl_elem_info_t *info = calloc(1, a->einfo_sizeof());
  p->val = calloc(1, a->val_sizeof());
  if (!p->id || !info || !p->val) { free(info); return; }
  static const int ifaces[] = {MPCUA_CTL_IFACE_PCM, MPCUA_CTL_IFACE_MIXER};
  for (unsigned i = 0; i < 2 && !p->ok; i++) {
    memset(p->id, 0, a->id_sizeof());
    a->id_set_interface(p->id, ifaces[i]);
    a->id_set_name(p->id, name);
    a->einfo_set_id(info, p->id);
    if (a->elem_info(ctl, info) == 0) {
      p->min = a->einfo_get_min(info);
      p->max = a->einfo_get_max(info);
      p->ok = 1;
    }
  }
  free(info);
  if (p->ok) a->val_set_id(p->val, p->id);
  p->last = -1;
}

static void pitch_set(pitch_ctl *p, snd_ctl_t *ctl, long ppm) {
  if (!p->ok) return;
  long v = 1000000 + ppm;
  if (v < p->min) v = p->min;
  if (v > p->max) v = p->max;
  if (v == p->last) return;
  g_ua.alsa.val_set_integer(p->val, 0, v);
  if (g_ua.alsa.elem_write(ctl, p->val) >= 0) p->last = v;
}

static void pitch_close(pitch_ctl *p) { free(p->id); free(p->val); memset(p, 0, sizeof *p); }

/* ---- gadget PCM setup --------------------------------------------------------------------- */

static int pcm_setup(snd_pcm_t *pcm, mpcua_fmt f, unsigned ch, snd_pcm_uframes_t *buffer) {
  const mpcua_alsa *a = &g_ua.alsa;
  const mpcua_cfg *c = &g_ua.cfg;
  snd_pcm_hw_params_t *hw = calloc(1, a->hw_sizeof());
  snd_pcm_sw_params_t *sw = calloc(1, a->sw_sizeof());
  int r = -ENOMEM;
  if (!hw || !sw) goto out;
  snd_pcm_uframes_t period = c->fwd_period, buf = (snd_pcm_uframes_t)c->fwd_period * c->fwd_periods;
  int dir = 0;
  if ((r = a->hw_any(pcm, hw)) < 0 ||
      (r = a->hw_set_access(pcm, hw, MPCUA_ACCESS_RW_INTERLEAVED)) < 0 ||
      (r = a->hw_set_format(pcm, hw, mpcua_fmt_to_alsa(f))) < 0 ||
      (r = a->hw_set_channels(pcm, hw, ch)) < 0 ||
      (r = a->hw_set_rate(pcm, hw, c->rate, 0)) < 0 ||
      (r = a->hw_set_period_near(pcm, hw, &period, &dir)) < 0 ||
      (r = a->hw_set_buffer_near(pcm, hw, &buf)) < 0 ||
      (r = a->hw_params(pcm, hw)) < 0)
    goto out;
  a->hw_get_buffer_size(hw, buffer);
  if ((r = a->sw_current(pcm, sw)) < 0 ||
      (r = a->sw_set_start_threshold(pcm, sw, 0x7fffffff)) < 0 ||   /* started explicitly */
      (r = a->sw_set_avail_min(pcm, sw, period)) < 0 ||
      (r = a->sw_params(pcm, sw)) < 0)
    goto out;
  r = a->prepare(pcm);
out:
  free(hw);
  free(sw);
  return r;
}

/* ---- one session: gadget card open until it goes away or MPC reconfigures ----------------- */

typedef struct {
  snd_pcm_t *gp, *gc;          /* gadget playback (to computer) and capture (from computer) */
  snd_ctl_t *ctl;
  pitch_ctl pp, pc;
  mpcua_drift dp, dc;
  snd_pcm_uframes_t gp_buf, gc_buf;
  mpcua_fmt ufmt;
  unsigned play_gen, cap_gen;
  int primed, host_primed;
  double last_play_progress, last_cap_data, last_drift, last_stats, last_check;
  double tone_phase;
  unsigned long frames_to_host, frames_from_host;
} session;

static int recover(snd_pcm_t *pcm, int err) {
  if (err == -ENODEV || err == -EBADFD || err == -ESHUTDOWN) return -1;
  return g_ua.alsa.recover(pcm, err, 1) < 0 ? -1 : 0;
}

/* MPC -> computer. Returns -1 when the session must end. */
static int service_playback(session *S, double t) {
  const mpcua_alsa *a = &g_ua.alsa;
  const mpcua_cfg *c = &g_ua.cfg;
  if (!atomic_load(&g_ua.play.ok)) return 0;   /* MPC is reconfiguring; the gen check ends the session */
  unsigned oc = g_ua.play.ch;
  unsigned ic = (g_ua.need_in && atomic_load(&g_ua.cap.ok)) ? g_ua.cap.ch : 0;
  if (!oc) return 0;
  int target = c->target_frames > 0 ? c->target_frames : 256;

  snd_pcm_sframes_t avail = a->avail_update(S->gp);
  if (avail < 0) return recover(S->gp, (int)avail) < 0 ? -1 : (S->primed = 0, 0);
  if ((snd_pcm_uframes_t)avail < S->gp_buf) S->last_play_progress = t;

  uint32_t rf = mpcua_ring_fill(&g_ua.out_ring) / oc;
  if (ic) { uint32_t r2 = mpcua_ring_fill(&g_ua.in_ring) / ic; if (r2 < rf) rf = r2; }
  if (rf > (uint32_t)(target + HI_WATER_EXTRA)) {      /* computer not reading: keep the queue fresh */
    mpcua_ring_trim(&g_ua.out_ring, (uint32_t)target, oc);
    if (ic) mpcua_ring_trim(&g_ua.in_ring, (uint32_t)target, ic);
    rf = (uint32_t)target;
  }
  if (!S->primed) {
    if (rf < (uint32_t)target) return 0;
    S->primed = 1;
  }
  uint32_t k = (uint32_t)avail;
  if (k > rf) k = rf;
  if (k > SCRATCH_FRAMES) k = SCRATCH_FRAMES;
  if (!k) return 0;

  mpcua_ring_read_frames(&g_ua.out_ring, s_out, k, oc);
  if (ic) mpcua_ring_read_frames(&g_ua.in_ring, s_in, k, ic);
  int nh = c->n_to_host;
  for (uint32_t i = 0; i < k; i++)
    mpcua_chmap_frame(c->to_host_map, nh, s_out + (size_t)i * oc, oc, ic ? s_in + (size_t)i * ic : NULL,
                      ic, NULL, 0, s_usb + (size_t)i * nh);
  if (c->test_tone) {
    double inc = 2.0 * M_PI * 1000.0 / c->rate;
    for (uint32_t i = 0; i < k; i++) {
      int32_t v = (int32_t)(sin(S->tone_phase) * 0.25 * 2147483647.0);
      S->tone_phase += inc;
      if (S->tone_phase > 2.0 * M_PI) S->tone_phase -= 2.0 * M_PI;
      for (int ch = 0; ch < nh; ch++) s_usb[(size_t)i * nh + ch] = v;
    }
  }
  mpcua_fmt_from_s32(s_usb, s_raw, S->ufmt, (size_t)k * nh);
  snd_pcm_sframes_t w = a->writei(S->gp, s_raw, k);
  if (w < 0 && w != -EAGAIN) return recover(S->gp, (int)w) < 0 ? -1 : (S->primed = 0, 0);
  if (w > 0) S->frames_to_host += (unsigned long)w;
  if (a->state(S->gp) == MPCUA_STATE_PREPARED) a->start(S->gp);
  return 0;
}

/* computer -> MPC. */
static int service_capture(session *S, double t) {
  const mpcua_alsa *a = &g_ua.alsa;
  const mpcua_cfg *c = &g_ua.cfg;
  if (a->state(S->gc) == MPCUA_STATE_PREPARED) a->start(S->gc);
  snd_pcm_sframes_t avail = a->avail_update(S->gc);
  if (avail < 0) return recover(S->gc, (int)avail);
  if (avail > SCRATCH_FRAMES) avail = SCRATCH_FRAMES;
  if (avail <= 0) return 0;
  snd_pcm_sframes_t r = a->readi(S->gc, s_raw, (snd_pcm_uframes_t)avail);
  if (r < 0) return r == -EAGAIN ? 0 : recover(S->gc, (int)r);
  if (r == 0) return 0;
  S->last_cap_data = t;
  S->frames_from_host += (unsigned long)r;
  unsigned hc = c->host_channels;
  mpcua_fmt_to_s32(s_raw, S->ufmt, s_usb, (size_t)r * hc);
  if (!atomic_load(&g_ua.cap.ok) || c->input_mode == MPCUA_INPUT_OFF) return 0;
  unsigned mc = g_ua.cap.ch;   /* host ring frames are laid out like MPC's capture frames */
  if (!mc) return 0;
  for (snd_pcm_sframes_t i = 0; i < r; i++) {
    int32_t *dst = s_in + (size_t)i * mc;
    for (unsigned ch = 0; ch < mc; ch++) dst[ch] = 0;
    int n = c->n_to_mpc < (int)mc ? c->n_to_mpc : (int)mc;
    mpcua_chmap_frame(c->to_mpc_map, n, NULL, 0, NULL, 0, s_usb + (size_t)i * hc, hc, dst);
  }
  uint32_t w = mpcua_ring_write_frames(&g_ua.host_ring, s_in, (uint32_t)r, mc);
  if (w < (uint32_t)r) atomic_fetch_add(&g_ua.ovf_host, 1);
  int target = c->target_frames > 0 ? c->target_frames : 256;
  if (!S->host_primed && mpcua_ring_fill(&g_ua.host_ring) / mc >= (uint32_t)target) {
    S->host_primed = 1;
    atomic_store_explicit(&g_ua.host_live, 1, memory_order_release);
  }
  return 0;
}

static void service_drift(session *S, double t) {
  double dt = t - S->last_drift;
  if (dt < 0.05) return;
  S->last_drift = t;
  const mpcua_alsa *a = &g_ua.alsa;
  if (S->gp && atomic_load(&g_ua.play.ok) && g_ua.play.ch) {
    if (t - S->last_play_progress > IDLE_RESET_S || !S->primed) {
      mpcua_drift_reset(&S->dp);              /* computer is not recording: hold nominal */
      pitch_set(&S->pp, S->ctl, 0);
    } else {
      snd_pcm_sframes_t d = 0;
      if (a->delay(S->gp, &d) < 0) d = 0;
      double fill = (double)(mpcua_ring_fill(&g_ua.out_ring) / g_ua.play.ch) + (double)d;
      pitch_set(&S->pp, S->ctl, mpcua_drift_update(&S->dp, fill, dt));
    }
  }
  if (S->gc) {
    if (t - S->last_cap_data > IDLE_RESET_S) {
      if (S->host_primed) {
        S->host_primed = 0;
        atomic_store_explicit(&g_ua.host_live, 0, memory_order_release);
      }
      mpcua_drift_reset(&S->dc);
      pitch_set(&S->pc, S->ctl, 0);
    } else if (S->host_primed && atomic_load(&g_ua.cap.ok) && g_ua.cap.ch) {
      snd_pcm_sframes_t av = a->avail_update(S->gc);
      double fill = (double)(mpcua_ring_fill(&g_ua.host_ring) / g_ua.cap.ch) + (av > 0 ? (double)av : 0);
      pitch_set(&S->pc, S->ctl, mpcua_drift_update(&S->dc, fill, dt));
    }
  }
}

static double dbfs(uint32_t peak) { return peak ? 20.0 * log10((double)peak / 2147483648.0) : -999.0; }

static void stats(session *S, double t) {
  if (t - S->last_stats < 10.0) return;
  S->last_stats = t;
  mpcua_log("to computer %lu fr (pitch %ld), from computer %lu fr (pitch %ld, %s); "
            "overflows out/in/host %lu/%lu/%lu, input underruns %lu",
            S->frames_to_host, S->pp.ok ? S->pp.last : 0, S->frames_from_host,
            S->pc.ok ? S->pc.last : 0, atomic_load(&g_ua.host_live) ? "live" : "idle",
            atomic_load(&g_ua.ovf_out), atomic_load(&g_ua.ovf_in), atomic_load(&g_ua.ovf_host),
            atomic_load(&g_ua.und_host));
  mpcua_log("tapped: main out %lu calls %lu fr peak %.1f dBFS, inputs %lu calls %lu fr peak %.1f dBFS",
            atomic_load(&g_ua.play.calls), atomic_load(&g_ua.play.frames), dbfs(atomic_exchange(&g_ua.play.peak, 0)),
            atomic_load(&g_ua.cap.calls), atomic_load(&g_ua.cap.frames), dbfs(atomic_exchange(&g_ua.cap.peak, 0)));
}

static void run_session(int card) {
  const mpcua_alsa *a = &g_ua.alsa;
  const mpcua_cfg *c = &g_ua.cfg;
  session S;
  memset(&S, 0, sizeof S);
  char dev[32];
  snprintf(dev, sizeof dev, "hw:%d,0", card);
  S.ufmt = mpcua_fmt_from_ssize(c->sample_bytes);
  S.play_gen = atomic_load(&g_ua.play.gen);
  S.cap_gen = atomic_load(&g_ua.cap.gen);

  int r;
  if ((r = a->pcm_open(&S.gp, dev, MPCUA_PCM_PLAYBACK, MPCUA_PCM_NONBLOCK)) < 0 ||
      (r = pcm_setup(S.gp, S.ufmt, (unsigned)c->n_to_host, &S.gp_buf)) < 0) {
    mpcua_log("gadget playback %s: error %d", dev, r);
    goto done;
  }
  if (c->host_channels) {
    if ((r = a->pcm_open(&S.gc, dev, MPCUA_PCM_CAPTURE, MPCUA_PCM_NONBLOCK)) < 0 ||
        (r = pcm_setup(S.gc, S.ufmt, c->host_channels, &S.gc_buf)) < 0) {
      mpcua_log("gadget capture %s: error %d (continuing without it)", dev, r);
      if (S.gc) a->pcm_close(S.gc);
      S.gc = NULL;
    }
  }
  char cdev[16];
  snprintf(cdev, sizeof cdev, "hw:%d", card);
  if (a->ctl_open(&S.ctl, cdev, 0) == 0) {
    pitch_open(&S.pp, S.ctl, "Playback Pitch 1000000");
    pitch_open(&S.pc, S.ctl, "Capture Pitch 1000000");
  }
  mpcua_log("session on %s: buffers %lu/%lu, pitch controls %s/%s", dev, S.gp_buf, S.gc_buf,
            S.pp.ok ? "yes" : "NO", S.pc.ok ? "yes" : "NO");
  int target = c->target_frames;
  mpcua_drift_init(&S.dp, c->rate, target < 0 ? -1 : (double)target + (double)S.gp_buf, c->max_ppm,
                   c->drift_bw, +1);
  mpcua_drift_init(&S.dc, c->rate, target < 0 ? -1 : (double)target, c->max_ppm, c->drift_bw, -1);

  /* Fresh start for the producer-side queues, then let the audio thread tap. */
  mpcua_ring_trim(&g_ua.out_ring, 0, 1);
  mpcua_ring_trim(&g_ua.in_ring, 0, 1);
  atomic_store(&g_ua.fwd_running, 1);

  double t = now_s();
  S.last_drift = S.last_stats = S.last_check = t;
  struct pollfd pfd[8];
  for (;;) {
    int n = 0;
    int k1 = a->poll_count(S.gp);
    if (k1 > 0 && k1 <= 4) n += a->poll_desc(S.gp, pfd, (unsigned)k1);
    if (S.gc) {
      int k2 = a->poll_count(S.gc);
      if (k2 > 0 && k2 <= 4) n += a->poll_desc(S.gc, pfd + n, (unsigned)k2);
    }
    poll(pfd, (nfds_t)(n > 0 ? n : 0), 5);
    t = now_s();
    if (service_playback(&S, t) < 0) { mpcua_log("gadget playback gone"); break; }
    if (S.gc && service_capture(&S, t) < 0) { mpcua_log("gadget capture gone"); break; }
    service_drift(&S, t);
    stats(&S, t);
    if (t - S.last_check > 0.5) {
      S.last_check = t;
      if (atomic_load(&g_ua.play.gen) != S.play_gen || atomic_load(&g_ua.cap.gen) != S.cap_gen ||
          !atomic_load(&g_ua.play.ok)) {
        mpcua_log("MPC reconfigured its audio; restarting session");
        break;
      }
      if (!gadget_ready() || find_gadget_card() != card) { mpcua_log("gadget went away"); break; }
    }
  }
done:
  atomic_store(&g_ua.fwd_running, 0);
  atomic_store(&g_ua.host_live, 0);
  if (S.ctl) {
    pitch_set(&S.pp, S.ctl, 0);
    pitch_set(&S.pc, S.ctl, 0);
    a->ctl_close(S.ctl);
  }
  pitch_close(&S.pp);
  pitch_close(&S.pc);
  if (S.gp) a->pcm_close(S.gp);
  if (S.gc) a->pcm_close(S.gc);
}

static void *fwd_main(void *arg) {
  (void)arg;
  prctl(PR_SET_NAME, "usbaudio-fwd", 0, 0, 0);
  mpcua_log("forwarder started");
  int waiting_logged = 0;
  for (;;) {
    int card = gadget_ready() ? find_gadget_card() : -1;
    if (card < 0 || !atomic_load(&g_ua.play.ok)) {
      if (!waiting_logged) { mpcua_log("waiting for the gadget card and MPC's audio"); waiting_logged = 1; }
      sleep_ms(500);
      continue;
    }
    waiting_logged = 0;
    run_session(card);
    sleep_ms(500);
  }
  return NULL;
}

void mpcua_fwd_start(void) {
  static atomic_flag started = ATOMIC_FLAG_INIT;
  if (atomic_flag_test_and_set(&started)) return;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);   /* never inherit MPC's RT policy */
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  struct sched_param sp = {0};
  pthread_attr_setschedparam(&at, &sp);
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);   /* the new thread inherits "all blocked" */
  pthread_t th;
  int r = pthread_create(&th, &at, fwd_main, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  pthread_attr_destroy(&at);
  if (r) {
    mpcua_log("pthread_create: %s", strerror(r));
    atomic_flag_clear(&started);
  }
}
