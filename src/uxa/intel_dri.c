/**************************************************************************

Copyright 2001 VA Linux Systems Inc., Fremont, California.
Copyright © 2002 by David Dawes

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
ATI, VA LINUX SYSTEMS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors: Jeff Hartmann <jhartmann@valinux.com>
 *          David Dawes <dawes@xfree86.org>
 *          Keith Whitwell <keith@tungstengraphics.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"

#include "xf86Pci.h"
#include "xf86drm.h"

#include "windowstr.h"
#include "shadow.h"
#include "fb.h"

#include "intel.h"
#include "i830_reg.h"

#include "i915_drm.h"

#include "dri2.h"

#if USE_UXA
#include "intel_uxa.h"
#endif

typedef struct {
	int refcnt;
	PixmapPtr pixmap;
} I830DRI2BufferPrivateRec, *I830DRI2BufferPrivatePtr;

#if HAS_DEVPRIVATEKEYREC
static DevPrivateKeyRec i830_client_key;
#else
static int i830_client_key;
#endif

static void I830DRI2FlipEventHandler(unsigned int frame,
				     unsigned int tv_sec,
				     unsigned int tv_usec,
				     DRI2FrameEventPtr flip_info);

static void I830DRI2FrameEventHandler(unsigned int frame,
				      unsigned int tv_sec,
				      unsigned int tv_usec,
				      DRI2FrameEventPtr swap_info);

static void
i830_dri2_del_frame_event(DRI2FrameEventPtr info);

static uint32_t crtc_select(int index)
{
	if (index > 1)
		return index << DRM_VBLANK_HIGH_CRTC_SHIFT;
	else if (index > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static void
intel_dri2_vblank_handler(ScrnInfoPtr scrn,
                          xf86CrtcPtr crtc,
                          uint64_t msc,
                          uint64_t usec,
                          void *data)
{
        I830DRI2FrameEventHandler((uint32_t) msc, usec / 1000000, usec % 1000000, data);
}

static void
intel_dri2_vblank_abort(ScrnInfoPtr scrn,
                        xf86CrtcPtr crtc,
                        void *data)
{
        i830_dri2_del_frame_event(data);
}

static uint32_t pixmap_flink(PixmapPtr pixmap)
{
	struct intel_uxa_pixmap *priv = intel_uxa_get_pixmap_private(pixmap);
	uint32_t name;

	if (priv == NULL || priv->bo == NULL)
		return 0;

	if (dri_bo_flink(priv->bo, &name) != 0)
		return 0;

	priv->pinned |= PIN_DRI2;
	return name;
}

static PixmapPtr get_front_buffer(DrawablePtr drawable)
{
	PixmapPtr pixmap;

	pixmap = get_drawable_pixmap(drawable);
	if (!intel_get_pixmap_bo(pixmap))
		return NULL;

	pixmap->refcnt++;
	return pixmap;
}

#if DRI2INFOREC_VERSION < 2
static DRI2BufferPtr
I830DRI2CreateBuffers(DrawablePtr drawable, unsigned int *attachments,
		      int count)
{
	ScreenPtr screen = drawable->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DRI2BufferPtr buffers;
	I830DRI2BufferPrivatePtr privates;
	PixmapPtr pixmap, pDepthPixmap;
	int i;

	buffers = calloc(count, sizeof *buffers);
	if (buffers == NULL)
		return NULL;
	privates = calloc(count, sizeof *privates);
	if (privates == NULL) {
		free(buffers);
		return NULL;
	}

	pDepthPixmap = NULL;
	for (i = 0; i < count; i++) {
		pixmap = NULL;
		if (attachments[i] == DRI2BufferFrontLeft) {
			pixmap = get_front_buffer(drawable);
		} else if (attachments[i] == DRI2BufferStencil && pDepthPixmap) {
			pixmap = pDepthPixmap;
			pixmap->refcnt++;
		}

		if (pixmap == NULL) {
			unsigned int hint = INTEL_CREATE_PIXMAP_DRI2;

			if (intel->tiling & INTEL_TILING_3D) {
				switch (attachments[i]) {
				case DRI2BufferDepth:
					if (SUPPORTS_YTILING(intel))
						hint |= INTEL_CREATE_PIXMAP_TILING_Y;
					else
						hint |= INTEL_CREATE_PIXMAP_TILING_X;
					break;
				case DRI2BufferFakeFrontLeft:
				case DRI2BufferFakeFrontRight:
				case DRI2BufferBackLeft:
				case DRI2BufferBackRight:
					hint |= INTEL_CREATE_PIXMAP_TILING_X;
					break;
				}
			}

			pixmap = screen->CreatePixmap(screen,
						      drawable->width,
						      drawable->height,
						      drawable->depth,
						      hint);
			if (pixmap == NULL ||
			    intel_get_pixmap_bo(pixmap) == NULL)
			{
				if (pixmap)
					dixDestroyPixmap(pixmap, 0);
				goto unwind;
			}
		}

		if (attachments[i] == DRI2BufferDepth)
			pDepthPixmap = pixmap;

		buffers[i].attachment = attachments[i];
		buffers[i].pitch = pixmap->devKind;
		buffers[i].cpp = pixmap->drawable.bitsPerPixel / 8;
		buffers[i].driverPrivate = &privates[i];
		buffers[i].flags = 0;	/* not tiled */
		privates[i].refcnt = 1;
		privates[i].pixmap = pixmap;

		if ((buffers[i].name = pixmap_flink(pixmap)) == 0) {
			/* failed to name buffer */
			dixDestroyPixmap(pixmap, 0);
			goto unwind;
		}
	}

	return buffers;

unwind:
	while (i--)
		dixDestroyPixmap(privates[i].pixmap, 0);
	free(privates);
	free(buffers);
	return NULL;
}

static void
I830DRI2DestroyBuffers(DrawablePtr drawable, DRI2BufferPtr buffers, int count)
{
	ScreenPtr screen = drawable->pScreen;
	I830DRI2BufferPrivatePtr private;
	int i;

	for (i = 0; i < count; i++) {
		private = buffers[i].driverPrivate;
		dixDestroyPixmap(private->pixmap, 0);
	}

	if (buffers) {
		free(buffers[0].driverPrivate);
		free(buffers);
	}
}

#else

