/*
 * Copyright (c) 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include "config.h"

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "sna.h"
#include "intel_dri3_syncobj.h"

#include <xf86.h>
#include <dri3.h>
#include <misyncshm.h>
#include <misyncstr.h>

static DevPrivateKeyRec sna_sync_fence_private_key;
struct sna_sync_fence {
	SyncFenceSetTriggeredFunc set_triggered;
};

static inline struct sna_sync_fence *sna_sync_fence(SyncFence *fence)
{
	return dixLookupPrivate(&fence->devPrivates, &sna_sync_fence_private_key);
}

static inline void mark_dri3_pixmap(struct sna *sna, struct sna_pixmap *priv, struct kgem_bo *bo)
{
	bo->flush = true;
	if (bo->exec)
		sna->kgem.flush = 1;
	if (bo == priv->gpu_bo)
		priv->flush |= FLUSH_READ | FLUSH_WRITE;
	else
		priv->shm = true;

	sna_watch_flush(sna, 1);

	kgem_bo_submit(&sna->kgem, bo);
	kgem_bo_unclean(&sna->kgem, bo);
}

static void sna_sync_flush(struct sna *sna, struct sna_pixmap *priv)
{
	struct kgem_bo *bo = NULL;

	DBG(("%s(pixmap=%ld)\n", __FUNCTION__, priv->pixmap->drawable.serialNumber));
	assert(priv);

	if (priv->pinned & PIN_DRI3) {
		assert(priv->gpu_bo);
		assert(priv->pinned & PIN_DRI3);
		DBG(("%s: flushing prime GPU bo, handle=%ld\n", __FUNCTION__, priv->gpu_bo->handle));
		if (sna_pixmap_move_to_gpu(priv->pixmap, MOVE_READ | MOVE_WRITE | MOVE_ASYNC_HINT | __MOVE_FORCE)) {
			sna_damage_all(&priv->gpu_damage, priv->pixmap);
			bo = priv->gpu_bo;
		}
	} else {
		assert(priv->cpu_bo);
		assert(IS_STATIC_PTR(priv->ptr));
		DBG(("%s: flushing prime CPU bo, handle=%ld\n", __FUNCTION__, priv->cpu_bo->handle));
		if (sna_pixmap_move_to_cpu(priv->pixmap, MOVE_READ | MOVE_WRITE | MOVE_ASYNC_HINT))
			bo = priv->cpu_bo;
	}

	if (bo != NULL) {
		kgem_bo_submit(&sna->kgem, bo);
		kgem_bo_unclean(&sna->kgem, bo);
	}
}

static void
sna_sync_fence_set_triggered(SyncFence *fence)
{
	struct sna *sna = to_sna_from_screen(fence->pScreen);
	struct sna_sync_fence *sna_fence = sna_sync_fence(fence);

	DBG(("%s()\n", __FUNCTION__));
	sna_accel_flush(sna);

	fence->funcs.SetTriggered = sna_fence->set_triggered;
	sna_fence->set_triggered(fence);
	sna_fence->set_triggered = fence->funcs.SetTriggered;
	fence->funcs.SetTriggered = sna_sync_fence_set_triggered;
}

static void
sna_sync_create_fence(ScreenPtr screen, SyncFence *fence, Bool initially_triggered)
{
	struct sna *sna = to_sna_from_screen(screen);
	SyncScreenFuncsPtr funcs = miSyncGetScreenFuncs(screen);

	DBG(("%s()\n", __FUNCTION__));

	funcs->CreateFence = sna->dri3.create_fence;
	sna->dri3.create_fence(screen, fence, initially_triggered);
	sna->dri3.create_fence = funcs->CreateFence;
	funcs->CreateFence = sna_sync_create_fence;

	sna_sync_fence(fence)->set_triggered = fence->funcs.SetTriggered;
	fence->funcs.SetTriggered = sna_sync_fence_set_triggered;
}

static bool
sna_sync_open(struct sna *sna, ScreenPtr screen)
{
	SyncScreenFuncsPtr funcs;

	DBG(("%s()\n", __FUNCTION__));

	if (!miSyncShmScreenInit(screen))
		return false;

	if (!dixPrivateKeyRegistered(&sna_sync_fence_private_key)) {
		if (!dixRegisterPrivateKey(&sna_sync_fence_private_key,
					   PRIVATE_SYNC_FENCE,
					   sizeof(struct sna_sync_fence)))
			return false;
	}

	funcs = miSyncGetScreenFuncs(screen);
	sna->dri3.create_fence = funcs->CreateFence;
	funcs->CreateFence = sna_sync_create_fence;

	return true;
}

static int sna_dri3_open_device(ScreenPtr screen,
				RRProviderPtr provider,
				int *out)
{
	int fd;

	DBG(("%s()\n", __FUNCTION__));
	fd = intel_get_client_fd(to_sna_from_screen(screen)->dev);
	if (fd < 0)
		return -fd;

	*out = fd;
	return Success;
}

/* tiling < 0: implicit, as the kernel reports it; else from a modifier */
static PixmapPtr __sna_dri3_pixmap_from_fd(ScreenPtr screen,
					   int fd,
					   CARD16 width,
					   CARD16 height,
					   CARD16 stride,
					   CARD8 depth,
					   CARD8 bpp,
					   int tiling)
{
	struct sna *sna = to_sna_from_screen(screen);
	PixmapPtr pixmap;
	struct sna_pixmap *priv;
	struct kgem_bo *bo;

	DBG(("%s(fd=%d, width=%d, height=%d, stride=%d, depth=%d, bpp=%d, tiling=%d)\n",
	     __FUNCTION__, fd, width, height, stride, depth, bpp, tiling));
	if (width > INT16_MAX || height > INT16_MAX)
		return NULL;

	if ((uint32_t)width * bpp > (uint32_t)stride * 8)
		return NULL;

	if (depth < 8)
		return NULL;

	switch (bpp) {
	case 8:
	case 16:
	case 32:
		break;
	default:
		return NULL;
	}

	bo = kgem_create_for_prime(&sna->kgem, fd, (uint32_t)stride * height,
				   tiling);
	if (bo == NULL)
		return NULL;

	/* Check for a duplicate */
	list_for_each_entry(priv, &sna->dri3.pixmaps, cow_list) {
		int other_stride = 0, other_tiling = 0;
		if (bo->snoop) {
			assert(priv->cpu_bo);
			assert(IS_STATIC_PTR(priv->ptr));
			if (bo->handle == priv->cpu_bo->handle) {
				other_stride = priv->cpu_bo->pitch;
				other_tiling = priv->cpu_bo->tiling;
			}
		} else  {
			assert(priv->gpu_bo);
			assert(priv->pinned & PIN_DRI3);
			if (bo->handle == priv->gpu_bo->handle) {
				other_stride = priv->gpu_bo->pitch;
				other_tiling = priv->gpu_bo->tiling;
			}
		}
		if (other_stride) {
			pixmap = priv->pixmap;
			DBG(("%s: imported fd matches existing DRI3 pixmap=%ld\n", __FUNCTION__, pixmap->drawable.serialNumber));
			bo->handle = 0; /* fudge to prevent gem_close */
			kgem_bo_destroy(&sna->kgem, bo);
			if (width != pixmap->drawable.width ||
			    height != pixmap->drawable.height ||
			    depth != pixmap->drawable.depth ||
			    bpp != pixmap->drawable.bitsPerPixel ||
			    stride != other_stride ||
			    (tiling >= 0 && tiling != other_tiling)) {
				DBG(("%s: imported fd mismatches existing DRI3 pixmap (width=%d, height=%d, depth=%d, bpp=%d, stride=%d, tiling=%d)\n", __FUNCTION__,
				     pixmap->drawable.width,
				     pixmap->drawable.height,
				     pixmap->drawable.depth,
				     pixmap->drawable.bitsPerPixel,
				     other_stride, other_tiling));
				return NULL;
			}
			sna_sync_flush(sna, priv);
			pixmap->refcnt++;
			return pixmap;
		}
	}

	if (tiling >= 0) {
		/* snooped (CPU) buffers can't be tiled */
		if (bo->snoop && tiling != I915_TILING_NONE)
			goto free_bo;
		if (!kgem_bo_import_tiling(&sna->kgem, bo, tiling, stride)) {
			DBG(("%s: unable to apply tiling=%d to handle=%d\n",
			     __FUNCTION__, tiling, bo->handle));
			goto free_bo;
		}
	}

	if (!kgem_check_surface_size(&sna->kgem,
				     width, height, bpp,
				     bo->tiling, stride, kgem_bo_size(bo))) {
		DBG(("%s: client supplied pitch=%d, size=%d too small for %dx%d surface\n",
		     __FUNCTION__, stride, kgem_bo_size(bo), width, height));
		goto free_bo;
	}

	pixmap = sna_pixmap_create_unattached(screen, 0, 0, depth);
	if (pixmap == NullPixmap)
		goto free_bo;

	if (!screen->ModifyPixmapHeader(pixmap, width, height,
					depth, bpp, stride, NULL))
		goto free_pixmap;

	priv = sna_pixmap_attach_to_bo(pixmap, bo);
	if (priv == NULL)
		goto free_pixmap;

	bo->pitch = stride;
	priv->stride = stride;

	if (bo->snoop) {
		assert(priv->cpu_bo == bo);
		pixmap->devPrivate.ptr = kgem_bo_map__cpu(&sna->kgem, priv->cpu_bo);
		if (pixmap->devPrivate.ptr == NULL)
			goto free_pixmap;

		pixmap->devKind = stride;
		priv->ptr = MAKE_STATIC_PTR(pixmap->devPrivate.ptr);
	} else {
		assert(priv->gpu_bo == bo);
		priv->create = kgem_can_create_2d(&sna->kgem,
						  width, height, depth);
		priv->pinned |= PIN_DRI3;
	}
	list_add(&priv->cow_list, &sna->dri3.pixmaps);

	mark_dri3_pixmap(sna, priv, bo);

	return pixmap;

free_pixmap:
	dixDestroyPixmap(pixmap, 0);
free_bo:
	kgem_bo_destroy(&sna->kgem, bo);
	return NULL;
}

