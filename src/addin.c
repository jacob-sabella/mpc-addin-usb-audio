/* addin.c: LD_PRELOAD entry points.
 *
 * Inside MPC's process only (checked via /proc/self/exe), this:
 *   1. hooks usbg_enable_gadget: when MPC is about to bind its standalone gadget, a UAC2 function is
 *      created and linked with MPC's own libusbgx handles, so MPC's recursive teardown removes it;
 *   2. hooks snd_pcm_open/hw_params/close to find MPC's playback and capture PCMs on the codec card;
 *   3. hooks snd_pcm_writei/readi (and the non-interleaved variants) to copy main out and inputs into
 *      lock-free rings, and to mix computer playback into what MPC reads from its inputs.
 * The gadget's ALSA PCMs are serviced by the forwarder thread (fwd.c).
 *
 * Audio-thread rules (writei/readi/writen/readn): no locks, no allocation, no syscalls beyond the real
 * call, no logging. Everything else runs on MPC's setup threads or the forwarder.
 * In any other process every hook is a plain pass-through.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "addin.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gadget.h"
#include "log.h"

/* Only the hooks (and test helpers) are exported; everything else is built hidden. */
#define EXPORT __attribute__((visibility("default")))

mpcua_state g_ua;

#define RING_SAMPLES 65536u   /* per ring: 4096 frames of 16 ch, or 32768 frames of 2 ch */
static int32_t out_store[RING_SAMPLES], in_store[RING_SAMPLES], host_store[RING_SAMPLES];

/* ---- real functions ------------------------------------------------------------------------- */

static int (*real_open)(snd_pcm_t **, const char *, int, int);
static int (*real_close)(snd_pcm_t *);
static int (*real_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static snd_pcm_sframes_t (*real_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);

static void resolve_reals(void) {
  *(void **)&real_open = dlsym(RTLD_NEXT, "snd_pcm_open");
  *(void **)&real_close = dlsym(RTLD_NEXT, "snd_pcm_close");
  *(void **)&real_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
  *(void **)&real_writei = dlsym(RTLD_NEXT, "snd_pcm_writei");
  *(void **)&real_readi = dlsym(RTLD_NEXT, "snd_pcm_readi");
  *(void **)&real_writen = dlsym(RTLD_NEXT, "snd_pcm_writen");
  *(void **)&real_readn = dlsym(RTLD_NEXT, "snd_pcm_readn");
}

/* ---- activation ------------------------------------------------------------------------------ */

static int exe_is_mpc(void) {
  char p[256];
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
  if (n <= 0) return 0;
  p[n] = 0;
  const char *b = strrchr(p, '/');
  return strcmp(b ? b + 1 : p, "MPC") == 0;
}

/* A USB gadget's own ALSA card (its id is "UAC2Gadget", for example): never MPC's audio. */
static int is_gadget_card(int card) {
  char p[64], id[64] = "";
  snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
  FILE *f = fopen(p, "re");
  if (!f) return 0;
  if (!fgets(id, sizeof id, f)) id[0] = 0;
  fclose(f);
  return strstr(id, "Gadget") != NULL;
}

/* The folder this .so was loaded from (the installer puts its settings and log there), "" if unknown. */
static void addin_dir(char *out, size_t n) {
  out[0] = 0;
  FILE *f = fopen("/proc/self/maps", "re");
  if (!f) return;
  unsigned long me = (unsigned long)(uintptr_t)&addin_dir;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long a, b;
    char *path = strchr(line, '/');
    if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
    path[strcspn(path, "\n")] = 0;
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    if (strlen(path) < n) memcpy(out, path, strlen(path) + 1);   /* too long: unknown, never a truncated path */
    break;
  }
  fclose(f);
}

/* The codec card: same rule as MPC's own controller-mode forwarder (/dev/snd/by-path/platform-sound).
 * -1 when there is none (models whose audio is a USB device): snd_pcm_open then adopts the first card
 * MPC opens. */
static int resolve_tap_card(const mpcua_cfg *c) {
  if (c->tap_card >= 0) return c->tap_card;
  char t[64];
  ssize_t n = readlink("/dev/snd/by-path/platform-sound", t, sizeof t - 1);
  if (n <= 0) return -1;
  t[n] = 0;
  const char *s = strstr(t, "controlC");
  return s ? atoi(s + 8) : -1;
}