static DRI2Buffer2Ptr
I830DRI2CreateBuffer(DrawablePtr drawable, unsigned int attachment,
		     unsigned int format)
{
	ScreenPtr screen = drawable->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DRI2Buffer2Ptr buffer;
	I830DRI2BufferPrivatePtr privates;
	PixmapPtr pixmap;

	buffer = calloc(1, sizeof *buffer);
	if (buffer == NULL)
		return NULL;
	privates = calloc(1, sizeof *privates);
	if (privates == NULL) {
		free(buffer);
		return NULL;
	}

	pixmap = NULL;
	if (attachment == DRI2BufferFrontLeft)
		pixmap = get_front_buffer(drawable);

	if (pixmap == NULL) {
		unsigned int hint = INTEL_CREATE_PIXMAP_DRI2;
		int pixmap_width = drawable->width;
		int pixmap_height = drawable->height;
		int pixmap_cpp = (format != 0) ? format : drawable->depth;

		if (intel->tiling & INTEL_TILING_3D) {
			switch (attachment) {
			case DRI2BufferDepth:
			case DRI2BufferDepthStencil:
			case DRI2BufferHiz:
				if (SUPPORTS_YTILING(intel)) {
					hint |= INTEL_CREATE_PIXMAP_TILING_Y;
					break;
				}
				/* fall through */
			case DRI2BufferAccum:
			case DRI2BufferBackLeft:
			case DRI2BufferBackRight:
			case DRI2BufferFakeFrontLeft:
			case DRI2BufferFakeFrontRight:
			case DRI2BufferFrontLeft:
			case DRI2BufferFrontRight:
				hint |= INTEL_CREATE_PIXMAP_TILING_X;
				break;
			case DRI2BufferStencil:
				/*
				 * The stencil buffer is W tiled. However, we
				 * request from the kernel a non-tiled buffer
				 * because the GTT is incapable of W fencing.
				 */
				hint |= INTEL_CREATE_PIXMAP_TILING_NONE;
				break;
			default:
				free(privates);
				free(buffer);
				return NULL;
                        }
		}

		/*
		 * The stencil buffer has quirky pitch requirements.  From Vol
		 * 2a, 11.5.6.2.1 3DSTATE_STENCIL_BUFFER, field "Surface
		 * Pitch":
		 *    The pitch must be set to 2x the value computed based on
		 *    width, as the stencil buffer is stored with two rows
		 *    interleaved.
		 * To accomplish this, we resort to the nasty hack of doubling
		 * the drm region's cpp and halving its height.
		 *
		 * If we neglect to double the pitch, then render corruption
		 * occurs.
		 */
		if (attachment == DRI2BufferStencil) {
			pixmap_width = ALIGN(pixmap_width, 64);
			pixmap_height = ALIGN((pixmap_height + 1) / 2, 64);
			pixmap_cpp *= 2;
		}

		pixmap = screen->CreatePixmap(screen,
					      pixmap_width,
					      pixmap_height,
					      pixmap_cpp,
					      hint);
		if (pixmap == NULL || intel_get_pixmap_bo(pixmap) == NULL) {
			if (pixmap)
				dixDestroyPixmap(pixmap, 0);
			free(privates);
			free(buffer);
			return NULL;
		}
	}

	buffer->attachment = attachment;
	buffer->pitch = pixmap->devKind;
	buffer->cpp = pixmap->drawable.bitsPerPixel / 8;
	buffer->driverPrivate = privates;
	buffer->format = format;
	buffer->flags = 0;	/* not tiled */
	privates->refcnt = 1;
	privates->pixmap = pixmap;

	if ((buffer->name = pixmap_flink(pixmap)) == 0) {
		/* failed to name buffer */
		dixDestroyPixmap(pixmap, 0);
		free(privates);
		free(buffer);
		return NULL;
	}

	return buffer;
}

static void I830DRI2DestroyBuffer(DrawablePtr drawable, DRI2Buffer2Ptr buffer)
{
	if (buffer && buffer->driverPrivate) {
		I830DRI2BufferPrivatePtr private = buffer->driverPrivate;
		if (--private->refcnt == 0) {
			ScreenPtr screen = private->pixmap->drawable.pScreen;
			dixDestroyPixmap(private->pixmap, 0);

			free(private);
			free(buffer);
		}
	} else
		free(buffer);
}

#endif

