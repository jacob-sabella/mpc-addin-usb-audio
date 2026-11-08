/* gadget.c: see gadget.h. Runs on MPC's gadget-setup thread or the addin's own threads, never on the
 * audio thread. */
#include "gadget.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

/* ---- the hub mux, and our gadget on the device's own bus ------------------------------------- */

static int read_line(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "re");
  if (!f) return -1;
  if (!fgets(out, (int)n, f)) out[0] = 0;
  fclose(f);
  out[strcspn(out, "\n")] = 0;
  return 0;
}

int mpcua_hub_mux_state(const char *mux_dir, char *state, size_t n) {
  char p[384];
  struct stat st;
  if (snprintf(p, sizeof p, "%s/state", mux_dir) >= (int)sizeof p) return 0;
  if (stat(p, &st) != 0) return 0;
  return read_line(p, state, n) == 0 ? 1 : -1;
}

int mpcua_gadget_on_own_bus(const char *usb_dir, const char *vid, const char *pid, const char *manufacturer) {
  DIR *d = opendir(usb_dir);
  if (!d) return 0;
  struct dirent *de;
  int found = 0;
  while (!found && (de = readdir(d))) {
    if (de->d_name[0] == '.' || strchr(de->d_name, ':')) continue;
    char p[400], v[24], pr[24], m[64];
    if (snprintf(p, sizeof p, "%s/%s/idVendor", usb_dir, de->d_name) >= (int)sizeof p) continue;
    if (read_line(p, v, sizeof v) != 0 || strcmp(v, vid)) continue;
    snprintf(p, sizeof p, "%s/%s/idProduct", usb_dir, de->d_name);
    if (read_line(p, pr, sizeof pr) != 0 || strcmp(pr, pid)) continue;
    snprintf(p, sizeof p, "%s/%s/manufacturer", usb_dir, de->d_name);
    if (read_line(p, m, sizeof m) == 0 && !strcmp(m, manufacturer)) found = 1;
  }
  closedir(d);
  return found;
}

/* ---- our own gadget (standalone fallback) ---------------------------------------------------- */

static int mk(const char *path) {
  return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -errno;
}

static int wr(const char *dir, const char *rel, const char *val) {
  char p[384];
  if (snprintf(p, sizeof p, "%s/%s", dir, rel) >= (int)sizeof p) return -ENAMETOOLONG;
  return mpcua_write_attr(p, val);
}

static int first_entry(const char *dir, char *out, size_t n) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *de;
  int r = -1;
  while ((de = readdir(d)))
    if (de->d_name[0] != '.' && strlen(de->d_name) < n) { memcpy(out, de->d_name, strlen(de->d_name) + 1); r = 0; break; }
  closedir(d);
  return r;
}

int mpcua_other_gadget_present(const mpcua_cfg *c) {
  DIR *d = opendir(c->configfs);
  if (!d) return 0;
  struct dirent *de;
  int found = 0;
  while ((de = readdir(d)))
    if (de->d_name[0] != '.' && strcmp(de->d_name, c->own_gadget) != 0) { found = 1; break; }
  closedir(d);
  return found;
}

int mpcua_own_gadget_remove(const mpcua_cfg *c) {
  char g[256], p[384];
  struct stat st;
  snprintf(g, sizeof g, "%s/%s", c->configfs, c->own_gadget);
  if (stat(g, &st) != 0) return 0;
  wr(g, "UDC", "\n");   /* unbind; fails harmlessly when it isn't bound */
  snprintf(p, sizeof p, "%s/configs/c.1/uac2.%s", g, c->instance); unlink(p);
  snprintf(p, sizeof p, "%s/configs/c.1/strings/0x409", g); rmdir(p);
  snprintf(p, sizeof p, "%s/configs/c.1", g); rmdir(p);
  snprintf(p, sizeof p, "%s/functions/uac2.%s", g, c->instance); rmdir(p);
  snprintf(p, sizeof p, "%s/strings/0x409", g); rmdir(p);
  return rmdir(g) == 0 ? 1 : -errno;
}

#define STEP(expr, what) do { int r_ = (expr); if (r_ < 0) { \
    if (err && errlen) snprintf(err, errlen, "%s: %s", what, strerror(-r_)); \
    mpcua_own_gadget_remove(c); return -1; } } while (0)

int mpcua_own_gadget_create(const mpcua_cfg *c, const char *udc_dir, char *err, size_t errlen) {
  char g[256], p[384], fdir[320], udc[64];
  if (first_entry(udc_dir, udc, sizeof udc) != 0) {
    if (err && errlen) snprintf(err, errlen, "no USB device controller in %s", udc_dir);
    return -1;
  }
  snprintf(g, sizeof g, "%s/%s", c->configfs, c->own_gadget);
  snprintf(fdir, sizeof fdir, "%s/functions/uac2.%s", g, c->instance);

  STEP(mk(g), "gadget");
  /* The Linux Foundation's multifunction composite gadget ID, as configfs examples use. */
  STEP(wr(g, "idVendor", "0x" MPCUA_OWN_VID), "idVendor");
  STEP(wr(g, "idProduct", "0x" MPCUA_OWN_PID), "idProduct");
  STEP(wr(g, "bcdDevice", "0x0100"), "bcdDevice");
  if (c->iad_class) STEP(mpcua_gadget_set_iad_class(g), "device class");
  snprintf(p, sizeof p, "%s/strings/0x409", g);
  STEP(mk(p), "strings");
  STEP(wr(p, "manufacturer", MPCUA_OWN_MANUFACTURER), "manufacturer");
  STEP(wr(p, "product", c->function_name[0] ? c->function_name : "MPC USB Audio"), "product");

  STEP(mk(fdir), "uac2 function");
  char e2[160] = "";
  if (mpcua_uac2_write_attrs(fdir, c, e2, sizeof e2) < 0) {
    if (err && errlen) snprintf(err, errlen, "uac2 attributes: %s", e2);
    mpcua_own_gadget_remove(c);
    return -1;
  }

  snprintf(p, sizeof p, "%s/configs/c.1", g);
  STEP(mk(p), "config");
  snprintf(p, sizeof p, "%s/configs/c.1/strings/0x409", g);
  STEP(mk(p), "config strings");
  STEP(wr(p, "configuration", "USB audio"), "configuration");
  snprintf(p, sizeof p, "%s/configs/c.1/uac2.%s", g, c->instance);
  if (symlink(fdir, p) != 0 && errno != EEXIST) STEP(-errno, "link function");
  STEP(wr(g, "UDC", udc), "bind to the UDC");
  return 0;
}