static PixmapPtr sna_dri3_pixmap_from_fd(ScreenPtr screen,
					 int fd,
					 CARD16 width,
					 CARD16 height,
					 CARD16 stride,
					 CARD8 depth,
					 CARD8 bpp)
{
	return __sna_dri3_pixmap_from_fd(screen, fd, width, height, stride,
					 depth, bpp, -1);
}

static int sna_dri3_modifier_to_tiling(struct sna *sna, uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return I915_TILING_NONE;
	case I915_FORMAT_MOD_X_TILED:
		return I915_TILING_X;
	case I915_FORMAT_MOD_Y_TILED:
		if (sna->kgem.can_blt_y && sna->kgem.can_render_y)
			return I915_TILING_Y;
		return -1;
	default:
		return -1;
	}
}

static uint64_t sna_dri3_tiling_to_modifier(int tiling)
{
	switch (tiling) {
	case I915_TILING_NONE:
		return DRM_FORMAT_MOD_LINEAR;
	case I915_TILING_X:
		return I915_FORMAT_MOD_X_TILED;
	case I915_TILING_Y:
		return I915_FORMAT_MOD_Y_TILED;
	default:
		return DRM_FORMAT_MOD_INVALID;
	}
}

static PixmapPtr sna_dri3_pixmap_from_fds(ScreenPtr screen,
					  CARD8 num_fds,
					  const int *fds,
					  CARD16 width,
					  CARD16 height,
					  const CARD32 *strides,
					  const CARD32 *offsets,
					  CARD8 depth,
					  CARD8 bpp,
					  CARD64 modifier)
{
	struct sna *sna = to_sna_from_screen(screen);
	int tiling = -1;

	DBG(("%s(num_fds=%d, modifier=%llx)\n",
	     __FUNCTION__, num_fds, (long long)modifier));
	/* single plane RGB only; the old protocol's 16-bit stride limit stays */
	if (num_fds != 1 || offsets[0] != 0 || strides[0] > UINT16_MAX)
		return NULL;

	if (modifier != DRM_FORMAT_MOD_INVALID) {
		tiling = sna_dri3_modifier_to_tiling(sna, modifier);
		if (tiling < 0)
			return NULL;
	}

	return __sna_dri3_pixmap_from_fd(screen, fds[0], width, height,
					 strides[0], depth, bpp, tiling);
}