static void
I830DRI2CopyRegion(DrawablePtr drawable, RegionPtr pRegion,
		   DRI2BufferPtr destBuffer, DRI2BufferPtr sourceBuffer)
{
	I830DRI2BufferPrivatePtr srcPrivate = sourceBuffer->driverPrivate;
	I830DRI2BufferPrivatePtr dstPrivate = destBuffer->driverPrivate;
	ScreenPtr screen = drawable->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DrawablePtr src = (sourceBuffer->attachment == DRI2BufferFrontLeft)
		? drawable : &srcPrivate->pixmap->drawable;
	DrawablePtr dst = (destBuffer->attachment == DRI2BufferFrontLeft)
		? drawable : &dstPrivate->pixmap->drawable;
	RegionPtr pCopyClip;
	GCPtr gc;

	gc = GetScratchGC(dst->depth, screen);
	if (!gc)
		return;

	pCopyClip = REGION_CREATE(screen, NULL, 0);
	REGION_COPY(screen, pCopyClip, pRegion);
	(*gc->funcs->ChangeClip) (gc, CT_REGION, pCopyClip, 0);
	ValidateGC(dst, gc);

	/* Wait for the scanline to be outside the region to be copied */
	if (scrn->vtSema &&
	    pixmap_is_scanout(get_drawable_pixmap(dst)) &&
	    intel->swapbuffers_wait && INTEL_INFO(intel)->gen < 060) {
		BoxPtr box;
		BoxRec crtcbox;
		int y1, y2;
		int event, load_scan_lines_pipe;
		xf86CrtcPtr crtc;
		Bool full_height = FALSE;

		box = REGION_EXTENTS(unused, gc->pCompositeClip);
		crtc = intel_covering_crtc(scrn, box, NULL, &crtcbox);

		/*
		 * Make sure the CRTC is valid and this is the real front
		 * buffer
		 */
		if (crtc != NULL && !crtc->rotatedData) {
			int pipe = intel_crtc_to_pipe(crtc);

			/*
			 * Make sure we don't wait for a scanline that will
			 * never occur
			 */
			y1 = (crtcbox.y1 <= box->y1) ? box->y1 - crtcbox.y1 : 0;
			y2 = (box->y2 <= crtcbox.y2) ?
			    box->y2 - crtcbox.y1 : crtcbox.y2 - crtcbox.y1;

			if (y1 == 0 && y2 == (crtcbox.y2 - crtcbox.y1))
			    full_height = TRUE;

			/*
			 * Pre-965 doesn't have SVBLANK, so we need a bit
			 * of extra time for the blitter to start up and
			 * do its job for a full height blit
			 */
			if (full_height && INTEL_INFO(intel)->gen < 040)
			    y2 -= 2;

			if (pipe == 0) {
				event = MI_WAIT_FOR_PIPEA_SCAN_LINE_WINDOW;
				load_scan_lines_pipe =
				    MI_LOAD_SCAN_LINES_DISPLAY_PIPEA;
				if (full_height && INTEL_INFO(intel)->gen >= 040)
				    event = MI_WAIT_FOR_PIPEA_SVBLANK;
			} else {
				event = MI_WAIT_FOR_PIPEB_SCAN_LINE_WINDOW;
				load_scan_lines_pipe =
				    MI_LOAD_SCAN_LINES_DISPLAY_PIPEB;
				if (full_height && INTEL_INFO(intel)->gen >= 040)
				    event = MI_WAIT_FOR_PIPEB_SVBLANK;
			}

			if (crtc->mode.Flags & V_INTERLACE) {
				/* DSL count field lines */
				y1 /= 2;
				y2 /= 2;
			}

			BEGIN_BATCH(5);
			/*
			 * The documentation says that the LOAD_SCAN_LINES
			 * command always comes in pairs. Don't ask me why.
			 */
			OUT_BATCH(MI_LOAD_SCAN_LINES_INCL |
				  load_scan_lines_pipe);
			OUT_BATCH((y1 << 16) | (y2-1));
			OUT_BATCH(MI_LOAD_SCAN_LINES_INCL |
				  load_scan_lines_pipe);
			OUT_BATCH((y1 << 16) | (y2-1));
			OUT_BATCH(MI_WAIT_FOR_EVENT | event);
			ADVANCE_BATCH();
		}
	}

	/* It's important that this copy gets submitted before the
	 * direct rendering client submits rendering for the next
	 * frame, but we don't actually need to submit right now.  The
	 * client will wait for the DRI2CopyRegion reply or the swap
	 * buffer event before rendering, and we'll hit the flush
	 * callback chain before those messages are sent.  We submit
	 * our batch buffers from the flush callback chain so we know
	 * that will happen before the client tries to render
	 * again. */

	gc->ops->CopyArea(src, dst, gc,
			  0, 0,
			  drawable->width, drawable->height,
			  0, 0);

	FreeScratchGC(gc);

	/* And make sure the WAIT_FOR_EVENT is queued before any
	 * modesetting/dpms operations on the pipe.
	 */
	intel_batch_submit(scrn);
}

static void
I830DRI2FallbackBlitSwap(DrawablePtr drawable,
			 DRI2BufferPtr dst,
			 DRI2BufferPtr src)
{
	BoxRec box;
	RegionRec region;

	box.x1 = 0;
	box.y1 = 0;
	box.x2 = drawable->width;
	box.y2 = drawable->height;
	REGION_INIT(pScreen, &region, &box, 0);

	I830DRI2CopyRegion(drawable, &region, dst, src);
}

#if DRI2INFOREC_VERSION >= 4

static void I830DRI2ReferenceBuffer(DRI2Buffer2Ptr buffer)
{
	if (buffer) {
		I830DRI2BufferPrivatePtr private = buffer->driverPrivate;
		private->refcnt++;
	}
}

static xf86CrtcPtr
I830DRI2DrawableCrtc(DrawablePtr pDraw)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	BoxRec box, crtcbox;
	xf86CrtcPtr crtc = NULL;

	box.x1 = pDraw->x;
	box.y1 = pDraw->y;
	box.x2 = box.x1 + pDraw->width;
	box.y2 = box.y1 + pDraw->height;

	if (pDraw->type != DRAWABLE_PIXMAP)
		crtc = intel_covering_crtc(pScrn, &box, NULL, &crtcbox);

	/* Make sure the CRTC is valid and this is the real front buffer */
	if (crtc != NULL && !crtc->rotatedData)
                return crtc;

	return NULL;
}

static RESTYPE	frame_event_client_type, frame_event_drawable_type;

struct i830_dri2_resource {
	XID id;
	RESTYPE type;
	struct list list;
};

static struct i830_dri2_resource *
get_resource(XID id, RESTYPE type)
{
	struct i830_dri2_resource *resource;
	void *ptr;

	ptr = NULL;
	dixLookupResourceByType(&ptr, id, type, NULL, DixWriteAccess);
	if (ptr)
		return ptr;

	resource = malloc(sizeof(*resource));
	if (resource == NULL)
		return NULL;

	if (!AddResource(id, type, resource)) {
		free(resource);
		return NULL;
	}

	resource->id = id;
	resource->type = type;
	list_init(&resource->list);
	return resource;
}

static int
i830_dri2_frame_event_client_gone(void *data, XID id)
{
	struct i830_dri2_resource *resource = data;

	while (!list_is_empty(&resource->list)) {
		DRI2FrameEventPtr info =
			list_first_entry(&resource->list,
					 DRI2FrameEventRec,
					 client_resource);

		list_del(&info->client_resource);
		info->client = NULL;
	}
	free(resource);

	return Success;
}

static int
i830_dri2_frame_event_drawable_gone(void *data, XID id)
{
	struct i830_dri2_resource *resource = data;

	while (!list_is_empty(&resource->list)) {
		DRI2FrameEventPtr info =
			list_first_entry(&resource->list,
					 DRI2FrameEventRec,
					 drawable_resource);

		list_del(&info->drawable_resource);
		info->drawable_id = None;
	}
	free(resource);

	return Success;
}

static Bool
i830_dri2_register_frame_event_resource_types(void)
{
	frame_event_client_type = CreateNewResourceType(i830_dri2_frame_event_client_gone, "Frame Event Client");
	if (!frame_event_client_type)
		return FALSE;

	frame_event_drawable_type = CreateNewResourceType(i830_dri2_frame_event_drawable_gone, "Frame Event Drawable");
	if (!frame_event_drawable_type)
		return FALSE;

	return TRUE;
}