/* MPC builds its standalone gadget within a second or two of starting. If it never does, say so once:
 * the addin has nothing to add USB audio to (reported on an MPC Live II in standalone mode).
 * MPC_USB_AUDIO_GADGET_WAIT overrides the wait, in seconds (tests). */
static void *gadget_watch(void *arg) {
  (void)arg;
  prctl(PR_SET_NAME, "usbaudio-wait", 0, 0, 0);
  const char *w = getenv("MPC_USB_AUDIO_GADGET_WAIT");
  int secs = w && atoi(w) > 0 ? atoi(w) : 60;
  for (int i = 0; i < secs; i++) {
    if (atomic_load(&g_ua.gadget_seen)) return NULL;
    struct timespec ts = {1, 0};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
  }
  if (atomic_load(&g_ua.gadget_seen)) return NULL;
  char udc[64] = "none", state[64] = "?";
  DIR *d = opendir("/sys/class/udc");
  struct dirent *de;
  while (d && (de = readdir(d)))
    if (de->d_name[0] != '.') { snprintf(udc, sizeof udc, "%.63s", de->d_name); break; }
  if (d) closedir(d);
  if (strcmp(udc, "none")) {
    char p[160];
    snprintf(p, sizeof p, "/sys/class/udc/%s/state", udc);
    FILE *f = fopen(p, "re");
    if (f) { if (fgets(state, sizeof state, f)) state[strcspn(state, "\n")] = 0; fclose(f); }
  }
  mpcua_log("no USB audio: after %d s MPC has not enabled its gadget '%s' (UDC %s: %s). On some "
            "models MPC builds no USB gadget in standalone mode, so there is nothing to add USB audio to",
            secs, g_ua.cfg.gadget, udc, state);
  return NULL;
}

__attribute__((constructor)) static void mpcua_ctor(void) {
  resolve_reals();
  if (!exe_is_mpc()) return;

  char dir[128], defpath[160];   /* dir + "/usbaudio.log" fits log_path */
  addin_dir(dir, sizeof dir);
  snprintf(defpath, sizeof defpath, "%s/%s", dir[0] ? dir : ".", MPCUA_CONF_NAME);
  const char *path = getenv("MPC_USB_AUDIO_CONF");
  if (!path || !*path) path = defpath;
  char err[160] = "";
  int bad = mpcua_cfg_load(&g_ua.cfg, path, err, sizeof err);
  if (!strcmp(g_ua.cfg.log_path, "auto"))
    snprintf(g_ua.cfg.log_path, sizeof g_ua.cfg.log_path, "%s/usbaudio.log", dir[0] ? dir : ".");
  mpcua_log_open(g_ua.cfg.log_path);
  if (bad < 0) mpcua_log("config %s unreadable, using defaults", path);
  else if (bad) mpcua_log("config %s: %d problem(s), first: %s", path, bad, err);
  else mpcua_log("config %s", path);
  if (!g_ua.cfg.enabled) { mpcua_log("disabled in config"); return; }

  int missing = mpcua_alsa_resolve(&g_ua.alsa);
  g_ua.alsa_ok = missing == 0;
  if (missing) mpcua_log("%d libasound symbols missing; forwarder disabled", missing);

  mpcua_ring_init(&g_ua.out_ring, out_store, RING_SAMPLES);
  mpcua_ring_init(&g_ua.in_ring, in_store, RING_SAMPLES);
  mpcua_ring_init(&g_ua.host_ring, host_store, RING_SAMPLES);
  for (int i = 0; i < g_ua.cfg.n_to_host; i++)
    if (g_ua.cfg.to_host_map[i].kind == MPCUA_SRC_IN) g_ua.need_in = 1;
  atomic_store(&g_ua.tap_card, resolve_tap_card(&g_ua.cfg));
  g_ua.active = 1;
  int tap = atomic_load(&g_ua.tap_card);
  if (tap >= 0)
    mpcua_log("active: gadget '%s', %d ch to computer, %u from it, %u Hz, codec card %d",
              g_ua.cfg.gadget, g_ua.cfg.n_to_host, g_ua.cfg.host_channels, g_ua.cfg.rate, tap);
  else
    mpcua_log("active: gadget '%s', %d ch to computer, %u from it, %u Hz, no platform codec: "
              "the first card MPC opens", g_ua.cfg.gadget, g_ua.cfg.n_to_host, g_ua.cfg.host_channels,
              g_ua.cfg.rate);
  int e = mpcua_spawn(gadget_watch);
  if (e) mpcua_log("gadget watch thread: %s", strerror(e));
}

