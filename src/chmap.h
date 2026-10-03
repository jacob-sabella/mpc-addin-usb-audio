/* chmap.h: channel mapping between MPC's codec streams and the USB audio streams.
 *
 * Spec strings are comma-separated lists, one entry per destination channel:
 *   to_host   = out1,out2,in1,in2   USB channels sent to the computer, taken from MPC's playback
 *                                   (main out) or capture (inputs) channels, 1-based
 *   to_mpc_in = host1,host2         for each MPC input channel, which computer channel lands there
 * An entry of "0" or "-" is silence. Unknown names or out-of-range numbers are parse errors.
 */
#ifndef MPCUA_CHMAP_H
#define MPCUA_CHMAP_H

#include <stdint.h>

#define MPCUA_MAX_CH 16

enum { MPCUA_SRC_NONE = 0, MPCUA_SRC_OUT = 1, MPCUA_SRC_IN = 2, MPCUA_SRC_HOST = 4 };

typedef struct { uint8_t kind; uint8_t idx; /* 0-based */ } mpcua_chsrc;

/* Returns the number of entries parsed (1..max) or -1. `allowed` is a mask of MPCUA_SRC_* kinds. */
int mpcua_chmap_parse(const char *spec, unsigned allowed, mpcua_chsrc *map, int max);

/* Build one frame of `n` channels from one MPC out frame and one MPC in frame. Missing sources (index
 * beyond the live channel count, or a NULL frame) give silence. */
void mpcua_chmap_frame(const mpcua_chsrc *map, int n, const int32_t *outf, unsigned out_ch,
                       const int32_t *inf, unsigned in_ch, const int32_t *hostf, unsigned host_ch,
                       int32_t *dst);

#endif
