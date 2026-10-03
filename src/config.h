/* config.h: addin settings, read once from a key=value file (default
 * usbaudio.conf next to the .so, or $MPC_USB_AUDIO_CONF). A missing file means defaults. */
#ifndef MPCUA_CONFIG_H
#define MPCUA_CONFIG_H

#include <stddef.h>
#include "chmap.h"

#define MPCUA_CONF_NAME "usbaudio.conf"   /* next to the .so, unless $MPC_USB_AUDIO_CONF names one */

enum { MPCUA_INPUT_OFF = 0, MPCUA_INPUT_SUM = 1, MPCUA_INPUT_REPLACE = 2 };

typedef struct {
  int enabled;              /* enabled=1 */
  char gadget[64];          /* gadget=standalone  (the gadget MPC creates in standalone mode) */
  char instance[32];        /* instance=usbaudio  (functions/uac2.<instance>) */
  char configfs[128];       /* configfs=/sys/kernel/config/usb_gadget */
  unsigned rate;            /* rate=44100         (must equal MPC's codec rate) */
  unsigned sample_bytes;    /* sample_bytes=4     (2, 3 or 4 on the USB side) */
  char to_host[128];        /* to_host=out1,out2,in1,in2 */
  unsigned host_channels;   /* host_channels=2    (computer -> MPC; 0 disables) */
  char to_mpc_in[128];      /* to_mpc_in=host1,host2 */
  int input_mode;           /* input_mode=sum | replace | off */
  unsigned hs_bint;         /* hs_bint=4          (USB interval 2^(n-1) x 125 us) */
  unsigned req_number;      /* req_number=0       (0 = kernel default) */
  int iad_class;            /* iad_class=1        (device class EF/02/01 for composite + IAD) */
  char function_name[64];   /* function_name=MPC USB Audio */
  int tap_card;             /* tap_card=auto | <ALSA card index> (auto = by-path platform-sound) */
  int target_frames;        /* target_frames=256  (queued frames per direction; -1 = auto) */
  unsigned fwd_period;      /* fwd_period=64      (gadget PCM period, frames) */
  unsigned fwd_periods;     /* fwd_periods=4 */
  double max_ppm;           /* max_ppm=1000 */
  double drift_bw;          /* drift_bw=0.01 (Hz) */
  int test_tone;            /* test_tone=0        (1: send a 1 kHz tone instead of the tap) */
  char log_path[160];       /* log=auto: usbaudio.log next to the .so; a path; "" disables */

  mpcua_chsrc to_host_map[MPCUA_MAX_CH]; int n_to_host;
  mpcua_chsrc to_mpc_map[MPCUA_MAX_CH];  int n_to_mpc;
} mpcua_cfg;

void mpcua_cfg_defaults(mpcua_cfg *c);
/* Parse text into c (on top of whatever is there). Returns the number of problems; the first is
 * described in err. Bad values keep the previous value. Ends with mpcua_cfg_finish. */
int mpcua_cfg_parse(mpcua_cfg *c, const char *text, char *err, size_t errlen);
/* Defaults + file. A missing file is not an error. Returns problems as above, -1 on read error. */
int mpcua_cfg_load(mpcua_cfg *c, const char *path, char *err, size_t errlen);
/* Parse the channel maps; on error falls back to the default maps. Returns 0 or -1. */
int mpcua_cfg_finish(mpcua_cfg *c, char *err, size_t errlen);

#endif
