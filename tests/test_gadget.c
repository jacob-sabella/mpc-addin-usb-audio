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

int main(void) {
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
  T_DONE("gadget");
}
