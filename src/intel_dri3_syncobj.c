/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2026 Joseph Crowell
 */
#include "config.h"

#include "intel_dri3_syncobj.h"

#if HAVE_DRI3 && HAVE_DRI3_SYNCOBJ

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <xf86drm.h>

struct intel_dri3_syncobj {
	struct dri3_syncobj base;
	int drm_fd;
	uint32_t handle;
};

enum intel_dri3_syncobj_op {
	SYNCOBJ_OP_IMPORT,
	SYNCOBJ_OP_WAIT,
	SYNCOBJ_OP_EXPORT_FENCE,
	SYNCOBJ_OP_IMPORT_FENCE,
	SYNCOBJ_OP_SIGNAL,
	SYNCOBJ_OP_EVENTFD,
	SYNCOBJ_OP_COUNT
};

static inline struct intel_dri3_syncobj *
to_intel_syncobj(struct dri3_syncobj *syncobj)
{
	return (struct intel_dri3_syncobj *)syncobj;
}

/* these run for every presented frame, so only log each failure once */
static void syncobj_warn(ScreenPtr screen, enum intel_dri3_syncobj_op op,
			 const char *call, int err)
{
	static bool warned[SYNCOBJ_OP_COUNT];

	if (warned[op])
		return;
	warned[op] = true;

	xf86DrvMsg(xf86ScreenToScrn(screen)->scrnIndex, X_WARNING,
		   "DRI3 sync object: %s failed: %s\n", call, strerror(err));
}

static void syncobj_free(struct dri3_syncobj *base)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);

	drmSyncobjDestroy(syncobj->drm_fd, syncobj->handle);
	free(syncobj);
}

/* TRUE once the point has a fence (WAIT_AVAILABLE) or has signalled
 * (WAIT_FOR_SUBMIT); ETIME just means "not yet" */
static Bool syncobj_check(struct dri3_syncobj *base, uint64_t point,
			  uint32_t flags)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);
	uint32_t handle = syncobj->handle;

	if (drmSyncobjTimelineWait(syncobj->drm_fd, &handle, &point, 1, 0,
				   flags, NULL) == 0)
		return TRUE;

	if (errno != ETIME)
		syncobj_warn(base->screen, SYNCOBJ_OP_WAIT,
			     "drmSyncobjTimelineWait", errno);
	return FALSE;
}

static Bool syncobj_has_fence(struct dri3_syncobj *base, uint64_t point)
{
	return syncobj_check(base, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static Bool syncobj_is_signaled(struct dri3_syncobj *base, uint64_t point)
{
	return syncobj_check(base, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
}

/* a sync_file fd for the point, owned by the caller, or -1 */
static int syncobj_export_fence(struct dri3_syncobj *base, uint64_t point)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);
	uint32_t temp;
	int fd = -1;

	if (drmSyncobjCreate(syncobj->drm_fd, 0, &temp)) {
		syncobj_warn(base->screen, SYNCOBJ_OP_EXPORT_FENCE,
			     "drmSyncobjCreate", errno);
		return -1;
	}

	if (drmSyncobjTransfer(syncobj->drm_fd, temp, 0,
			       syncobj->handle, point, 0))
		syncobj_warn(base->screen, SYNCOBJ_OP_EXPORT_FENCE,
			     "drmSyncobjTransfer", errno);
	else if (drmSyncobjExportSyncFile(syncobj->drm_fd, temp, &fd)) {
		syncobj_warn(base->screen, SYNCOBJ_OP_EXPORT_FENCE,
			     "drmSyncobjExportSyncFile", errno);
		fd = -1;
	}

	drmSyncobjDestroy(syncobj->drm_fd, temp);
	return fd;
}

static void syncobj_signal(struct dri3_syncobj *base, uint64_t point)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);
	uint32_t handle = syncobj->handle;

	if (drmSyncobjTimelineSignal(syncobj->drm_fd, &handle, &point, 1))
		syncobj_warn(base->screen, SYNCOBJ_OP_SIGNAL,
			     "drmSyncobjTimelineSignal", errno);
}

/* attaches the sync_file fence_fd to the point and closes fence_fd;
 * fence_fd -1 means the work is already complete */