static XID
get_client_id(ClientPtr client)
{
#if HAS_DIXREGISTERPRIVATEKEY
	XID *ptr = dixGetPrivateAddr(&client->devPrivates, &i830_client_key);
#else
	XID *ptr = dixLookupPrivate(&client->devPrivates, &i830_client_key);
#endif
	if (*ptr == 0)
		*ptr = FakeClientID(client->index);
	return *ptr;
}

/*
 * Hook this frame event into the server resource
 * database so we can clean it up if the drawable or
 * client exits while the swap is pending
 */
static Bool
i830_dri2_add_frame_event(DRI2FrameEventPtr info)
{
	struct i830_dri2_resource *resource;

	resource = get_resource(get_client_id(info->client),
				frame_event_client_type);
	if (resource == NULL)
		return FALSE;

	list_add(&info->client_resource, &resource->list);

	resource = get_resource(info->drawable_id, frame_event_drawable_type);
	if (resource == NULL) {
		list_del(&info->client_resource);
		return FALSE;
	}

	list_add(&info->drawable_resource, &resource->list);

	return TRUE;
}

static void
i830_dri2_del_frame_event(DRI2FrameEventPtr info)
{
	list_del(&info->client_resource);
	list_del(&info->drawable_resource);

	if (info->front)
		I830DRI2DestroyBuffer(NULL, info->front);
	if (info->back)
		I830DRI2DestroyBuffer(NULL, info->back);

	if (info->old_buffer) {
		/* Check that the old buffer still matches the front buffer
		 * in case a mode change occurred before we woke up.
		 */
		if (info->intel->back_buffer == NULL &&
		    info->old_width  == info->intel->scrn->virtualX &&
		    info->old_height == info->intel->scrn->virtualY &&
		    info->old_pitch  == info->intel->front_pitch &&
		    info->old_tiling == info->intel->front_tiling)
			info->intel->back_buffer = info->old_buffer;
		else
			dri_bo_unreference(info->old_buffer);
	}

	free(info);
}

static struct intel_uxa_pixmap *
intel_exchange_pixmap_buffers(struct intel_screen_private *intel, PixmapPtr front, PixmapPtr back)
{
	struct intel_uxa_pixmap *new_front = NULL, *new_back;
	RegionRec region;

	/* Post damage on the front buffer so that listeners, such
	 * as DisplayLink know take a copy and shove it over the USB.
	 * also for sw cursors.
	 */
	region.extents.x1 = region.extents.y1 = 0;
	region.extents.x2 = front->drawable.width;
	region.extents.y2 = front->drawable.height;
	region.data = NULL;
	DamageRegionAppend(&front->drawable, &region);

	new_front = intel_uxa_get_pixmap_private(back);
	new_back = intel_uxa_get_pixmap_private(front);
	intel_uxa_set_pixmap_private(front, new_front);
	intel_uxa_set_pixmap_private(back, new_back);
	new_front->busy = 1;
	new_back->busy = -1;

	DamageRegionProcessPending(&front->drawable);

	return new_front;
}

static void
I830DRI2ExchangeBuffers(struct intel_screen_private *intel, DRI2BufferPtr front, DRI2BufferPtr back)
{
	I830DRI2BufferPrivatePtr front_priv, back_priv;
	struct intel_uxa_pixmap *new_front;

	front_priv = front->driverPrivate;
	back_priv = back->driverPrivate;

	/* Swap BO names so DRI works */
	front->name = back->name;
	back->name = pixmap_flink(front_priv->pixmap);

	/* Swap pixmap bos */
	new_front = intel_exchange_pixmap_buffers(intel,
						  front_priv->pixmap,
						  back_priv->pixmap);
	dri_bo_unreference (intel->front_buffer);
	intel->front_buffer = new_front->bo;
	dri_bo_reference (intel->front_buffer);
}

static drm_intel_bo *get_pixmap_bo(I830DRI2BufferPrivatePtr priv)
{
	drm_intel_bo *bo = intel_get_pixmap_bo(priv->pixmap);
	assert(bo != NULL); /* guaranteed by construction of the DRI2 buffer */
	return bo;
}

static void
I830DRI2FlipComplete(uint64_t frame, uint64_t usec, void *pageflip_data)
{
        DRI2FrameEventPtr info = pageflip_data;

        I830DRI2FlipEventHandler((uint32_t) frame, usec / 1000000,
                                 usec % 1000000,
                                 info);
}

static void
I830DRI2FlipAbort(void *pageflip_data)
{
        DRI2FrameEventPtr info = pageflip_data;

        i830_dri2_del_frame_event(info);
}

static Bool
allocate_back_buffer(struct intel_screen_private *intel)
{
	drm_intel_bo *bo;
	int pitch;
	uint32_t tiling;

	if (intel->back_buffer)
		return TRUE;

	bo = intel_allocate_framebuffer(intel->scrn,
					intel->scrn->virtualX,
					intel->scrn->virtualY,
					intel->cpp,
					&pitch, &tiling);
	if (bo == NULL)
		return FALSE;

	if (pitch != intel->front_pitch || tiling != intel->front_tiling) {
		drm_intel_bo_unreference(bo);
		return FALSE;
	}

	intel->back_buffer = bo;
	return TRUE;
}

static Bool
can_exchange(DrawablePtr drawable, DRI2BufferPtr front, DRI2BufferPtr back)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(drawable->pScreen);
	struct intel_screen_private *intel = intel_get_screen_private(pScrn);
	I830DRI2BufferPrivatePtr front_priv = front->driverPrivate;
	I830DRI2BufferPrivatePtr back_priv = back->driverPrivate;
	PixmapPtr front_pixmap = front_priv->pixmap;
	PixmapPtr back_pixmap = back_priv->pixmap;
	struct intel_uxa_pixmap *front_intel = intel_uxa_get_pixmap_private(front_pixmap);
	struct intel_uxa_pixmap *back_intel = intel_uxa_get_pixmap_private(back_pixmap);

	if (!pScrn->vtSema)
		return FALSE;

	if (I830DRI2DrawableCrtc(drawable) == NULL)
		return FALSE;

	if (!DRI2CanFlip(drawable))
		return FALSE;

	if (intel->shadow_present)
		return FALSE;

	if (!intel->use_pageflipping)
		return FALSE;

	if (front_pixmap->drawable.width != back_pixmap->drawable.width)
		return FALSE;

	if (front_pixmap->drawable.height != back_pixmap->drawable.height)
		return FALSE;

	/* XXX should we be checking depth instead of bpp? */
