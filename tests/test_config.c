/* test_config.c: defaults, parsing, bad values, map fallbacks, file loading. */
#include "../src/config.h"
#include "t.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  mpcua_cfg c; char err[160];
  mpcua_cfg_defaults(&c);
  CHECK_EQ(c.enabled, 1); CHECK_EQ(c.rate, 44100); CHECK_EQ(c.n_to_host, 4); CHECK_EQ(c.n_to_mpc, 2);
  CHECK(!strcmp(c.gadget, "standalone")); CHECK_EQ(c.input_mode, MPCUA_INPUT_SUM);
  CHECK_EQ(c.tap_card, -1);

  const char *ok =
    "# comment\n"
    "  rate = 48000   # trailing comment\n"
    "to_host=out1,out2\n"
    "input_mode=replace\n"
    "tap_card=1\n"
    "drift_bw=0.02\n"
    "function_name=Studio Link\n"
    "\n";
  CHECK_EQ(mpcua_cfg_parse(&c, ok, err, sizeof err), 0);
  CHECK_EQ(c.rate, 48000); CHECK_EQ(c.n_to_host, 2); CHECK_EQ(c.input_mode, MPCUA_INPUT_REPLACE);
  CHECK_EQ(c.tap_card, 1); CHECK(c.drift_bw > 0.0199 && c.drift_bw < 0.0201);
  CHECK(!strcmp(c.function_name, "Studio Link"));

  mpcua_cfg_defaults(&c);
  int bad = mpcua_cfg_parse(&c, "rate=12\nfoo=1\nnoequals\ngadget=../etc\nsample_bytes=5\n", err, sizeof err);
  CHECK_EQ(bad, 5);
  CHECK(strstr(err, "rate") != NULL);           /* first problem is reported */
  CHECK_EQ(c.rate, 44100); CHECK(!strcmp(c.gadget, "standalone")); CHECK_EQ(c.sample_bytes, 4);

  /* bad maps fall back to defaults */
  mpcua_cfg_defaults(&c);
  CHECK_EQ(mpcua_cfg_parse(&c, "to_host=out1,bogus\n", err, sizeof err), 1);
  CHECK_EQ(c.n_to_host, 4);
  CHECK(strstr(err, "to_host") != NULL);
  mpcua_cfg_defaults(&c);
  CHECK_EQ(mpcua_cfg_parse(&c, "to_mpc_in=host3,host1\n", err, sizeof err), 1);  /* host3 > 2 */
  CHECK_EQ(c.n_to_mpc, 2);
  mpcua_cfg_defaults(&c);
  CHECK_EQ(mpcua_cfg_parse(&c, "host_channels=0\n", err, sizeof err), 1);
  CHECK_EQ(c.input_mode, MPCUA_INPUT_OFF);
  mpcua_cfg_defaults(&c);
  CHECK_EQ(mpcua_cfg_parse(&c, "host_channels=4\nto_mpc_in=host4,host3\n", err, sizeof err), 0);
  CHECK_EQ(c.to_mpc_map[0].idx, 3);

  /* file loading */
  char path[] = "/tmp/mpcua_cfg_XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(write(fd, "enabled=0\n", 10) == 10);
  close(fd);
  CHECK_EQ(mpcua_cfg_load(&c, path, err, sizeof err), 0);
  CHECK_EQ(c.enabled, 0);
  unlink(path);
  CHECK_EQ(mpcua_cfg_load(&c, path, err, sizeof err), 0);    /* missing file: defaults */
  CHECK_EQ(c.enabled, 1);
  T_DONE("config");
}
