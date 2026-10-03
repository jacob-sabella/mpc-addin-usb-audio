/* alsa_api.c: resolve the ALSA table (see alsa_api.h). Called once, never on the audio thread. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "alsa_api.h"

#include <dlfcn.h>
#include <stddef.h>

int mpcua_alsa_resolve(mpcua_alsa *a) {
  int missing = 0;
#define R(field, sym) do { *(void **)(&a->field) = dlsym(RTLD_NEXT, sym); if (!a->field) missing++; } while (0)
  R(pcm_open, "snd_pcm_open");
  R(pcm_close, "snd_pcm_close");
  R(hw_sizeof, "snd_pcm_hw_params_sizeof");
  R(hw_any, "snd_pcm_hw_params_any");
  R(hw_set_access, "snd_pcm_hw_params_set_access");
  R(hw_set_format, "snd_pcm_hw_params_set_format");
  R(hw_set_channels, "snd_pcm_hw_params_set_channels");
  R(hw_set_rate, "snd_pcm_hw_params_set_rate");
  R(hw_set_period_near, "snd_pcm_hw_params_set_period_size_near");
  R(hw_set_buffer_near, "snd_pcm_hw_params_set_buffer_size_near");
  R(hw_params, "snd_pcm_hw_params");
  R(hw_get_format, "snd_pcm_hw_params_get_format");
  R(hw_get_channels, "snd_pcm_hw_params_get_channels");
  R(hw_get_rate, "snd_pcm_hw_params_get_rate");
  R(hw_get_access, "snd_pcm_hw_params_get_access");
  R(hw_get_buffer_size, "snd_pcm_hw_params_get_buffer_size");
  R(sw_sizeof, "snd_pcm_sw_params_sizeof");
  R(sw_current, "snd_pcm_sw_params_current");
  R(sw_set_start_threshold, "snd_pcm_sw_params_set_start_threshold");
  R(sw_set_avail_min, "snd_pcm_sw_params_set_avail_min");
  R(sw_params, "snd_pcm_sw_params");
  R(prepare, "snd_pcm_prepare");
  R(start, "snd_pcm_start");
  R(state, "snd_pcm_state");
  R(avail_update, "snd_pcm_avail_update");
  R(delay, "snd_pcm_delay");
  R(writei, "snd_pcm_writei");
  R(readi, "snd_pcm_readi");
  R(writen, "snd_pcm_writen");
  R(readn, "snd_pcm_readn");
  R(recover, "snd_pcm_recover");
  R(poll_count, "snd_pcm_poll_descriptors_count");
  R(poll_desc, "snd_pcm_poll_descriptors");
  R(info_sizeof, "snd_pcm_info_sizeof");
  R(info, "snd_pcm_info");
  R(info_get_card, "snd_pcm_info_get_card");
  R(info_get_stream, "snd_pcm_info_get_stream");
  R(ctl_open, "snd_ctl_open");
  R(ctl_close, "snd_ctl_close");
  R(id_sizeof, "snd_ctl_elem_id_sizeof");
  R(id_set_interface, "snd_ctl_elem_id_set_interface");
  R(id_set_name, "snd_ctl_elem_id_set_name");
  R(einfo_sizeof, "snd_ctl_elem_info_sizeof");
  R(einfo_set_id, "snd_ctl_elem_info_set_id");
  R(elem_info, "snd_ctl_elem_info");
  R(einfo_get_min, "snd_ctl_elem_info_get_min");
  R(einfo_get_max, "snd_ctl_elem_info_get_max");
  R(val_sizeof, "snd_ctl_elem_value_sizeof");
  R(val_set_id, "snd_ctl_elem_value_set_id");
  R(val_set_integer, "snd_ctl_elem_value_set_integer");
  R(elem_write, "snd_ctl_elem_write");
#undef R
  return missing;
}
