/* test_chmap.c: channel map parsing and frame assembly. */
#include "../src/chmap.h"
#include "t.h"

int main(void) {
  mpcua_chsrc m[MPCUA_MAX_CH];
  const unsigned TO_HOST = MPCUA_SRC_OUT | MPCUA_SRC_IN;
  CHECK_EQ(mpcua_chmap_parse("out1,out2,in1,in2", TO_HOST, m, MPCUA_MAX_CH), 4);
  CHECK_EQ(m[0].kind, MPCUA_SRC_OUT); CHECK_EQ(m[0].idx, 0);
  CHECK_EQ(m[3].kind, MPCUA_SRC_IN); CHECK_EQ(m[3].idx, 1);
  CHECK_EQ(mpcua_chmap_parse(" out2 , 0 ,-", TO_HOST, m, MPCUA_MAX_CH), 3);
  CHECK_EQ(m[0].idx, 1); CHECK_EQ(m[1].kind, MPCUA_SRC_NONE); CHECK_EQ(m[2].kind, MPCUA_SRC_NONE);
  CHECK_EQ(mpcua_chmap_parse("host1", TO_HOST, m, MPCUA_MAX_CH), -1);  /* kind not allowed */
  CHECK_EQ(mpcua_chmap_parse("out0", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out17", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out1,", TO_HOST, m, MPCUA_MAX_CH), -1);   /* empty entry */
  CHECK_EQ(mpcua_chmap_parse("outx", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out1,out1,out1", TO_HOST, m, 2), -1);     /* too many */
  CHECK_EQ(mpcua_chmap_parse("", TO_HOST, m, MPCUA_MAX_CH), -1);

  /* assemble: 4 USB channels from 2 out + 2 in, with out3 missing on a 2-channel device */
  CHECK_EQ(mpcua_chmap_parse("out1,out2,in2,out3", TO_HOST, m, MPCUA_MAX_CH), 4);
  int32_t outf[2] = {10, 20}, inf[2] = {30, 40}, dst[4];
  mpcua_chmap_frame(m, 4, outf, 2, inf, 2, NULL, 0, dst);
  CHECK_EQ(dst[0], 10); CHECK_EQ(dst[1], 20); CHECK_EQ(dst[2], 40); CHECK_EQ(dst[3], 0);
  mpcua_chmap_frame(m, 4, outf, 2, NULL, 0, NULL, 0, dst);              /* no input tap */
  CHECK_EQ(dst[2], 0);

  CHECK_EQ(mpcua_chmap_parse("host2,host1", MPCUA_SRC_HOST, m, MPCUA_MAX_CH), 2);
  int32_t hf[2] = {7, 8};
  mpcua_chmap_frame(m, 2, NULL, 0, NULL, 0, hf, 2, dst);
  CHECK_EQ(dst[0], 8); CHECK_EQ(dst[1], 7);
  T_DONE("chmap");
}
