/* config.c: see config.h. Runs only at load time (constructor), never on the audio thread. */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEF_TO_HOST "out1,out2,in1,in2"
#define DEF_TO_MPC "host1,host2"

void mpcua_cfg_defaults(mpcua_cfg *c) {
  memset(c, 0, sizeof *c);
  c->enabled = 1;
  snprintf(c->gadget, sizeof c->gadget, "standalone");
  snprintf(c->instance, sizeof c->instance, "usbaudio");
  snprintf(c->configfs, sizeof c->configfs, "/sys/kernel/config/usb_gadget");
  c->rate = 44100;
  c->sample_bytes = 4;
  snprintf(c->to_host, sizeof c->to_host, DEF_TO_HOST);
  c->host_channels = 2;
  snprintf(c->to_mpc_in, sizeof c->to_mpc_in, DEF_TO_MPC);
  c->input_mode = MPCUA_INPUT_SUM;
  c->hs_bint = 4;
  c->iad_class = 1;
  snprintf(c->function_name, sizeof c->function_name, "MPC USB Audio");
  c->tap_card = -1;
  c->target_frames = 256;
  c->fwd_period = 64;
  c->fwd_periods = 4;
  c->max_ppm = 1000;
  c->drift_bw = 0.01;
  snprintf(c->log_path, sizeof c->log_path, "/data/mpc-usb-audio/usbaudio.log");
  mpcua_cfg_finish(c, NULL, 0);
}

static int problem(char *err, size_t n, int count, const char *fmt, const char *a, int line) {
  if (count == 0 && err && n) snprintf(err, n, fmt, a, line);
  return count + 1;
}

static int to_long(const char *v, long lo, long hi, long *out) {
  char *e; errno = 0;
  long x = strtol(v, &e, 0);
  if (errno || e == v || *e || x < lo || x > hi) return -1;
  *out = x; return 0;
}

static int to_double(const char *v, double lo, double hi, double *out) {
  char *e; errno = 0;
  double x = strtod(v, &e);
  if (errno || e == v || *e || !(x >= lo && x <= hi)) return -1;
  *out = x; return 0;
}

static int set_str(char *dst, size_t n, const char *v) {
  if (strlen(v) >= n) return -1;
  memcpy(dst, v, strlen(v) + 1);
  return 0;
}

