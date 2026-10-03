/* gadget.h: the UAC2 function's configfs attributes, and small configfs file helpers.
 *
 * The function itself is created through MPC's own libusbgx handle (see addin.c) so MPC's recursive
 * teardown removes it; this file only fills in the attribute files between creation and linking.
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

#endif