#if 0
	if (front_pixmap->drawable.depth != back_pixmap->drawable.depth)
		return FALSE;
#else
	if (front_pixmap->drawable.bitsPerPixel != back_pixmap->drawable.bitsPerPixel)
		return FALSE;
#endif

	/* prevent an implicit tiling mode change */
	if (front_intel->tiling != back_intel->tiling)
		return FALSE;

	if (front_intel->pinned & ~(PIN_SCANOUT | PIN_DRI2))
		return FALSE;

	return TRUE;
}

static Bool
queue_flip(struct intel_screen_private *intel,
	   DrawablePtr draw,
	   DRI2FrameEventPtr info)
{
	xf86CrtcPtr crtc = I830DRI2DrawableCrtc(draw);
	I830DRI2BufferPrivatePtr priv = info->back->driverPrivate;
	drm_intel_bo *old_back = get_pixmap_bo(priv);

	if (crtc == NULL)
		return FALSE;

	if (!can_exchange(draw, info->front, info->back))
		return FALSE;

	if (!intel_do_pageflip(intel, old_back,
			       intel_crtc_to_index(crtc),
			       FALSE, info,
			       I830DRI2FlipComplete, I830DRI2FlipAbort))
		return FALSE;

#if DRI2INFOREC_VERSION >= 6
	if (intel->use_triple_buffer && allocate_back_buffer(intel)) {
		info->old_width  = intel->scrn->virtualX;
		info->old_height = intel->scrn->virtualY;
		info->old_pitch  = intel->front_pitch;
		info->old_tiling = intel->front_tiling;
		info->old_buffer = intel->front_buffer;
		dri_bo_reference(info->old_buffer);

		priv = info->front->driverPrivate;
		intel_set_pixmap_bo(priv->pixmap, intel->back_buffer);

		dri_bo_unreference(intel->back_buffer);
		intel->back_buffer = NULL;

		DRI2SwapLimit(draw, 2);
	} else
		DRI2SwapLimit(draw, 1);
#endif

	/* Then flip DRI2 pointers and update the screen pixmap */
	I830DRI2ExchangeBuffers(intel, info->front, info->back);
	return TRUE;
}

static Bool
queue_swap(struct intel_screen_private *intel,
	   DrawablePtr draw,
	   DRI2FrameEventPtr info)
{
	xf86CrtcPtr crtc = I830DRI2DrawableCrtc(draw);
	drmVBlank vbl;

	if (crtc == NULL)
		return FALSE;

	vbl.request.type =
		DRM_VBLANK_RELATIVE |
		DRM_VBLANK_EVENT |
		crtc_select(intel_crtc_to_index(crtc));
	vbl.request.sequence = 1;
	vbl.request.signal =
		intel_drm_queue_alloc(intel->scrn, crtc, info,
				      intel_dri2_vblank_handler,
				      intel_dri2_vblank_abort);
	if (vbl.request.signal == 0)
		return FALSE;

	info->type = DRI2_SWAP;
	if (drmWaitVBlank(intel->drmSubFD, &vbl)) {
		intel_drm_abort_seq(intel->scrn, vbl.request.signal);
		return FALSE;
	}

	return TRUE;
}

static void I830DRI2FrameEventHandler(unsigned int frame,
				      unsigned int tv_sec,
				      unsigned int tv_usec,
				      DRI2FrameEventPtr swap_info)
{
	intel_screen_private *intel = swap_info->intel;
	DrawablePtr drawable;
	int status;

	if (!swap_info->drawable_id)
		status = BadDrawable;
	else
		status = dixLookupDrawable(&drawable, swap_info->drawable_id, serverClient,
					   M_ANY, DixWriteAccess);
	if (status != Success) {
		i830_dri2_del_frame_event(swap_info);
		return;
	}

	switch (swap_info->type) {
	case DRI2_FLIP:
		/* If we can still flip... */
		if (!queue_flip(intel, drawable, swap_info) &&
		    !queue_swap(intel, drawable, swap_info)) {
		case DRI2_SWAP:
			I830DRI2FallbackBlitSwap(drawable,
						 swap_info->front, swap_info->back);
			DRI2SwapComplete(swap_info->client, drawable, frame, tv_sec, tv_usec,
					 DRI2_BLIT_COMPLETE,
					 swap_info->client ? swap_info->event_complete : NULL,
					 swap_info->event_data);
			break;
		}
		return;

	case DRI2_WAITMSC:
		if (swap_info->client)
			DRI2WaitMSCComplete(swap_info->client, drawable,
					    frame, tv_sec, tv_usec);
		break;
	default:
		xf86DrvMsg(intel->scrn->scrnIndex, X_WARNING,
			   "%s: unknown vblank event received\n", __func__);
		/* Unknown type */
		break;
	}

	i830_dri2_del_frame_event(swap_info);
}

static void I830DRI2FlipEventHandler(unsigned int frame,
				     unsigned int tv_sec,
				     unsigned int tv_usec,
				     DRI2FrameEventPtr flip_info)
{
	struct intel_screen_private *intel = flip_info->intel;
	DrawablePtr drawable;

	drawable = NULL;
	if (flip_info->drawable_id)
		dixLookupDrawable(&drawable, flip_info->drawable_id, serverClient,
				  M_ANY, DixWriteAccess);


	/* We assume our flips arrive in order, so we don't check the frame */
	switch (flip_info->type) {
	case DRI2_FLIP:
	case DRI2_SWAP:
		if (!drawable)
			break;

		/* Check for too small vblank count of pageflip completion, taking wraparound
		 * into account. This usually means some defective kms pageflip completion,
		 * causing wrong (msc, ust) return values and possible visual corruption.
		 */
		if ((frame < flip_info->frame) && (flip_info->frame - frame < 5)) {
			static int limit = 5;

			/* XXX we are currently hitting this path with older
			 * kernels, so make it quieter.
			 */
			if (limit) {
				xf86DrvMsg(intel->scrn->scrnIndex, X_WARNING,
					   "%s: Pageflip completion has impossible msc %d < target_msc %d\n",
					   __func__, frame, flip_info->frame);
				limit--;
			}

			/* All-0 values signal timestamping failure. */
			frame = tv_sec = tv_usec = 0;
		}

		DRI2SwapComplete(flip_info->client, drawable, frame, tv_sec, tv_usec,
				 DRI2_FLIP_COMPLETE, flip_info->client ? flip_info->event_complete : NULL,
				 flip_info->event_data);
		break;

	default:
		xf86DrvMsg(intel->scrn->scrnIndex, X_WARNING,
			   "%s: unknown vblank event received\n", __func__);
		/* Unknown type */
		break;
	}

	i830_dri2_del_frame_event(flip_info);
}