/* Gadget/instance names end up in configfs paths: keep them to a safe character set. */
static int set_name(char *dst, size_t n, const char *v) {
  if (!*v) return -1;
  for (const char *p = v; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return -1;
  return set_str(dst, n, v);
}

static int apply(mpcua_cfg *c, const char *k, const char *v) {
  long l; double d;
#define INT(key, field, lo, hi) if (!strcmp(k, key)) { if (to_long(v, lo, hi, &l)) return -1; c->field = l; return 0; }
#define DBL(key, field, lo, hi) if (!strcmp(k, key)) { if (to_double(v, lo, hi, &d)) return -1; c->field = d; return 0; }
  INT("enabled", enabled, 0, 1)
  INT("rate", rate, 8000, 192000)
  INT("host_channels", host_channels, 0, MPCUA_MAX_CH)
  INT("hs_bint", hs_bint, 1, 4)
  INT("req_number", req_number, 0, 32)
  INT("iad_class", iad_class, 0, 1)
  INT("target_frames", target_frames, -1, 4096)
  INT("fwd_period", fwd_period, 16, 1024)
  INT("fwd_periods", fwd_periods, 2, 16)
  INT("test_tone", test_tone, 0, 1)
  DBL("max_ppm", max_ppm, 1, 20000)
  DBL("drift_bw", drift_bw, 0.0005, 0.5)
#undef INT
#undef DBL
  if (!strcmp(k, "sample_bytes")) {
    if (to_long(v, 2, 4, &l)) return -1;
    c->sample_bytes = (unsigned)l; return 0;
  }
  if (!strcmp(k, "gadget")) return set_name(c->gadget, sizeof c->gadget, v);
  if (!strcmp(k, "instance")) return set_name(c->instance, sizeof c->instance, v);
  if (!strcmp(k, "configfs")) return *v == '/' ? set_str(c->configfs, sizeof c->configfs, v) : -1;
  if (!strcmp(k, "to_host")) return set_str(c->to_host, sizeof c->to_host, v);
  if (!strcmp(k, "to_mpc_in")) return set_str(c->to_mpc_in, sizeof c->to_mpc_in, v);
  if (!strcmp(k, "function_name")) return set_str(c->function_name, sizeof c->function_name, v);
  if (!strcmp(k, "log")) return set_str(c->log_path, sizeof c->log_path, v);
  if (!strcmp(k, "input_mode")) {
    if (!strcmp(v, "sum")) c->input_mode = MPCUA_INPUT_SUM;
    else if (!strcmp(v, "replace")) c->input_mode = MPCUA_INPUT_REPLACE;
    else if (!strcmp(v, "off")) c->input_mode = MPCUA_INPUT_OFF;
    else return -1;
    return 0;
  }
  if (!strcmp(k, "tap_card")) {
    if (!strcmp(v, "auto")) { c->tap_card = -1; return 0; }
    if (to_long(v, 0, 31, &l)) return -1;
    c->tap_card = (int)l; return 0;
  }
  return -2;
}

int mpcua_cfg_parse(mpcua_cfg *c, const char *text, char *err, size_t errlen) {
  int bad = 0, line = 0;
  const char *p = text;
  while (p && *p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char buf[256];
    line++;
    if (len >= sizeof buf) { bad = problem(err, errlen, bad, "line too long%s (line %d)", "", line); goto next; }
    memcpy(buf, p, len); buf[len] = 0;
    char *s = buf;
    char *hash = strchr(s, '#'); if (hash) *hash = 0;
    while (isspace((unsigned char)*s)) s++;
    for (char *e = s + strlen(s); e > s && isspace((unsigned char)e[-1]);) *--e = 0;
    if (!*s) goto next;
    char *eq = strchr(s, '=');
    if (!eq) { bad = problem(err, errlen, bad, "missing '=' in '%s' (line %d)", s, line); goto next; }
    *eq = 0;
    char *k = s, *v = eq + 1;
    for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
    while (isspace((unsigned char)*v)) v++;
    int r = apply(c, k, v);
    if (r == -2) bad = problem(err, errlen, bad, "unknown key '%s' (line %d)", k, line);
    else if (r < 0) bad = problem(err, errlen, bad, "bad value for '%s' (line %d)", k, line);
  next:
    p = nl ? nl + 1 : NULL;
  }
  if (mpcua_cfg_finish(c, bad ? NULL : err, errlen) < 0) bad++;
  return bad;
}

int mpcua_cfg_finish(mpcua_cfg *c, char *err, size_t errlen) {
  int rc = 0;
  int n = mpcua_chmap_parse(c->to_host, MPCUA_SRC_OUT | MPCUA_SRC_IN, c->to_host_map, MPCUA_MAX_CH);
  if (n < 1) {
    if (err && errlen) snprintf(err, errlen, "bad to_host '%s', using " DEF_TO_HOST, c->to_host);
    err = NULL; rc = -1;
    snprintf(c->to_host, sizeof c->to_host, DEF_TO_HOST);
    n = mpcua_chmap_parse(c->to_host, MPCUA_SRC_OUT | MPCUA_SRC_IN, c->to_host_map, MPCUA_MAX_CH);
  }
  c->n_to_host = n;
  n = mpcua_chmap_parse(c->to_mpc_in, MPCUA_SRC_HOST, c->to_mpc_map, MPCUA_MAX_CH);
  for (int i = 0; n > 0 && i < n; i++)
    if (c->to_mpc_map[i].kind == MPCUA_SRC_HOST && c->to_mpc_map[i].idx >= c->host_channels) n = -1;
  if (n < 1) {
    if (err && errlen) snprintf(err, errlen, "bad to_mpc_in '%s', using " DEF_TO_MPC, c->to_mpc_in);
    rc = -1;
    snprintf(c->to_mpc_in, sizeof c->to_mpc_in, DEF_TO_MPC);
    n = mpcua_chmap_parse(c->to_mpc_in, MPCUA_SRC_HOST, c->to_mpc_map, MPCUA_MAX_CH);
    if (c->host_channels < 2) c->input_mode = MPCUA_INPUT_OFF;
  }
  c->n_to_mpc = n;
  if (c->host_channels == 0) c->input_mode = MPCUA_INPUT_OFF;
  return rc;
}

int mpcua_cfg_load(mpcua_cfg *c, const char *path, char *err, size_t errlen) {
  mpcua_cfg_defaults(c);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? 0 : -1;
  char text[8192];
  ssize_t n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n < 0) return -1;
  text[n] = 0;
  return mpcua_cfg_parse(c, text, err, errlen);
}