/* explicit: the client learns the tiling from a modifier, so tiled
 * buffers need no detiling for GPUs without fences */
static int __sna_dri3_fd_from_pixmap(ScreenPtr screen,
				     PixmapPtr pixmap,
				     uint32_t *stride,
				     uint32_t *size,
				     int *tiling,
				     bool explicit)
{
	struct sna *sna = to_sna_from_screen(screen);
	struct sna_pixmap *priv;
	struct kgem_bo *bo = NULL;
	int fd;

	DBG(("%s(pixmap=%ld, width=%d, height=%d)\n", __FUNCTION__,
	     pixmap->drawable.serialNumber, pixmap->drawable.width, pixmap->drawable.height));
	if (pixmap == sna->front && sna->flags & SNA_TEAR_FREE) {
		DBG(("%s: DRI3 protocol cannot support TearFree frontbuffers\n", __FUNCTION__));
		return -1;
	}

	priv = sna_pixmap(pixmap);
	if (priv && IS_STATIC_PTR(priv->ptr) && priv->cpu_bo) {
		if (sna_pixmap_move_to_cpu(pixmap, MOVE_READ | MOVE_WRITE | MOVE_ASYNC_HINT))
			bo = priv->cpu_bo;
	} else {
		priv = sna_pixmap_move_to_gpu(pixmap, MOVE_READ | MOVE_WRITE | MOVE_ASYNC_HINT | __MOVE_FORCE | __MOVE_DRI);
		if (priv != NULL) {
			sna_damage_all(&priv->gpu_damage, pixmap);
			bo = priv->gpu_bo;
		}
	}
	if (bo == NULL) {
		DBG(("%s: pixmap not supported by GPU\n", __FUNCTION__));
		return -1;
	}
	assert(priv != NULL);

	if (bo->pitch > UINT16_MAX) {
		DBG(("%s: pixmap pitch (%d) too large for DRI3 protocol\n",
		     __FUNCTION__, bo->pitch));
		return -1;
	}

	if (!explicit && bo->tiling && !sna->kgem.can_fence) {
		if (!sna_pixmap_change_tiling(pixmap, I915_TILING_NONE)) {
			DBG(("%s: unable to discard GPU tiling (%d) for DRI3 protocol\n",
			     __FUNCTION__, bo->tiling));
			return -1;
		}
		bo = priv->gpu_bo;
	}

	fd = kgem_bo_export_to_prime(&sna->kgem, bo);
	if (fd == -1) {
		DBG(("%s: exporting handle=%d to fd failed\n", __FUNCTION__, bo->handle));
		return -1;
	}

	if (bo == priv->gpu_bo)
		priv->pinned |= PIN_DRI3;
	list_move(&priv->cow_list, &sna->dri3.pixmaps);

	mark_dri3_pixmap(sna, priv, bo);

	bo = (priv->pinned & PIN_DRI3) ? priv->gpu_bo : priv->cpu_bo;
	*stride = bo->pitch;
	*size = kgem_bo_size(bo);
	*tiling = bo->tiling;
	DBG(("%s: exporting %s pixmap=%ld, handle=%d, stride=%d, size=%d, tiling=%d\n",
	     __FUNCTION__,
	     (priv->pinned & PIN_DRI3) ? "GPU" : "CPU", pixmap->drawable.serialNumber,
	     bo->handle, *stride, *size, *tiling));
	return fd;
}

