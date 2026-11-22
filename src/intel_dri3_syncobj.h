/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2026 Joseph Crowell
 */
#ifndef INTEL_DRI3_SYNCOBJ_H
#define INTEL_DRI3_SYNCOBJ_H

#if HAVE_DRI3 && HAVE_DRI3_SYNCOBJ

#include <stdbool.h>

#include <xorg-server.h>
#include <xf86.h>
#include <dri3.h>

/* DRI3 1.4 timeline sync objects on the kernel's DRM syncobjs, shared by
 * SNA and UXA. Based on glamor's (xserver glamor/glamor_egl.c). */

/* TRUE when the kernel has timeline syncobjs and syncobj eventfd; logs
 * the result for the screen once */
bool intel_dri3_syncobj_supported(ScrnInfoPtr scrn, int drm_fd);

/* fd is the client's DRM timeline syncobj; the DRI3 core closes it */
struct dri3_syncobj *intel_dri3_import_syncobj(ScreenPtr screen, int drm_fd,
					       XID id, int fd);

#endif

#endif /* INTEL_DRI3_SYNCOBJ_H */
