/* test_gadget.c: attribute list and writing into a fake function directory. Real configfs creates
 * the attribute files itself; here the test pre-creates the ones a kernel would have. */
#include "../src/gadget.h"
#include "t.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *dir, const char *name) {
  char p[256]; snprintf(p, sizeof p, "%s/%s", dir, name);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); close(fd);
}
static void slurp(const char *dir, const char *name, char *out, size_t n) {
  char p[256]; snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "r"); out[0] = 0;
  if (f) { size_t r = fread(out, 1, n - 1, f); out[r] = 0; fclose(f); }
}

static void sh(const char *fmt, const char *a) {
  char cmd[600]; snprintf(cmd, sizeof cmd, fmt, a, a, a, a, a, a);
  CHECK(system(cmd) == 0);
}

/* The standalone fallback against a fake configfs: the test lays out the directories and attribute
 * files the kernel would make on each mkdir, and a fake /sys/class/udc. */
static void own_gadget(void) {
  char root[] = "/tmp/mpcua_own_XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  mpcua_cfg c; mpcua_cfg_defaults(&c);
  char err[200] = "", cfs[96], udc[96], g[160], v[64], p[256];
  snprintf(cfs, sizeof cfs, "%s/cfs", root);
  snprintf(udc, sizeof udc, "%s/udc", root);
  snprintf(c.configfs, sizeof c.configfs, "%s", cfs);
  snprintf(g, sizeof g, "%s/mpcusbaudio", cfs);

  CHECK_EQ(mpcua_own_gadget_create(&c, udc, err, sizeof err), -1);   /* no UDC at all */
  CHECK(strstr(err, "no USB device controller") != NULL);

  sh("mkdir -p '%s/udc/fe800000.usb' '%s/cfs/mpcusbaudio/strings/0x409' "
     "'%s/cfs/mpcusbaudio/functions/uac2.usbaudio' '%s/cfs/mpcusbaudio/configs/c.1/strings/0x409'", root);
  sh("cd '%s/cfs/mpcusbaudio' && touch idVendor idProduct bcdDevice bDeviceClass bDeviceSubClass "
     "bDeviceProtocol UDC strings/0x409/manufacturer strings/0x409/product configs/c.1/strings/0x409/configuration && "
     "cd functions/uac2.usbaudio && touch p_chmask p_srate p_ssize c_chmask c_srate c_ssize", root);
  CHECK_EQ(mpcua_other_gadget_present(&c), 0);              /* only ours */
  CHECK_EQ(mpcua_own_gadget_create(&c, udc, err, sizeof err), 0);
  slurp(g, "UDC", v, sizeof v); CHECK(!strcmp(v, "fe800000.usb"));
  slurp(g, "idVendor", v, sizeof v); CHECK(!strcmp(v, "0x1d6b"));
  slurp(g, "bDeviceClass", v, sizeof v); CHECK(!strcmp(v, "0xEF"));
  slurp(g, "strings/0x409/product", v, sizeof v); CHECK(!strcmp(v, "MPC USB Audio"));
  slurp(g, "strings/0x409/manufacturer", v, sizeof v); CHECK(!strcmp(v, "mpc-addin-usb-audio"));
  slurp(g, "functions/uac2.usbaudio/p_chmask", v, sizeof v); CHECK(!strcmp(v, "0xf"));
  char link[256] = ""; snprintf(p, sizeof p, "%s/configs/c.1/uac2.usbaudio", g);
  CHECK(readlink(p, link, sizeof link - 1) > 0); CHECK(strstr(link, "functions/uac2.usbaudio") != NULL);

  /* removal: unbind first. Real configfs attribute files don't stop rmdir; here they do, until gone */
  CHECK(mpcua_own_gadget_remove(&c) < 0);
  slurp(g, "UDC", v, sizeof v); CHECK(!strcmp(v, "\n"));
  CHECK(access(p, F_OK) != 0);                              /* the function is unlinked from the config */
  sh("find '%s/cfs/mpcusbaudio' -type f -delete", root);
  CHECK(mpcua_own_gadget_remove(&c) < 0);                   /* configfs's own groups stand in the way here */
  sh("cd '%s/cfs/mpcusbaudio' && rmdir configs/c.1/strings configs/c.1 configs functions strings", root);
  CHECK_EQ(mpcua_own_gadget_remove(&c), 1);
  CHECK(access(g, F_OK) != 0);
  CHECK_EQ(mpcua_own_gadget_remove(&c), 0);                 /* nothing there */

  /* a required attribute the kernel lacks: fails, says which, and cleans up what it made */
  sh("mkdir -p '%s/cfs/mpcusbaudio/strings/0x409' && cd '%s/cfs/mpcusbaudio' && touch idVendor", root);
  CHECK_EQ(mpcua_own_gadget_create(&c, udc, err, sizeof err), -1);
  CHECK(strstr(err, "idProduct") != NULL);

  /* any other gadget directory counts as MPC's */
  sh("mkdir -p '%s/cfs/standalone'", root);
  CHECK_EQ(mpcua_other_gadget_present(&c), 1);
  sh("rm -rf '%s'", root);
}