/*
 * ScheduleSwap is responsible for requesting a DRM vblank event for the
 * appropriate frame.
 *
 * In the case of a blit (e.g. for a windowed swap) or buffer exchange,
 * the vblank requested can simply be the last queued swap frame + the swap
 * interval for the drawable.
 *
 * In the case of a page flip, we request an event for the last queued swap
 * frame + swap interval - 1, since we'll need to queue the flip for the frame
 * immediately following the received event.
 *
 * The client will be blocked if it tries to perform further GL commands
 * after queueing a swap, though in the Intel case after queueing a flip, the
 * client is free to queue more commands; they'll block in the kernel if
 * they access buffers busy with the flip.
 *
 * When the swap is complete, the driver should call into the server so it
 * can send any swap complete events that have been requested.
 */
static int
I830DRI2ScheduleSwap(ClientPtr client, DrawablePtr draw, DRI2BufferPtr front,
		     DRI2BufferPtr back, CARD64 *target_msc, CARD64 divisor,
		     CARD64 remainder, DRI2SwapEventPtr func, void *data)
{
	ScreenPtr screen = draw->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	drmVBlank vbl;
	int ret;
        xf86CrtcPtr crtc = I830DRI2DrawableCrtc(draw);
        int index = crtc ? intel_crtc_to_index(crtc) : -1;
        int flip = 0;
	DRI2FrameEventPtr swap_info = NULL;
	uint64_t current_msc, current_ust;
        uint64_t request_msc;
        uint32_t seq;

	/* Drawable not displayed... just complete the swap */
	if (index == -1)
	    goto blit_fallback;

	swap_info = calloc(1, sizeof(DRI2FrameEventRec));
	if (!swap_info)
	    goto blit_fallback;

	swap_info->intel = intel;
	swap_info->drawable_id = draw->id;
	swap_info->client = client;
	swap_info->event_complete = func;
	swap_info->event_data = data;
	swap_info->front = front;
	swap_info->back = back;
	swap_info->type = DRI2_SWAP;

	if (!i830_dri2_add_frame_event(swap_info)) {
	    free(swap_info);
	    swap_info = NULL;
	    goto blit_fallback;
	}

	I830DRI2ReferenceBuffer(front);
	I830DRI2ReferenceBuffer(back);

        ret = intel_get_crtc_msc_ust(scrn, crtc, &current_msc, &current_ust);
	if (ret)
	    goto blit_fallback;

	/*
	 * If we can, schedule the flip directly from here rather
	 * than waiting for an event from the kernel for the current
	 * (or a past) MSC.
	 */
	if (divisor == 0 &&
	    current_msc >= *target_msc &&
	    queue_flip(intel, draw, swap_info))
		return TRUE;

	if (can_exchange(draw, front, back)) {
		swap_info->type = DRI2_FLIP;
		/* Flips need to be submitted one frame before */
		if (*target_msc > 0)
			--*target_msc;
		flip = 1;
	}

#if DRI2INFOREC_VERSION >= 6
	DRI2SwapLimit(draw, 1);
#endif

	/*
	 * If divisor is zero, or current_msc is smaller than target_msc
	 * we just need to make sure target_msc passes before initiating
	 * the swap.
	 */
	if (divisor == 0 || current_msc < *target_msc) {
		vbl.request.type =
			DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | crtc_select(index);

		/* If non-pageflipping, but blitting/exchanging, we need to use
		 * DRM_VBLANK_NEXTONMISS to avoid unreliable timestamping later
		 * on.
		 */
		if (flip == 0)
			vbl.request.type |= DRM_VBLANK_NEXTONMISS;

		/* If target_msc already reached or passed, set it to
		 * current_msc to ensure we return a reasonable value back
		 * to the caller. This makes swap_interval logic more robust.
		 */
		if (current_msc > *target_msc)
			*target_msc = current_msc;

                seq = intel_drm_queue_alloc(scrn, crtc, swap_info, intel_dri2_vblank_handler, intel_dri2_vblank_abort);
                if (!seq)
                        goto blit_fallback;

		vbl.request.sequence = intel_crtc_msc_to_sequence(scrn, crtc, *target_msc);
		vbl.request.signal = seq;

		ret = drmWaitVBlank(intel->drmSubFD, &vbl);
		if (ret) {
			xf86DrvMsg(scrn->scrnIndex, X_WARNING,
				   "divisor 0 get vblank counter failed: %s\n",
				   strerror(errno));
			intel_drm_abort_seq(intel->scrn, seq);
			swap_info = NULL;
			goto blit_fallback;
		}

                *target_msc = intel_sequence_to_crtc_msc(crtc, vbl.reply.sequence + flip);
		swap_info->frame = *target_msc;

		return TRUE;
	}

	/*
	 * If we get here, target_msc has already passed or we don't have one,
	 * and we need to queue an event that will satisfy the divisor/remainder
	 * equation.
	 */
	vbl.request.type =
		DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | crtc_select(index);
	if (flip == 0)
		vbl.request.type |= DRM_VBLANK_NEXTONMISS;

        request_msc = current_msc - (current_msc % divisor) +
                remainder;

	/*
	 * If the calculated deadline vbl.request.sequence is smaller than
	 * or equal to current_msc, it means we've passed the last point
	 * when effective onset frame seq could satisfy
	 * seq % divisor == remainder, so we need to wait for the next time
	 * this will happen.

	 * This comparison takes the 1 frame swap delay in pageflipping mode
	 * into account, as well as a potential DRM_VBLANK_NEXTONMISS delay
	 * if we are blitting/exchanging instead of flipping.
	 */
	if (request_msc <= current_msc)
		request_msc += divisor;

        seq = intel_drm_queue_alloc(scrn, crtc, swap_info, intel_dri2_vblank_handler, intel_dri2_vblank_abort);
        if (!seq)
                goto blit_fallback;

	/* Account for 1 frame extra pageflip delay if flip > 0 */
        vbl.request.sequence = intel_crtc_msc_to_sequence(scrn, crtc, request_msc) - flip;
	vbl.request.signal = seq;

	ret = drmWaitVBlank(intel->drmSubFD, &vbl);
	if (ret) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "final get vblank counter failed: %s\n",
			   strerror(errno));
		goto blit_fallback;
	}

	/* Adjust returned value for 1 fame pageflip offset of flip > 0 */
	*target_msc = intel_sequence_to_crtc_msc(crtc, vbl.reply.sequence + flip);
	swap_info->frame = *target_msc;

	return TRUE;

