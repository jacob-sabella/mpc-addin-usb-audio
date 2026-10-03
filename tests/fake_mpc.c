/* fake_mpc.c: stands in for /usr/bin/MPC (the binary must be named MPC for the add-in to activate).
 * Drives the preloaded add-in through the same calls MPC makes and checks what it does.
 * Usage: MPC <configfs-root> [expect-inactive] */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t.h"

typedef struct { int card, stream; } fake_pcm;
typedef struct { int fmt; unsigned ch, rate; int access; } fake_hw;
typedef struct { char name[32]; int cfg; } fake_gadget;

int snd_pcm_open(fake_pcm **, const char *, int, int);
int snd_pcm_close(fake_pcm *);
int snd_pcm_hw_params(fake_pcm *, fake_hw *);
long snd_pcm_writei(fake_pcm *, const void *, unsigned long);
long snd_pcm_readi(fake_pcm *, void *, unsigned long);
long snd_pcm_readn(fake_pcm *, void **, unsigned long);
int usbg_enable_gadget(fake_gadget *, void *);
extern int fake_add_calls, fake_enable_calls, fake_rm_calls, fake_cap_s16;

static int (*t_active)(void);
static void (*t_set)(int, int);
static uint32_t (*t_pop)(int, int32_t *, uint32_t, unsigned);
static uint32_t (*t_push)(const int32_t *, uint32_t, unsigned);

