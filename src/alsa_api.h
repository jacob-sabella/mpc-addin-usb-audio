/* alsa_api.h: the slice of the libasound ABI the add-in uses, declared by hand (no ALSA headers
 * needed to build). Everything is resolved with dlsym(RTLD_NEXT) from MPC's already-loaded
 * libasound.so.2, which gives the default (current) symbol versions. */
#ifndef MPCUA_ALSA_API_H
#define MPCUA_ALSA_API_H

#include <poll.h>
#include <stddef.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef struct _snd_pcm_info snd_pcm_info_t;
typedef struct _snd_ctl snd_ctl_t;
typedef struct _snd_ctl_elem_id snd_ctl_elem_id_t;
typedef struct _snd_ctl_elem_info snd_ctl_elem_info_t;
typedef struct _snd_ctl_elem_value snd_ctl_elem_value_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

enum { MPCUA_PCM_PLAYBACK = 0, MPCUA_PCM_CAPTURE = 1 };
enum { MPCUA_PCM_NONBLOCK = 1 };
enum { MPCUA_ACCESS_RW_INTERLEAVED = 3, MPCUA_ACCESS_RW_NONINTERLEAVED = 4 };
enum { MPCUA_STATE_PREPARED = 2, MPCUA_STATE_RUNNING = 3, MPCUA_STATE_XRUN = 4 };
enum { MPCUA_CTL_IFACE_MIXER = 2, MPCUA_CTL_IFACE_PCM = 3 };

typedef struct {
  int (*pcm_open)(snd_pcm_t **, const char *, int, int);
  int (*pcm_close)(snd_pcm_t *);
  size_t (*hw_sizeof)(void);
  int (*hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
  int (*hw_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
  int (*hw_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
  int (*hw_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
  int (*hw_set_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
  int (*hw_set_period_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *);
  int (*hw_set_buffer_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
  int (*hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
  int (*hw_get_format)(const snd_pcm_hw_params_t *, int *);
  int (*hw_get_channels)(const snd_pcm_hw_params_t *, unsigned *);
  int (*hw_get_rate)(const snd_pcm_hw_params_t *, unsigned *, int *);
  int (*hw_get_access)(const snd_pcm_hw_params_t *, int *);
  int (*hw_get_buffer_size)(const snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
  size_t (*sw_sizeof)(void);
  int (*sw_current)(snd_pcm_t *, snd_pcm_sw_params_t *);
  int (*sw_set_start_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
  int (*sw_set_avail_min)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
  int (*sw_params)(snd_pcm_t *, snd_pcm_sw_params_t *);
  int (*prepare)(snd_pcm_t *);
  int (*start)(snd_pcm_t *);
  int (*state)(snd_pcm_t *);
  snd_pcm_sframes_t (*avail_update)(snd_pcm_t *);
  int (*delay)(snd_pcm_t *, snd_pcm_sframes_t *);
  snd_pcm_sframes_t (*writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
  snd_pcm_sframes_t (*readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
  snd_pcm_sframes_t (*writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
  snd_pcm_sframes_t (*readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);
  int (*recover)(snd_pcm_t *, int, int);
  int (*poll_count)(snd_pcm_t *);
  int (*poll_desc)(snd_pcm_t *, struct pollfd *, unsigned);
  size_t (*info_sizeof)(void);
  int (*info)(snd_pcm_t *, snd_pcm_info_t *);
  int (*info_get_card)(const snd_pcm_info_t *);
  int (*info_get_stream)(const snd_pcm_info_t *);
  int (*ctl_open)(snd_ctl_t **, const char *, int);
  int (*ctl_close)(snd_ctl_t *);
  size_t (*id_sizeof)(void);
  void (*id_set_interface)(snd_ctl_elem_id_t *, int);
  void (*id_set_name)(snd_ctl_elem_id_t *, const char *);
  size_t (*einfo_sizeof)(void);
  void (*einfo_set_id)(snd_ctl_elem_info_t *, const snd_ctl_elem_id_t *);
  int (*elem_info)(snd_ctl_t *, snd_ctl_elem_info_t *);
  long (*einfo_get_min)(const snd_ctl_elem_info_t *);
  long (*einfo_get_max)(const snd_ctl_elem_info_t *);
  size_t (*val_sizeof)(void);
  void (*val_set_id)(snd_ctl_elem_value_t *, const snd_ctl_elem_id_t *);
  void (*val_set_integer)(snd_ctl_elem_value_t *, unsigned, long);
  int (*elem_write)(snd_ctl_t *, snd_ctl_elem_value_t *);
} mpcua_alsa;

/* Fill the table. Returns the number of symbols that could not be resolved. */
int mpcua_alsa_resolve(mpcua_alsa *a);

#endif