static int sna_dri3_fd_from_pixmap(ScreenPtr screen,
				   PixmapPtr pixmap,
				   CARD16 *stride,
				   CARD32 *size)
{
	uint32_t pitch;
	int tiling, fd;

	fd = __sna_dri3_fd_from_pixmap(screen, pixmap, &pitch, size,
				       &tiling, false);
	if (fd >= 0)
		*stride = pitch;
	return fd;
}

static int sna_dri3_fds_from_pixmap(ScreenPtr screen,
				    PixmapPtr pixmap,
				    int *fds,
				    uint32_t *strides,
				    uint32_t *offsets,
				    uint64_t *modifier)
{
	uint32_t size;
	int tiling;

	fds[0] = __sna_dri3_fd_from_pixmap(screen, pixmap, &strides[0], &size,
					   &tiling, true);
	if (fds[0] < 0)
		return 0;

	offsets[0] = 0;
	*modifier = sna_dri3_tiling_to_modifier(tiling);
	return 1;
}

/* the formats the server derives from a client's bpp (drm_format_for_depth) */
static const uint32_t sna_dri3_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB8888,
};

static bool sna_dri3_has_format(uint32_t format)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(sna_dri3_formats); i++)
		if (sna_dri3_formats[i] == format)
			return true;

	return false;
}

static int sna_dri3_get_formats(ScreenPtr screen,
				CARD32 *num_formats,
				CARD32 **formats)
{
	DBG(("%s()\n", __FUNCTION__));

	*formats = malloc(sizeof(sna_dri3_formats));
	if (*formats == NULL)
		return FALSE;

	memcpy(*formats, sna_dri3_formats, sizeof(sna_dri3_formats));
	*num_formats = ARRAY_SIZE(sna_dri3_formats);
	return TRUE;
}

