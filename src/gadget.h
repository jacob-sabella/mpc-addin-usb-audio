/* gadget.h: the UAC2 function's configfs attributes, and small configfs file helpers.
 *
 * The function itself is created through MPC's own libusbgx handle (see addin.c) so MPC's recursive
 * teardown removes it; this file fills in the attribute files between creation and linking. Where MPC
 * builds no gadget, it also builds and removes one of the addin's own (the standalone fallback below).
 */
#ifndef MPCUA_GADGET_H
#define MPCUA_GADGET_H

#include <stddef.h>
#include "config.h"

typedef struct { const char *name; char value[72]; int required; } mpcua_attr;

/* Attribute list for the UAC2 function described by cfg. Returns the count. */
int mpcua_uac2_attrs(const mpcua_cfg *cfg, mpcua_attr *out, int max);

/* Write the attributes into an existing function directory. Required attributes must succeed;
 * optional ones that the kernel does not have (ENOENT) are skipped. Returns 0 or -1 (err filled). */
int mpcua_uac2_write_attrs(const char *fdir, const mpcua_cfg *cfg, char *err, size_t errlen);

/* Device class EF/02/01 (Misc / IAD), needed by Windows for a composite device with an IAD. */
int mpcua_gadget_set_iad_class(const char *gdir);

/* Write a value to an existing attribute file (no O_CREAT). Returns 0 or -errno. */
int mpcua_write_attr(const char *path, const char *value);

/* The standalone fallback, for firmware where MPC builds no gadget in standalone mode (an MPC One on
 * HAKAI 3.9.1, issue #4): a gadget of our own, <configfs>/<own_gadget>, with one uac2.<instance>
 * function, bound to the first UDC in udc_dir (/sys/class/udc). Real configfs creates the attribute
 * files itself. Returns 0, or -1 with err filled and whatever was made removed again. */
int mpcua_own_gadget_create(const mpcua_cfg *cfg, const char *udc_dir, char *err, size_t errlen);
/* Unbind and remove <configfs>/<own_gadget>. Returns 1 if it was removed, 0 if it wasn't there, or
 * -errno while it is still there. */
int mpcua_own_gadget_remove(const mpcua_cfg *cfg);
/* 1 when configfs holds any gadget other than ours (MPC is building or using one). */
int mpcua_other_gadget_present(const mpcua_cfg *cfg);

/* Our own gadget's USB ids (the Linux Foundation multifunction composite id, as configfs examples use)
 * and manufacturer string, as sysfs prints them. */
#define MPCUA_OWN_VID "1d6b"
#define MPCUA_OWN_PID "0104"
#define MPCUA_OWN_MANUFACTURER "mpc-addin-usb-audio"

/* The USB hub mux some models have (the Force; the Live II and X by their device trees):
 * <mux_dir>/state (/sys/devices/platform/usb-mux) reads "internal" in standalone mode, when the
 * internal hub belongs to the device itself, and "external" in Computer Mode, when a computer holds
 * it. The device port sits behind that hub, so with "internal" a gadget is enumerated by the device
 * itself and no computer hears it. Returns 1 with the state filled, 0 when there is no mux (the device
 * port is wired to the USB-B socket), -1 when it exists but cannot be read. */
int mpcua_hub_mux_state(const char *mux_dir, char *state, size_t n);

/* 1 when a device with these ids and manufacturer (as sysfs prints them: "1d6b", "0104") is on the
 * device's own USB buses under usb_dir (/sys/bus/usb/devices): our gadget, looped back to the device
 * that built it. Interface entries (with a colon) are skipped. */
int mpcua_gadget_on_own_bus(const char *usb_dir, const char *vid, const char *pid, const char *manufacturer);

#endif