static void slurp(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r"); out[0] = 0;
  if (f) { size_t r = fread(out, 1, n - 1, f); out[r] = 0; fclose(f); }
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const char *root = argv[1];
  *(void **)&t_active = dlsym(RTLD_DEFAULT, "mpcua_test_active");
  *(void **)&t_set = dlsym(RTLD_DEFAULT, "mpcua_test_set");
  *(void **)&t_pop = dlsym(RTLD_DEFAULT, "mpcua_test_pop");
  *(void **)&t_push = dlsym(RTLD_DEFAULT, "mpcua_test_push_host");
  CHECK(t_active && t_set && t_pop && t_push);
  if (t_fail) T_DONE("hooks: test helpers present");

  if (argc > 2) {  /* running under another name: must be a pure pass-through */
    CHECK_EQ(t_active(), 0);
    fake_gadget g = {"standalone", 0};
    CHECK_EQ(usbg_enable_gadget(&g, NULL), 0);
    CHECK_EQ(fake_add_calls, 0); CHECK_EQ(fake_enable_calls, 1);
    fake_pcm *p; fake_hw hw = {10, 2, 44100, 3};
    snd_pcm_open(&p, "hw:0,0", 0, 0); snd_pcm_hw_params(p, &hw);
    int32_t buf[8] = {0};
    CHECK_EQ(snd_pcm_writei(p, buf, 4), 4);
    snd_pcm_close(p);
    T_DONE("hooks: inert outside MPC");
  }
  CHECK_EQ(t_active(), 1);

  /* 1. gadget: our function only on the standalone gadget, attributes + IAD class written */
  char path[512], v[64];
  fake_gadget g = {"standalone", 0};
  CHECK_EQ(usbg_enable_gadget(&g, NULL), 0);
  CHECK_EQ(fake_add_calls, 1); CHECK_EQ(fake_enable_calls, 1); CHECK_EQ(fake_rm_calls, 0);
  snprintf(path, sizeof path, "%s/standalone/functions/uac2.usbaudio/p_chmask", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "0xf"));
  snprintf(path, sizeof path, "%s/standalone/functions/uac2.usbaudio/function_name", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "MPC USB Audio"));
  snprintf(path, sizeof path, "%s/standalone/bDeviceClass", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "0xEF"));
  fake_gadget other = {"smexstream", 0};
  CHECK_EQ(usbg_enable_gadget(&other, NULL), 0);
  CHECK_EQ(fake_add_calls, 1); CHECK_EQ(fake_enable_calls, 2);

  /* 2. playback tap: S32 stereo on the codec card (tap_card=0 in the test config) */
  fake_pcm *pp, *other_pcm;
  fake_hw hw = {10, 2, 44100, 3};
  snd_pcm_open(&pp, "hw:0,0", 0, 0);
  snd_pcm_open(&other_pcm, "hw:1,0", 0, 0);        /* another card: never tapped */
  snd_pcm_hw_params(pp, &hw);
  snd_pcm_hw_params(other_pcm, &hw);
  int32_t ramp[256], got[256];
  for (int i = 0; i < 256; i++) ramp[i] = i * 1000 - 7;
  CHECK_EQ(snd_pcm_writei(pp, ramp, 128), 128);
  CHECK_EQ(t_pop(0, got, 128, 2), 0);              /* forwarder not running: no tap */
  t_set(1, 0);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 128), 128);
  CHECK_EQ(snd_pcm_writei(other_pcm, ramp, 128), 128);
  CHECK_EQ(t_pop(0, got, 256, 2), 128);
  for (int i = 0; i < 256; i++) CHECK_EQ(got[i], ramp[i]);

  /* 3. capture: S16 stereo. Inputs are tapped before injection; computer audio is summed in. */
  fake_pcm *cp;
  fake_hw chw = {2, 2, 44100, 3};
  snd_pcm_open(&cp, "hw:0,0", 1, 0);
  snd_pcm_hw_params(cp, &chw);
  t_set(1, 1);
  int32_t host[128 * 2];
  for (int i = 0; i < 128; i++) { host[2 * i] = i << 16; host[2 * i + 1] = -(i << 16); }
  CHECK_EQ(t_push(host, 128, 2), 128);
  int16_t in[128 * 2];
  CHECK_EQ(snd_pcm_readi(cp, in, 128), 128);
  for (int i = 0; i < 128; i++) { CHECK_EQ(in[2 * i], 100 + i); CHECK_EQ(in[2 * i + 1], 100 - i); }
  CHECK_EQ(t_pop(1, got, 128, 2), 128);
  for (int i = 0; i < 256; i++) CHECK_EQ(got[i], 100 << 16);

  /* saturation in S16 */
  for (int i = 0; i < 4; i++) host[i] = 0x7fff0000;
  t_push(host, 2, 2);
  CHECK_EQ(snd_pcm_readi(cp, in, 2), 2);
  CHECK_EQ(in[0], 32767);

  /* non-interleaved readn path (MPC reconfigures the capture PCM for RW_NONINTERLEAVED) */
  chw.access = 4;
  snd_pcm_hw_params(cp, &chw);
  for (int i = 0; i < 8; i++) host[i] = 5 << 16;
  t_push(host, 4, 2);
  int16_t l[4], r[4]; void *bufs[2] = {l, r};
  CHECK_EQ(snd_pcm_readn(cp, bufs, 4), 4);
  CHECK_EQ(l[3], 105); CHECK_EQ(r[0], 105);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);            /* wrong access for this setup: untouched */
  chw.access = 3;
  snd_pcm_hw_params(cp, &chw);

  /* 4. computer audio not live: stale data is dropped, input untouched */
  t_push(host, 4, 2);
  t_set(1, 0);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);
  CHECK_EQ(in[0], 100);
  t_set(1, 1);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);
  CHECK_EQ(in[0], 100);                             /* ring was emptied */

  /* 5. rate mismatch: MPC reopens at 48 kHz, gadget is 44.1 kHz -> no tap */
  t_pop(0, got, 128, 2);
  hw.rate = 48000;
  snd_pcm_hw_params(pp, &hw);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 64), 64);
  CHECK_EQ(t_pop(0, got, 64, 2), 0);

  /* 6. close clears the tap */
  hw.rate = 44100;
  snd_pcm_hw_params(pp, &hw);
  snd_pcm_close(pp);
  snd_pcm_open(&pp, "hw:2,0", 0, 0);               /* new handle, other card */
  snd_pcm_hw_params(pp, &hw);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 64), 64);
  CHECK_EQ(t_pop(0, got, 64, 2), 0);
  snd_pcm_close(pp); snd_pcm_close(cp); snd_pcm_close(other_pcm);
  T_DONE("hooks: MPC process");
}