static int sna_dri3_get_modifiers(ScreenPtr screen,
				  uint32_t format,
				  uint32_t *num_modifiers,
				  uint64_t **modifiers)
{
	struct sna *sna = to_sna_from_screen(screen);
	uint64_t *mods;
	uint32_t n = 0;

	*num_modifiers = 0;
	*modifiers = NULL;
	if (!sna_dri3_has_format(format))
		return TRUE;

	mods = malloc(3 * sizeof(*mods));
	if (mods == NULL)
		return FALSE;

	mods[n++] = DRM_FORMAT_MOD_LINEAR;
	mods[n++] = I915_FORMAT_MOD_X_TILED;
	if (sna->kgem.can_blt_y && sna->kgem.can_render_y)
		mods[n++] = I915_FORMAT_MOD_Y_TILED;

	DBG(("%s(format=%08x): %d modifiers\n", __FUNCTION__, format, n));
	*num_modifiers = n;
	*modifiers = mods;
	return TRUE;
}

/* what a client buffer may use and still be flipped (see sna_present.c) */
static int sna_dri3_get_drawable_modifiers(DrawablePtr draw,
					   uint32_t format,
					   uint32_t *num_modifiers,
					   uint64_t **modifiers)
{
	struct sna *sna = to_sna_from_screen(draw->pScreen);
	uint64_t *mods;
	uint32_t n = 0;

	*num_modifiers = 0;
	*modifiers = NULL;
	if (draw->type != DRAWABLE_WINDOW || !sna_dri3_has_format(format))
		return TRUE;

	mods = malloc(3 * sizeof(*mods));
	if (mods == NULL)
		return FALSE;

	mods[n++] = DRM_FORMAT_MOD_LINEAR;
	if ((sna->flags & SNA_LINEAR_FB) == 0) {
		mods[n++] = I915_FORMAT_MOD_X_TILED;
		if (sna->kgem.can_scanout_y &&
		    sna->kgem.can_blt_y && sna->kgem.can_render_y)
			mods[n++] = I915_FORMAT_MOD_Y_TILED;
	}

	DBG(("%s(format=%08x): %d modifiers\n", __FUNCTION__, format, n));
	*num_modifiers = n;
	*modifiers = mods;
	return TRUE;
}

#if HAVE_DRI3_SYNCOBJ
static struct dri3_syncobj *sna_dri3_import_syncobj(ClientPtr client,
						    ScreenPtr screen,
						    XID id, int fd)
{
	return intel_dri3_import_syncobj(screen,
					 to_sna_from_screen(screen)->kgem.fd,
					 id, fd);
}
#endif

/* version 4 and import_syncobj are added in sna_dri3_open() when the
 * kernel supports timeline sync objects */
static dri3_screen_info_rec sna_dri3_info = {
	.version = 2, /* the newest set of hooks filled in below */

	.open = sna_dri3_open_device,
	.pixmap_from_fd = sna_dri3_pixmap_from_fd,
	.fd_from_pixmap = sna_dri3_fd_from_pixmap,

	.pixmap_from_fds = sna_dri3_pixmap_from_fds,
	.fds_from_pixmap = sna_dri3_fds_from_pixmap,
	.get_formats = sna_dri3_get_formats,
	.get_modifiers = sna_dri3_get_modifiers,
	.get_drawable_modifiers = sna_dri3_get_drawable_modifiers,
};

bool sna_dri3_open(struct sna *sna, ScreenPtr screen)
{
	DBG(("%s()\n", __FUNCTION__));

	if (!sna_sync_open(sna, screen))
		return false;

	list_init(&sna->dri3.pixmaps);

#if HAVE_DRI3_SYNCOBJ
	if (intel_dri3_syncobj_supported(sna->scrn, sna->kgem.fd)) {
		sna_dri3_info.version = 4;
		sna_dri3_info.import_syncobj = sna_dri3_import_syncobj;
		sna->dri3.syncobj = true;
	}
#endif

	return dri3_screen_init(screen, &sna_dri3_info);
}

void sna_dri3_close(struct sna *sna, ScreenPtr screen)
{
	SyncScreenFuncsPtr funcs;

	DBG(("%s()\n", __FUNCTION__));

	funcs = miSyncGetScreenFuncs(screen);
	if (funcs)
		funcs->CreateFence = sna->dri3.create_fence;
}
