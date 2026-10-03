/* fake_libs.c: just enough of libasound and libusbgx for the host integration test. The fake MPC
 * links against this; the add-in, preloaded in front, interposes and calls through with
 * dlsym(RTLD_NEXT). Only a subset of ALSA is provided, so the add-in's forwarder stays off and the
 * test drives the rings through the MPCUA_TEST_HOOKS helpers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int card, stream; } fake_pcm;
typedef struct { int fmt; unsigned ch, rate; int access; } fake_hw;
typedef struct { int card; } fake_info;

int fake_cap_s16 = 100;              /* what fake readi returns, per S16 sample */
int fake_writei_calls, fake_readi_calls;

int snd_pcm_open(fake_pcm **p, const char *name, int stream, int mode) {
  (void)mode;
  int card = 0;
  if (name && sscanf(name, "hw:%d", &card) != 1) card = -1;
  *p = calloc(1, sizeof **p);
  (*p)->card = card; (*p)->stream = stream;
  return 0;
}
int snd_pcm_close(fake_pcm *p) { free(p); return 0; }
int snd_pcm_hw_params(fake_pcm *p, fake_hw *h) { (void)p; (void)h; return 0; }
int snd_pcm_hw_params_get_format(const fake_hw *h, int *v) { *v = h->fmt; return 0; }
int snd_pcm_hw_params_get_channels(const fake_hw *h, unsigned *v) { *v = h->ch; return 0; }
int snd_pcm_hw_params_get_rate(const fake_hw *h, unsigned *v, int *d) { *v = h->rate; if (d) *d = 0; return 0; }
int snd_pcm_hw_params_get_access(const fake_hw *h, int *v) { *v = h->access; return 0; }
size_t snd_pcm_info_sizeof(void) { return sizeof(fake_info); }
int snd_pcm_info(fake_pcm *p, fake_info *i) { i->card = p->card; return 0; }
int snd_pcm_info_get_card(const fake_info *i) { return i->card; }
long snd_pcm_writei(fake_pcm *p, const void *b, unsigned long n) { (void)p; (void)b; fake_writei_calls++; return (long)n; }
long snd_pcm_writen(fake_pcm *p, void **b, unsigned long n) { (void)p; (void)b; return (long)n; }
long snd_pcm_readi(fake_pcm *p, void *b, unsigned long n) {
  (void)p; fake_readi_calls++;
  short *s = b;
  for (unsigned long i = 0; i < n * 2; i++) s[i] = (short)fake_cap_s16;
  return (long)n;
}
long snd_pcm_readn(fake_pcm *p, void **b, unsigned long n) {
  (void)p;
  for (int c = 0; c < 2; c++) for (unsigned long i = 0; i < n; i++) ((short *)b[c])[i] = (short)fake_cap_s16;
  return (long)n;
}

/* ---- libusbgx ---- */
typedef struct { char name[32]; int cfg; } fake_gadget;
typedef struct { char dir[256]; } fake_fn;
int fake_add_calls, fake_enable_calls, fake_rm_calls;
static fake_fn the_fn;

const char *usbg_get_gadget_name(fake_gadget *g) { return g->name; }
void *usbg_get_first_config(fake_gadget *g) { return &g->cfg; }
void *usbg_get_function(fake_gadget *g, int type, const char *inst) { (void)g; (void)type; (void)inst; return NULL; }
int usbg_lookup_function_type(const char *n) { return strcmp(n, "uac2") == 0 ? 14 : -1; }
int usbg_create_function(fake_gadget *g, int type, const char *inst, void *attrs, fake_fn **f) {
  (void)attrs;
  if (type != 14) return -1;
  const char *root = getenv("FAKE_CONFIGFS");
  snprintf(the_fn.dir, sizeof the_fn.dir, "%s/%s/functions/uac2.%s", root, g->name, inst);
  char cmd[400];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", the_fn.dir);
  if (system(cmd) != 0) return -1;
  static const char *attrs_k[] = {"p_chmask", "p_srate", "p_ssize", "c_chmask", "c_srate", "c_ssize",
                                  "c_sync", "p_hs_bint", "c_hs_bint", "function_name"};
  for (unsigned i = 0; i < sizeof attrs_k / sizeof *attrs_k; i++) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", the_fn.dir, attrs_k[i]);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); close(fd);
  }
  static const char *dev_k[] = {"bDeviceClass", "bDeviceSubClass", "bDeviceProtocol"};
  for (unsigned i = 0; i < 3; i++) {
    char p[400]; snprintf(p, sizeof p, "%s/%s/%s", root, g->name, dev_k[i]);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); close(fd);
  }
  *f = &the_fn;
  return 0;
}
int usbg_add_config_function(void *c, const char *name, fake_fn *f) { (void)c; (void)name; (void)f; fake_add_calls++; return 0; }
int usbg_rm_function(fake_fn *f, int opts) { (void)f; (void)opts; fake_rm_calls++; return 0; }
int usbg_enable_gadget(fake_gadget *g, void *udc) { (void)g; (void)udc; fake_enable_calls++; return 0; }
