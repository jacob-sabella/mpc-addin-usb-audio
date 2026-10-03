/* gadget.c: see gadget.h. Runs on MPC's gadget-setup thread, never on the audio thread. */
#include "gadget.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int mpcua_write_attr(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (fd < 0) return -errno;
  size_t len = strlen(value);
  ssize_t w = write(fd, value, len);
  int e = w < 0 ? errno : 0;
  close(fd);
  if (w < 0) return -e;
  return (size_t)w == len ? 0 : -EIO;
}

/* Numeric attributes are formatted as hex when `hex`, else decimal; string ones are copied. */
static void add(mpcua_attr *out, int max, int *n, const char *name, int req, int hex, unsigned v,
                const char *s) {
  if (*n >= max) return;
  mpcua_attr *a = &out[(*n)++];
  a->name = name; a->required = req;
  if (s) snprintf(a->value, sizeof a->value, "%s", s);
  else snprintf(a->value, sizeof a->value, hex ? "0x%x" : "%u", v);
}

int mpcua_uac2_attrs(const mpcua_cfg *c, mpcua_attr *out, int max) {
  int n = 0;
  unsigned p_mask = c->n_to_host >= 32 ? 0xffffffffu : (1u << c->n_to_host) - 1;
  unsigned c_mask = c->host_channels >= 32 ? 0xffffffffu : (1u << c->host_channels) - 1;
  /* f_uac2 naming is from the gadget's view: p_* = playback = data sent TO the computer. */
  add(out, max, &n, "p_chmask", 1, 1, p_mask, NULL);
  add(out, max, &n, "p_srate", 1, 0, c->rate, NULL);
  add(out, max, &n, "p_ssize", 1, 0, c->sample_bytes, NULL);
  add(out, max, &n, "c_chmask", 1, 1, c_mask, NULL);
  add(out, max, &n, "c_srate", 1, 0, c->rate, NULL);
  add(out, max, &n, "c_ssize", 1, 0, c->sample_bytes, NULL);
  if (c->host_channels) add(out, max, &n, "c_sync", 0, 0, 0, "async");
  add(out, max, &n, "p_hs_bint", 0, 0, c->hs_bint, NULL);
  add(out, max, &n, "c_hs_bint", 0, 0, c->hs_bint, NULL);
  if (c->req_number) add(out, max, &n, "req_number", 0, 0, c->req_number, NULL);
  /* No feature units: nothing would apply host-side volume/mute, so don't offer dead controls. */
  add(out, max, &n, "p_mute_present", 0, 0, 0, NULL);
  add(out, max, &n, "p_volume_present", 0, 0, 0, NULL);
  add(out, max, &n, "c_mute_present", 0, 0, 0, NULL);
  add(out, max, &n, "c_volume_present", 0, 0, 0, NULL);
  if (c->function_name[0]) add(out, max, &n, "function_name", 0, 0, 0, c->function_name);
  return n;
}

int mpcua_uac2_write_attrs(const char *fdir, const mpcua_cfg *c, char *err, size_t errlen) {
  mpcua_attr a[24];
  int n = mpcua_uac2_attrs(c, a, 24);
  for (int i = 0; i < n; i++) {
    char path[320];
    if (snprintf(path, sizeof path, "%s/%s", fdir, a[i].name) >= (int)sizeof path) return -1;
    int r = mpcua_write_attr(path, a[i].value);
    if (r == 0) continue;
    if (!a[i].required && r == -ENOENT) continue;
    if (err && errlen) snprintf(err, errlen, "%s=%s: %s", a[i].name, a[i].value, strerror(-r));
    if (a[i].required) return -1;
  }
  return 0;
}

int mpcua_gadget_set_iad_class(const char *gdir) {
  static const char *names[] = {"bDeviceClass", "bDeviceSubClass", "bDeviceProtocol"};
  static const char *vals[] = {"0xEF", "0x02", "0x01"};
  for (int i = 0; i < 3; i++) {
    char path[320];
    snprintf(path, sizeof path, "%s/%s", gdir, names[i]);
    int r = mpcua_write_attr(path, vals[i]);
    if (r) return r;
  }
  return 0;
}