/* ---- gadget hook (MPC setup thread) --------------------------------------------------------- */

typedef struct usbg_gadget usbg_gadget;
typedef struct usbg_udc usbg_udc;
typedef struct usbg_config usbg_config;
typedef struct usbg_function usbg_function;

static int (*real_enable_gadget)(usbg_gadget *, usbg_udc *);

static void add_uac2(usbg_gadget *g) {
  const char *(*get_name)(usbg_gadget *) = dlsym(RTLD_NEXT, "usbg_get_gadget_name");
  usbg_config *(*first_config)(usbg_gadget *) = dlsym(RTLD_NEXT, "usbg_get_first_config");
  usbg_function *(*get_function)(usbg_gadget *, int, const char *) = dlsym(RTLD_NEXT, "usbg_get_function");
  int (*create_function)(usbg_gadget *, int, const char *, void *, usbg_function **) =
      dlsym(RTLD_NEXT, "usbg_create_function");
  int (*add_config_function)(usbg_config *, const char *, usbg_function *) =
      dlsym(RTLD_NEXT, "usbg_add_config_function");
  int (*rm_function)(usbg_function *, int) = dlsym(RTLD_NEXT, "usbg_rm_function");
  int (*lookup_type)(const char *) = dlsym(RTLD_NEXT, "usbg_lookup_function_type");
  if (!get_name || !first_config || !get_function || !create_function || !add_config_function ||
      !rm_function || !lookup_type) {
    mpcua_log("libusbgx symbols missing; not adding USB audio");
    return;
  }
  const char *name = get_name(g);
  if (!name || strcmp(name, g_ua.cfg.gadget) != 0) {
    mpcua_log("gadget '%s' enabled: not ours, left alone", name ? name : "?");
    return;
  }
  int type = lookup_type("uac2");
  usbg_config *c = first_config(g);
  if (type < 0 || !c) { mpcua_log("no uac2 type (%d) or no config", type); return; }

  usbg_function *f = get_function(g, type, g_ua.cfg.instance);
  if (f) {
    mpcua_log("uac2.%s already present", g_ua.cfg.instance);
  } else {
    int r = create_function(g, type, g_ua.cfg.instance, NULL, &f);
    if (r != 0 || !f) { mpcua_log("usbg_create_function(uac2) failed: %d", r); return; }
    char fdir[320], err[160] = "";
    snprintf(fdir, sizeof fdir, "%s/%s/functions/uac2.%s", g_ua.cfg.configfs, name, g_ua.cfg.instance);
    if (mpcua_uac2_write_attrs(fdir, &g_ua.cfg, err, sizeof err) < 0) {
      mpcua_log("uac2 attributes: %s; removing function", err);
      rm_function(f, 1 /* USBG_RM_RECURSE */);
      return;
    }
    if (err[0]) mpcua_log("uac2 optional attribute: %s", err);
    r = add_config_function(c, NULL, f);
    if (r != 0) {
      mpcua_log("usbg_add_config_function(uac2) failed: %d; removing function", r);
      rm_function(f, 1);
      return;
    }
    if (g_ua.cfg.iad_class) {
      char gdir[256];
      snprintf(gdir, sizeof gdir, "%s/%s", g_ua.cfg.configfs, name);
      int e = mpcua_gadget_set_iad_class(gdir);
      if (e) mpcua_log("device class EF/02/01 not set: %s", strerror(-e));
    }
    mpcua_log("uac2.%s added to gadget '%s'", g_ua.cfg.instance, name);
  }
  atomic_store(&g_ua.gadget_seen, 1);
  if (g_ua.alsa_ok) mpcua_fwd_start();
}