/* The hub mux some models have, and our gadget showing up on the device's own USB bus. */
static void hub_checks(void) {
  char root[] = "/tmp/mpcua_hub_XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char mux[160], usb[160], state[32] = "x";
  snprintf(mux, sizeof mux, "%s/usb-mux", root);
  snprintf(usb, sizeof usb, "%s/usb", root);
  CHECK_EQ(mpcua_hub_mux_state(mux, state, sizeof state), 0);          /* no mux: port wired to USB-B */
  sh("mkdir -p '%s/usb-mux' && printf 'internal\\n' > '%s/usb-mux/state'", root);
  CHECK_EQ(mpcua_hub_mux_state(mux, state, sizeof state), 1);
  CHECK(!strcmp(state, "internal"));
  sh("printf 'external\\n' > '%s/usb-mux/state'", root);
  CHECK_EQ(mpcua_hub_mux_state(mux, state, sizeof state), 1);
  CHECK(!strcmp(state, "external"));

  CHECK_EQ(mpcua_gadget_on_own_bus(usb, "1d6b", "0104", "mpc-addin-usb-audio"), 0);   /* no sysfs at all */
  /* a root hub (same vendor, other product), an interface entry, another 1d6b:0104 device, then ours */
  sh("mkdir -p '%s/usb/usb2' '%s/usb/2-1.7:1.0' '%s/usb/2-1.1' '%s/usb/2-1.7'", root);
  sh("cd '%s/usb/usb2' && printf '1d6b\\n' > idVendor && printf '0002\\n' > idProduct && printf 'Linux\\n' > manufacturer", root);
  sh("cd '%s/usb/2-1.1' && printf '1d6b\\n' > idVendor && printf '0104\\n' > idProduct && printf 'Some Pi\\n' > manufacturer", root);
  CHECK_EQ(mpcua_gadget_on_own_bus(usb, "1d6b", "0104", "mpc-addin-usb-audio"), 0);   /* not ours */
  sh("cd '%s/usb/2-1.7' && printf '1d6b\\n' > idVendor && printf '0104\\n' > idProduct && printf 'mpc-addin-usb-audio\\n' > manufacturer", root);
  CHECK_EQ(mpcua_gadget_on_own_bus(usb, "1d6b", "0104", "mpc-addin-usb-audio"), 1);
  sh("rm -rf '%s'", root);
}

int main(void) {
  hub_checks();
  mpcua_cfg c; mpcua_cfg_defaults(&c);
  mpcua_attr a[24];
  int n = mpcua_uac2_attrs(&c, a, 24);
  CHECK(n >= 10);
  CHECK(!strcmp(a[0].name, "p_chmask")); CHECK(!strcmp(a[0].value, "0xf"));   /* 4 to the computer */
  CHECK(!strcmp(a[3].name, "c_chmask")); CHECK(!strcmp(a[3].value, "0x3"));   /* 2 from it */
  CHECK(!strcmp(a[1].value, "44100"));

  char dir[] = "/tmp/mpcua_fn_XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  const char *kernel_attrs[] = {"p_chmask", "p_srate", "p_ssize", "c_chmask", "c_srate", "c_ssize",
                                "c_sync", "p_hs_bint", "c_hs_bint", "p_volume_present"};
  for (unsigned i = 0; i < sizeof kernel_attrs / sizeof *kernel_attrs; i++) touch(dir, kernel_attrs[i]);
  char err[160] = "";
  CHECK_EQ(mpcua_uac2_write_attrs(dir, &c, err, sizeof err), 0);   /* missing optionals skipped */
  char v[64];
  slurp(dir, "p_srate", v, sizeof v); CHECK(!strcmp(v, "44100"));
  slurp(dir, "c_sync", v, sizeof v); CHECK(!strcmp(v, "async"));
  slurp(dir, "p_hs_bint", v, sizeof v); CHECK(!strcmp(v, "4"));
  slurp(dir, "p_volume_present", v, sizeof v); CHECK(!strcmp(v, "0"));

  /* a missing required attribute fails */
  char p[256]; snprintf(p, sizeof p, "%s/c_ssize", dir); unlink(p);
  CHECK_EQ(mpcua_uac2_write_attrs(dir, &c, err, sizeof err), -1);
  CHECK(strstr(err, "c_ssize") != NULL);

  /* IAD class */
  touch(dir, "bDeviceClass"); touch(dir, "bDeviceSubClass"); touch(dir, "bDeviceProtocol");
  CHECK_EQ(mpcua_gadget_set_iad_class(dir), 0);
  slurp(dir, "bDeviceClass", v, sizeof v); CHECK(!strcmp(v, "0xEF"));

  /* host_channels=0: capture disabled, no c_sync written */
  mpcua_cfg_parse(&c, "host_channels=0\n", err, sizeof err);
  n = mpcua_uac2_attrs(&c, a, 24);
  CHECK(!strcmp(a[3].value, "0x0"));
  for (int i = 0; i < n; i++) CHECK(strcmp(a[i].name, "c_sync") != 0);

  char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  CHECK(system(cmd) == 0);

  own_gadget();
  T_DONE("gadget");
}