blit_fallback:
	I830DRI2FallbackBlitSwap(draw, front, back);
	DRI2SwapComplete(client, draw, 0, 0, 0, DRI2_BLIT_COMPLETE, func, data);
	if (swap_info)
	    i830_dri2_del_frame_event(swap_info);
	*target_msc = 0; /* offscreen, so zero out target vblank count */
	return TRUE;
}

static uint64_t gettime_us(void)
{
	struct timespec tv;

	if (clock_gettime(CLOCK_MONOTONIC, &tv))
		return 0;

	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

/*
 * Get current frame count and frame count timestamp, based on drawable's
 * crtc.
 */
static int
I830DRI2GetMSC(DrawablePtr draw, CARD64 *ust, CARD64 *msc)
{
	ScreenPtr screen = draw->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	int ret;
        xf86CrtcPtr crtc = I830DRI2DrawableCrtc(draw);

	/* Drawable not displayed, make up a *monotonic* value */
	if (crtc == NULL) {
fail:
		*ust = gettime_us();
		*msc = 0;
		return TRUE;
	}

        ret = intel_get_crtc_msc_ust(scrn, crtc, msc, ust);
	if (ret) {
		static int limit = 5;
		if (limit) {
			xf86DrvMsg(scrn->scrnIndex, X_WARNING,
				   "%s:%d get vblank counter failed: %s\n",
				   __FUNCTION__, __LINE__,
				   strerror(errno));
			limit--;
		}
		goto fail;
	}

	return TRUE;
}

/*
 * Request a DRM event when the requested conditions will be satisfied.
 *
 * We need to handle the event and ask the server to wake up the client when
 * we receive it.
 */
static int
I830DRI2ScheduleWaitMSC(ClientPtr client, DrawablePtr draw, CARD64 target_msc,
			CARD64 divisor, CARD64 remainder)
{
	ScreenPtr screen = draw->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DRI2FrameEventPtr wait_info;
	drmVBlank vbl;
	int ret;
        xf86CrtcPtr crtc = I830DRI2DrawableCrtc(draw);
        int index = crtc ? intel_crtc_to_index(crtc) : -1;
	CARD64 current_msc, current_ust, request_msc;
        uint32_t seq;

	/* Drawable not visible, return immediately */
	if (index == -1)
		goto out_complete;

	wait_info = calloc(1, sizeof(DRI2FrameEventRec));
	if (!wait_info)
		goto out_complete;

	wait_info->intel = intel;
	wait_info->drawable_id = draw->id;
	wait_info->client = client;
	wait_info->type = DRI2_WAITMSC;

	if (!i830_dri2_add_frame_event(wait_info)) {
	    free(wait_info);
	    goto out_complete;
	}

	/* Get current count */
        ret = intel_get_crtc_msc_ust(scrn, crtc, &current_msc, &current_ust);
	if (ret)
	    goto out_free;

	/*
	 * If divisor is zero, or current_msc is smaller than target_msc,
	 * we just need to make sure target_msc passes  before waking up the
	 * client.
	 */
	if (divisor == 0 || current_msc < target_msc) {
		/* If target_msc already reached or passed, set it to
		 * current_msc to ensure we return a reasonable value back
		 * to the caller. This keeps the client from continually
		 * sending us MSC targets from the past by forcibly updating
		 * their count on this call.
		 */
                seq = intel_drm_queue_alloc(scrn, crtc, wait_info, intel_dri2_vblank_handler, intel_dri2_vblank_abort);
                if (!seq)
                        goto out_free;

		if (current_msc >= target_msc)
			target_msc = current_msc;
		vbl.request.type =
			DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | crtc_select(index);
		vbl.request.sequence = intel_crtc_msc_to_sequence(scrn, crtc, target_msc);
		vbl.request.signal = seq;

		ret = drmWaitVBlank(intel->drmSubFD, &vbl);
		if (ret) {
			static int limit = 5;
			if (limit) {
				xf86DrvMsg(scrn->scrnIndex, X_WARNING,
					   "%s:%d get vblank counter failed: %s\n",
					   __FUNCTION__, __LINE__,
					   strerror(errno));
				limit--;
			}
			intel_drm_abort_seq(intel->scrn, seq);
			goto out_complete;
		}

		wait_info->frame = intel_sequence_to_crtc_msc(crtc, vbl.reply.sequence);
		DRI2BlockClient(client, draw);
		return TRUE;
	}

	/*
	 * If we get here, target_msc has already passed or we don't have one,
	 * so we queue an event that will satisfy the divisor/remainder equation.
	 */
	vbl.request.type =
		DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | crtc_select(index);

        request_msc = current_msc - (current_msc % divisor) +
                remainder;
	/*
	 * If calculated remainder is larger than requested remainder,
	 * it means we've passed the last point where
	 * seq % divisor == remainder, so we need to wait for the next time
	 * that will happen.
	 */
	if ((current_msc % divisor) >= remainder)
                request_msc += divisor;

        seq = intel_drm_queue_alloc(scrn, crtc, wait_info, intel_dri2_vblank_handler, intel_dri2_vblank_abort);
        if (!seq)
                goto out_free;

	vbl.request.sequence = intel_crtc_msc_to_sequence(scrn, crtc, request_msc);
	vbl.request.signal = seq;

	ret = drmWaitVBlank(intel->drmSubFD, &vbl);
	if (ret) {
		static int limit = 5;
		if (limit) {
			xf86DrvMsg(scrn->scrnIndex, X_WARNING,
				   "%s:%d get vblank counter failed: %s\n",
				   __FUNCTION__, __LINE__,
				   strerror(errno));
			limit--;
		}
		intel_drm_abort_seq(intel->scrn, seq);
		goto out_complete;
	}

	wait_info->frame = intel_sequence_to_crtc_msc(crtc, vbl.reply.sequence);
	DRI2BlockClient(client, draw);

	return TRUE;

out_free:
	i830_dri2_del_frame_event(wait_info);
out_complete:
	DRI2WaitMSCComplete(client, draw, target_msc, 0, 0);
	return TRUE;
}

static int dri2_server_generation;
#endif

static int has_i830_dri(void)
{
	return access(DRI_DRIVER_PATH "/i830_dri.so", R_OK) == 0;
}

static int
namecmp(const char *s1, const char *s2)
{
	char c1, c2;

	if (!s1 || *s1 == 0) {
		if (!s2 || *s2 == 0)
			return 0;
		else
			return 1;
	}

	while (*s1 == '_' || *s1 == ' ' || *s1 == '\t')
		s1++;

	while (*s2 == '_' || *s2 == ' ' || *s2 == '\t')
		s2++;

	c1 = isupper(*s1) ? tolower(*s1) : *s1;
	c2 = isupper(*s2) ? tolower(*s2) : *s2;
	while (c1 == c2) {
		if (c1 == '\0')
			return 0;

		s1++;
		while (*s1 == '_' || *s1 == ' ' || *s1 == '\t')
			s1++;

		s2++;
		while (*s2 == '_' || *s2 == ' ' || *s2 == '\t')
			s2++;

		c1 = isupper(*s1) ? tolower(*s1) : *s1;
		c2 = isupper(*s2) ? tolower(*s2) : *s2;
	}

	return c1 - c2;
}

static Bool is_level(const char **str)
{
	const char *s = *str;
	char *end;
	unsigned val;

	if (s == NULL || *s == '\0')
		return TRUE;

	if (namecmp(s, "on") == 0)
		return TRUE;
	if (namecmp(s, "true") == 0)
		return TRUE;
	if (namecmp(s, "yes") == 0)
		return TRUE;

	if (namecmp(s, "0") == 0)
		return TRUE;
	if (namecmp(s, "off") == 0)
		return TRUE;
	if (namecmp(s, "false") == 0)
		return TRUE;
	if (namecmp(s, "no") == 0)
		return TRUE;

	val = strtoul(s, &end, 0);
	if (val && *end == '\0')
		return TRUE;
	if (val && *end == ':')
		*str = end + 1;
	return FALSE;
}

static const char *options_get_dri(intel_screen_private *intel)
{
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,7,99,901,0)
	return xf86GetOptValString(intel->Options, OPTION_DRI);
#else
	return NULL;
#endif
}