EXPORT int usbg_enable_gadget(usbg_gadget *g, usbg_udc *udc) {
  if (!real_enable_gadget) *(void **)&real_enable_gadget = dlsym(RTLD_NEXT, "usbg_enable_gadget");
  if (g_ua.active && g) add_uac2(g);
  return real_enable_gadget ? real_enable_gadget(g, udc) : -1;
}

/* ---- PCM tracking (MPC setup threads) ------------------------------------------------------- */

static mpcua_stream *stream_of(snd_pcm_t *pcm) {
  if (!pcm) return NULL;
  if (atomic_load(&g_ua.play.pcm) == pcm) return &g_ua.play;
  if (atomic_load(&g_ua.cap.pcm) == pcm) return &g_ua.cap;
  return NULL;
}

EXPORT int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode) {
  if (!real_open) resolve_reals();
  if (!real_open) return -ENOSYS;
  int r = real_open(pcmp, name, stream, mode);
  if (r < 0 || !g_ua.active || !g_ua.alsa.info_sizeof || !g_ua.alsa.info || !g_ua.alsa.info_get_card)
    return r;
  snd_pcm_info_t *info = alloca(g_ua.alsa.info_sizeof());
  memset(info, 0, g_ua.alsa.info_sizeof());
  if (g_ua.alsa.info(*pcmp, info) < 0) return r;
  int card = g_ua.alsa.info_get_card(info);
  int tap = atomic_load(&g_ua.tap_card);
  if (tap < 0) {   /* tap_card=auto and no platform codec: MPC's first card is its audio */
    if (card < 0 || is_gadget_card(card)) return r;
    if (atomic_compare_exchange_strong(&g_ua.tap_card, &tap, card))
      mpcua_log("codec card %d: the first card MPC opened ('%s')", card, name ? name : "?");
    tap = atomic_load(&g_ua.tap_card);
  }
  if (card != tap) return r;
  mpcua_stream *s = stream == MPCUA_PCM_PLAYBACK ? &g_ua.play : &g_ua.cap;
  atomic_store(&s->ok, 0);
  atomic_store(&s->pcm, *pcmp);
  atomic_fetch_add(&s->gen, 1);
  mpcua_log("tapping %s PCM '%s' (card %d)", stream == MPCUA_PCM_PLAYBACK ? "playback" : "capture",
            name ? name : "?", card);
  return r;
}

EXPORT int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
  if (!real_hw_params) resolve_reals();
  if (!real_hw_params) return -ENOSYS;
  mpcua_stream *s = g_ua.active ? stream_of(pcm) : NULL;
  if (s) atomic_store(&s->ok, 0);
  int r = real_hw_params(pcm, params);
  if (!s || r < 0) return r;
  const mpcua_alsa *a = &g_ua.alsa;
  int fmt = -1, access = -1, dir = 0;
  unsigned ch = 0, rate = 0;
  if (!a->hw_get_format || !a->hw_get_channels || !a->hw_get_rate || !a->hw_get_access) return r;
  a->hw_get_format(params, &fmt);
  a->hw_get_channels(params, &ch);
  a->hw_get_rate(params, &rate, &dir);
  a->hw_get_access(params, &access);
  s->fmt = mpcua_fmt_from_alsa(fmt);
  s->ch = ch;
  s->rate = rate;
  s->interleaved = access == MPCUA_ACCESS_RW_INTERLEAVED;
  atomic_fetch_add(&s->gen, 1);
  const char *what = s == &g_ua.play ? "playback" : "capture";
  if (s->fmt == MPCUA_FMT_UNKNOWN || ch < 1 || ch > MPCUA_MAX_CH ||
      (access != MPCUA_ACCESS_RW_INTERLEAVED && access != MPCUA_ACCESS_RW_NONINTERLEAVED)) {
    mpcua_log("%s: unsupported format %d / %u ch / access %d; not tapping", what, fmt, ch, access);
    return r;
  }
  if (rate != g_ua.cfg.rate) {
    mpcua_log("%s: MPC runs at %u Hz but the gadget is set to %u Hz; not tapping", what, rate,
              g_ua.cfg.rate);
    return r;
  }
  atomic_store_explicit(&s->ok, 1, memory_order_release);
  mpcua_log("%s: %u ch, format %d, %u Hz, %s", what, ch, fmt, rate,
            s->interleaved ? "interleaved" : "non-interleaved");
  return r;
}