static void syncobj_import_fence(struct dri3_syncobj *base, uint64_t point,
				 int fence_fd)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);
	struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
	uint32_t temp;
	int ret;

	if (fence_fd < 0) {
		syncobj_signal(base, point);
		return;
	}

	ret = drmSyncobjCreate(syncobj->drm_fd, 0, &temp);
	if (ret) {
		syncobj_warn(base->screen, SYNCOBJ_OP_IMPORT_FENCE,
			     "drmSyncobjCreate", errno);
	} else {
		ret = drmSyncobjImportSyncFile(syncobj->drm_fd, temp, fence_fd);
		if (ret)
			syncobj_warn(base->screen, SYNCOBJ_OP_IMPORT_FENCE,
				     "drmSyncobjImportSyncFile", errno);
		else {
			ret = drmSyncobjTransfer(syncobj->drm_fd,
						 syncobj->handle, point,
						 temp, 0, 0);
			if (ret)
				syncobj_warn(base->screen, SYNCOBJ_OP_IMPORT_FENCE,
					     "drmSyncobjTransfer", errno);
		}
		drmSyncobjDestroy(syncobj->drm_fd, temp);
	}

	if (ret) {
		/* the client must still see the point signalled */
		while (poll(&pfd, 1, -1) < 0 && (errno == EINTR || errno == EAGAIN))
			;
		syncobj_signal(base, point);
	}

	close(fence_fd);
}

static void syncobj_eventfd(struct dri3_syncobj *base, uint64_t point,
			    int efd, uint32_t flags)
{
	struct intel_dri3_syncobj *syncobj = to_intel_syncobj(base);

	if (drmSyncobjEventfd(syncobj->drm_fd, syncobj->handle, point,
			      efd, flags) == 0)
		return;

	syncobj_warn(base->screen, SYNCOBJ_OP_EVENTFD,
		     "drmSyncobjEventfd", errno);

	/* wake the waiter anyway; it checks the point again */
	if (eventfd_write(efd, 1) != 0)
		syncobj_warn(base->screen, SYNCOBJ_OP_EVENTFD,
			     "eventfd_write", errno);
}

static void syncobj_submitted_eventfd(struct dri3_syncobj *base,
				      uint64_t point, int efd)
{
	syncobj_eventfd(base, point, efd, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static void syncobj_signaled_eventfd(struct dri3_syncobj *base,
				     uint64_t point, int efd)
{
	syncobj_eventfd(base, point, efd, 0);
}

bool intel_dri3_syncobj_supported(ScrnInfoPtr scrn, int drm_fd)
{
	const char *reason = NULL;
	uint64_t cap = 0;

	if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap)
		reason = "no timeline syncobjs";
	else {
		/* an invalid handle gives ENOENT only if eventfd is supported */
		errno = 0;
		if (drmSyncobjEventfd(drm_fd, 0, 0, -1, 0) == 0 || errno != ENOENT)
			reason = "no syncobj eventfd";
	}

	if (reason)
		xf86DrvMsg(scrn->scrnIndex, X_INFO,
			   "DRI3 sync objects: disabled (%s)\n", reason);
	else
		xf86DrvMsg(scrn->scrnIndex, X_INFO,
			   "DRI3 sync objects: enabled\n");

	return reason == NULL;
}

struct dri3_syncobj *intel_dri3_import_syncobj(ScreenPtr screen, int drm_fd,
					       XID id, int fd)
{
	struct intel_dri3_syncobj *syncobj;
	uint32_t handle;

	if (drmSyncobjFDToHandle(drm_fd, fd, &handle)) {
		syncobj_warn(screen, SYNCOBJ_OP_IMPORT,
			     "drmSyncobjFDToHandle", errno);
		return NULL;
	}

	syncobj = calloc(1, sizeof(*syncobj));
	if (syncobj == NULL) {
		drmSyncobjDestroy(drm_fd, handle);
		return NULL;
	}

	syncobj->base.id = id;
	syncobj->base.screen = screen;
	syncobj->base.refcount = 1;
	syncobj->base.free = syncobj_free;
	syncobj->base.has_fence = syncobj_has_fence;
	syncobj->base.is_signaled = syncobj_is_signaled;
	syncobj->base.export_fence = syncobj_export_fence;
	syncobj->base.import_fence = syncobj_import_fence;
	syncobj->base.signal = syncobj_signal;
	syncobj->base.submitted_eventfd = syncobj_submitted_eventfd;
	syncobj->base.signaled_eventfd = syncobj_signaled_eventfd;
	syncobj->drm_fd = drm_fd;
	syncobj->handle = handle;

	return &syncobj->base;
}

#endif /* HAVE_DRI3 && HAVE_DRI3_SYNCOBJ */