static const char *dri_driver_name(intel_screen_private *intel)
{
	const char *s = options_get_dri(intel);

	if (is_level(&s)) {
		if (INTEL_INFO(intel)->gen < 030)
			return has_i830_dri() ? "i830" : "i915";
		else if (INTEL_INFO(intel)->gen < 040)
			return "i915";
		else
			return "i965";
	}

	return s;
}

Bool I830DRI2ScreenInit(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DRI2InfoRec info;
	int dri2scr_major = 1;
	int dri2scr_minor = 0;
#if DRI2INFOREC_VERSION >= 4
	const char *driverNames[2];
#endif

	if (intel->force_fallback) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "cannot enable DRI2 whilst forcing software fallbacks\n");
		return FALSE;
	}

	if (xf86LoaderCheckSymbol("DRI2Version"))
		DRI2Version(&dri2scr_major, &dri2scr_minor);

	if (dri2scr_minor < 1) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "DRI2 requires DRI2 module version 1.1.0 or later\n");
		return FALSE;
	}

#if HAS_DIXREGISTERPRIVATEKEY
	if (!dixRegisterPrivateKey(&i830_client_key, PRIVATE_CLIENT, sizeof(XID)))
		return FALSE;
#else
	if (!dixRequestPrivate(&i830_client_key, sizeof(XID)))
		return FALSE;
#endif


#if DRI2INFOREC_VERSION >= 4
	if (serverGeneration != dri2_server_generation) {
	    dri2_server_generation = serverGeneration;
	    if (!i830_dri2_register_frame_event_resource_types()) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "Cannot register DRI2 frame event resources\n");
		return FALSE;
	    }
	}
#endif

	intel->deviceName = drmGetDeviceNameFromFd(intel->drmSubFD);
	memset(&info, '\0', sizeof(info));
	info.fd = intel->drmSubFD;
	info.driverName = dri_driver_name(intel);
	info.deviceName = intel->deviceName;

#if DRI2INFOREC_VERSION == 1
	info.version = 1;
	info.CreateBuffers = I830DRI2CreateBuffers;
	info.DestroyBuffers = I830DRI2DestroyBuffers;
#elif DRI2INFOREC_VERSION == 2
	/* The ABI between 2 and 3 was broken so we could get rid of
	 * the multi-buffer alloc functions.  Make sure we indicate the
	 * right version so DRI2 can reject us if it's version 3 or above. */
	info.version = 2;
	info.CreateBuffer = I830DRI2CreateBuffer;
	info.DestroyBuffer = I830DRI2DestroyBuffer;
#else
	info.version = 3;
	info.CreateBuffer = I830DRI2CreateBuffer;
	info.DestroyBuffer = I830DRI2DestroyBuffer;
#endif

	info.CopyRegion = I830DRI2CopyRegion;
#if DRI2INFOREC_VERSION >= 4
	info.version = 4;
	info.ScheduleSwap = I830DRI2ScheduleSwap;
	info.GetMSC = I830DRI2GetMSC;
	info.ScheduleWaitMSC = I830DRI2ScheduleWaitMSC;
	info.numDrivers = 2;
	info.driverNames = driverNames;
	driverNames[0] = info.driverName;
	driverNames[1] = "va_gl";
#endif

	return DRI2ScreenInit(screen, &info);
}

void I830DRI2CloseScreen(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);

	DRI2CloseScreen(screen);
	drmFree(intel->deviceName);
}