EXPORT int snd_pcm_close(snd_pcm_t *pcm) {
  if (!real_close) resolve_reals();
  if (!real_close) return -ENOSYS;
  mpcua_stream *s = g_ua.active ? stream_of(pcm) : NULL;
  if (s) {
    atomic_store(&s->ok, 0);
    atomic_store(&s->pcm, NULL);
    atomic_fetch_add(&s->gen, 1);
  }
  return real_close(pcm);
}

/* ---- audio thread ---------------------------------------------------------------------------- */

static inline int tapping(mpcua_stream *s) {
  return atomic_load_explicit(&s->ok, memory_order_acquire) &&
         atomic_load_explicit(&g_ua.fwd_running, memory_order_relaxed);
}

/* Statistics for the 10 s log line: the stream's largest |sample| in what was just tapped. */
static void note_peak(mpcua_stream *s, const int32_t *p, uint32_t n) {
  uint32_t m = atomic_load_explicit(&s->peak, memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t a = p[i] < 0 ? (uint32_t)0 - (uint32_t)p[i] : (uint32_t)p[i];
    if (a > m) m = a;
  }
  atomic_store_explicit(&s->peak, m, memory_order_relaxed);
}

/* Interleaved buffer -> ring as int32. */
static void tap_i(mpcua_stream *s, mpcua_ring *q, const void *buf, uint32_t frames,
                  _Atomic unsigned long *ovf) {
  unsigned ch = s->ch;
  uint32_t fit = mpcua_ring_space(q) / ch;
  if (frames > fit) { frames = fit; atomic_fetch_add_explicit(ovf, 1, memory_order_relaxed); }
  if (!frames) return;
  int32_t *p1, *p2; uint32_t n1, n2;
  mpcua_ring_wseg(q, frames * ch, &p1, &n1, &p2, &n2);
  mpcua_fmt_to_s32(buf, s->fmt, p1, n1);
  if (n2) mpcua_fmt_to_s32((const char *)buf + (size_t)n1 * mpcua_fmt_bytes(s->fmt), s->fmt, p2, n2);
  note_peak(s, p1, n1); note_peak(s, p2, n2);
  atomic_fetch_add_explicit(&s->frames, frames, memory_order_relaxed);
  mpcua_ring_wcommit(q, frames * ch);
}

/* Non-interleaved buffers -> ring as interleaved int32. */
static void tap_n(mpcua_stream *s, mpcua_ring *q, void **bufs, uint32_t frames,
                  _Atomic unsigned long *ovf) {
  unsigned ch = s->ch;
  uint32_t fit = mpcua_ring_space(q) / ch;
  if (frames > fit) { frames = fit; atomic_fetch_add_explicit(ovf, 1, memory_order_relaxed); }
  if (!frames) return;
  int32_t *p[2]; uint32_t n[2];
  mpcua_ring_wseg(q, frames * ch, &p[0], &n[0], &p[1], &n[1]);
  uint32_t j = 0;
  for (int seg = 0; seg < 2; seg++)
    for (uint32_t k = 0; k < n[seg]; k++, j++)
      p[seg][k] = bufs[j % ch] ? mpcua_fmt_get(bufs[j % ch], s->fmt, j / ch) : 0;
  note_peak(s, p[0], n[0]); note_peak(s, p[1], n[1]);
  atomic_fetch_add_explicit(&s->frames, frames, memory_order_relaxed);
  mpcua_ring_wcommit(q, frames * ch);
}

/* Mix (or replace) computer audio into what MPC just read from its inputs. `bufs` is NULL for an
 * interleaved `buf`. */
static void inject(void *buf, void **bufs, uint32_t frames) {
  mpcua_stream *s = &g_ua.cap;
  int mode = g_ua.cfg.input_mode;
  if (mode == MPCUA_INPUT_OFF || !atomic_load_explicit(&s->ok, memory_order_acquire)) return;
  unsigned ch = s->ch;
  if (!atomic_load_explicit(&g_ua.host_live, memory_order_acquire)) {
    mpcua_ring_trim(&g_ua.host_ring, 0, ch);   /* consumer side: drop anything stale */
    return;
  }
  uint32_t have = mpcua_ring_fill(&g_ua.host_ring) / ch;
  uint32_t k = frames < have ? frames : have;
  if (k < frames) atomic_fetch_add_explicit(&g_ua.und_host, 1, memory_order_relaxed);
  if (!k) return;
  const int32_t *p[2]; uint32_t n[2];
  mpcua_ring_rseg(&g_ua.host_ring, k * ch, &p[0], &n[0], &p[1], &n[1]);
  uint32_t j = 0;
  for (int seg = 0; seg < 2; seg++)
    for (uint32_t i = 0; i < n[seg]; i++, j++) {
      void *base = buf; size_t idx = j;
      if (bufs) { base = bufs[j % ch]; idx = j / ch; if (!base) continue; }
      if (mode == MPCUA_INPUT_SUM) mpcua_mix_add(base, s->fmt, idx, p[seg][i]);
      else mpcua_fmt_put(base, s->fmt, idx, p[seg][i]);
    }
  mpcua_ring_rcommit(&g_ua.host_ring, k * ch);
}

EXPORT snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n) {
  if (!real_writei) return -ENOSYS;
  snd_pcm_sframes_t r = real_writei(pcm, buf, n);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed))
    atomic_fetch_add_explicit(&g_ua.play.calls, 1, memory_order_relaxed);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed) &&
      tapping(&g_ua.play) && g_ua.play.interleaved)
    tap_i(&g_ua.play, &g_ua.out_ring, buf, (uint32_t)r, &g_ua.ovf_out);
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
  if (!real_writen) return -ENOSYS;
  snd_pcm_sframes_t r = real_writen(pcm, bufs, n);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed))
    atomic_fetch_add_explicit(&g_ua.play.calls, 1, memory_order_relaxed);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed) &&
      tapping(&g_ua.play) && !g_ua.play.interleaved && bufs)
    tap_n(&g_ua.play, &g_ua.out_ring, bufs, (uint32_t)r, &g_ua.ovf_out);
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n) {
  if (!real_readi) return -ENOSYS;
  snd_pcm_sframes_t r = real_readi(pcm, buf, n);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed))
    atomic_fetch_add_explicit(&g_ua.cap.calls, 1, memory_order_relaxed);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed) &&
      g_ua.cap.interleaved) {
    if (g_ua.need_in && tapping(&g_ua.cap)) tap_i(&g_ua.cap, &g_ua.in_ring, buf, (uint32_t)r, &g_ua.ovf_in);
    inject(buf, NULL, (uint32_t)r);
  }
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
  if (!real_readn) return -ENOSYS;
  snd_pcm_sframes_t r = real_readn(pcm, bufs, n);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed))
    atomic_fetch_add_explicit(&g_ua.cap.calls, 1, memory_order_relaxed);
  if (r > 0 && bufs && pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed) &&
      !g_ua.cap.interleaved) {
    if (g_ua.need_in && tapping(&g_ua.cap)) tap_n(&g_ua.cap, &g_ua.in_ring, bufs, (uint32_t)r, &g_ua.ovf_in);
    inject(NULL, bufs, (uint32_t)r);
  }
  return r;
}

/* ---- test-only introspection (x86 host test build) ------------------------------------------ */
#ifdef MPCUA_TEST_HOOKS
EXPORT int mpcua_test_active(void) { return g_ua.active; }
EXPORT void mpcua_test_set(int fwd_running, int host_live) {
  atomic_store(&g_ua.fwd_running, fwd_running);
  atomic_store(&g_ua.host_live, host_live);
}
EXPORT uint32_t mpcua_test_pop(int which, int32_t *dst, uint32_t frames, unsigned ch) {
  return mpcua_ring_read_frames(which ? &g_ua.in_ring : &g_ua.out_ring, dst, frames, ch);
}
EXPORT uint32_t mpcua_test_push_host(const int32_t *src, uint32_t frames, unsigned ch) {
  return mpcua_ring_write_frames(&g_ua.host_ring, src, frames, ch);
}
#endif
