/*
 * Copyright © 2013-2014 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include "config.h"

#include <string.h>
#include <drm_fourcc.h>

#include "xorg-server.h"
#include "xf86.h"
#include "fb.h"

#include "intel.h"
#if USE_UXA
#include "intel_uxa.h"
#endif
#include "dri3.h"

static int
intel_dri3_open(ScreenPtr screen,
                RRProviderPtr provider,
                int *out)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	int fd;

	fd = intel_get_client_fd(intel->dev);
	if (fd < 0)
		return -fd;

	*out = fd;
	return Success;
}

/* tiling < 0: implicit; else from a modifier, which must match the kernel
 * tiling (libdrm may hand back one of our own bos, so it isn't changed) */
static PixmapPtr __intel_dri3_pixmap_from_fd(ScreenPtr screen,
					     int fd,
					     CARD16 width,
					     CARD16 height,
					     CARD16 stride,
					     CARD8 depth,
					     CARD8 bpp,
					     int tiling)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	intel_screen_private *intel = intel_get_screen_private(scrn);
	struct intel_uxa_pixmap *priv;
	PixmapPtr pixmap;
	dri_bo *bo;

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

	pixmap = fbCreatePixmap(screen, 0, 0, depth, 0);
	if (!pixmap)
		return NULL;

	if (!screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, NULL))
		goto free_pixmap;

	bo = drm_intel_bo_gem_create_from_prime(intel->bufmgr,
						fd, (uint32_t)height * stride);
	if (bo == NULL)
		goto free_pixmap;

	if (tiling >= 0) {
		uint32_t kernel_tiling, swizzle;

		if (drm_intel_bo_get_tiling(bo, &kernel_tiling, &swizzle) ||
		    kernel_tiling != (uint32_t)tiling) {
			dri_bo_unreference(bo);
			goto free_pixmap;
		}
	}

	intel_uxa_set_pixmap_bo(pixmap, bo);
	dri_bo_unreference(bo);

	priv = intel_uxa_get_pixmap_private(pixmap);
	if (priv == NULL)
		goto free_pixmap;

	priv->pinned |= PIN_DRI3;

	return pixmap;

free_pixmap:
	fbDestroyPixmap(pixmap);
	return NULL;
}

static PixmapPtr intel_dri3_pixmap_from_fd(ScreenPtr screen,
					   int fd,
					   CARD16 width,
					   CARD16 height,
					   CARD16 stride,
					   CARD8 depth,
					   CARD8 bpp)
{
	return __intel_dri3_pixmap_from_fd(screen, fd, width, height, stride,
					   depth, bpp, -1);
}

/* Y tiling only where UXA's blits accept it (intel_uxa_check_bo_tiling) */
static bool intel_dri3_has_y(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);

	return INTEL_INFO(intel_get_screen_private(scrn))->gen >= 060;
}

static int intel_dri3_modifier_to_tiling(ScreenPtr screen, uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return I915_TILING_NONE;
	case I915_FORMAT_MOD_X_TILED:
		return I915_TILING_X;
	case I915_FORMAT_MOD_Y_TILED:
		return intel_dri3_has_y(screen) ? I915_TILING_Y : -1;
	default:
		return -1;
	}
}

static uint64_t intel_dri3_tiling_to_modifier(int tiling)
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

static PixmapPtr intel_dri3_pixmap_from_fds(ScreenPtr screen,
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
	int tiling = -1;

	/* single plane RGB only; the old protocol's 16-bit stride limit stays */
	if (num_fds != 1 || offsets[0] != 0 || strides[0] > UINT16_MAX)
		return NULL;

	if (modifier != DRM_FORMAT_MOD_INVALID) {
		tiling = intel_dri3_modifier_to_tiling(screen, modifier);
		if (tiling < 0)
			return NULL;
	}

	return __intel_dri3_pixmap_from_fd(screen, fds[0], width, height,
					   strides[0], depth, bpp, tiling);
}

static int intel_dri3_fd_from_pixmap(ScreenPtr screen,
				     PixmapPtr pixmap,
				     CARD16 *stride,
				     CARD32 *size)
{
	struct intel_uxa_pixmap *priv;
	int fd;

	priv = intel_uxa_get_pixmap_private(pixmap);
	if (!priv)
		return -1;

	if (intel_pixmap_pitch(pixmap) > UINT16_MAX)
		return -1;

	if (drm_intel_bo_gem_export_to_prime(priv->bo, &fd) < 0)
		return -1;

	priv->pinned |= PIN_DRI3;

	*stride = intel_pixmap_pitch(pixmap);
	*size = priv->bo->size;
	return fd;
}

static int intel_dri3_fds_from_pixmap(ScreenPtr screen,
				      PixmapPtr pixmap,
				      int *fds,
				      uint32_t *strides,
				      uint32_t *offsets,
				      uint64_t *modifier)
{
	struct intel_uxa_pixmap *priv;
	CARD16 stride;
	CARD32 size;

	fds[0] = intel_dri3_fd_from_pixmap(screen, pixmap, &stride, &size);
	if (fds[0] < 0)
		return 0;

	priv = intel_uxa_get_pixmap_private(pixmap);
	strides[0] = stride;
	offsets[0] = 0;
	*modifier = intel_dri3_tiling_to_modifier(priv->tiling);
	return 1;
}

/* the formats the server derives from a client's bpp (drm_format_for_depth) */
static const uint32_t intel_dri3_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB8888,
};

static bool intel_dri3_has_format(uint32_t format)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(intel_dri3_formats); i++)
		if (intel_dri3_formats[i] == format)
			return true;

	return false;
}

static int intel_dri3_get_formats(ScreenPtr screen,
				  CARD32 *num_formats,
				  CARD32 **formats)
{
	*formats = malloc(sizeof(intel_dri3_formats));
	if (*formats == NULL)
		return FALSE;

	memcpy(*formats, intel_dri3_formats, sizeof(intel_dri3_formats));
	*num_formats = ARRAY_SIZE(intel_dri3_formats);
	return TRUE;
}

static int intel_dri3_get_modifiers(ScreenPtr screen,
				    uint32_t format,
				    uint32_t *num_modifiers,
				    uint64_t **modifiers)
{
	uint64_t *mods;
	uint32_t n = 0;

	*num_modifiers = 0;
	*modifiers = NULL;
	if (!intel_dri3_has_format(format))
		return TRUE;

	mods = malloc(3 * sizeof(*mods));
	if (mods == NULL)
		return FALSE;

	mods[n++] = DRM_FORMAT_MOD_LINEAR;
	mods[n++] = I915_FORMAT_MOD_X_TILED;
	if (intel_dri3_has_y(screen))
		mods[n++] = I915_FORMAT_MOD_Y_TILED;

	*num_modifiers = n;
	*modifiers = mods;
	return TRUE;
}

/* what a client buffer may use and still be flipped (intel_present.c
 * refuses Y tiling) */
static int intel_dri3_get_drawable_modifiers(DrawablePtr draw,
					     uint32_t format,
					     uint32_t *num_modifiers,
					     uint64_t **modifiers)
{
	uint64_t *mods;

	*num_modifiers = 0;
	*modifiers = NULL;
	if (draw->type != DRAWABLE_WINDOW || !intel_dri3_has_format(format))
		return TRUE;

	mods = malloc(2 * sizeof(*mods));
	if (mods == NULL)
		return FALSE;

	mods[0] = DRM_FORMAT_MOD_LINEAR;
	mods[1] = I915_FORMAT_MOD_X_TILED;
	*num_modifiers = 2;
	*modifiers = mods;
	return TRUE;
}

static dri3_screen_info_rec intel_dri3_screen_info = {
        .version = 2, /* the newest set of hooks filled in below */

        .open = intel_dri3_open,
        .pixmap_from_fd = intel_dri3_pixmap_from_fd,
        .fd_from_pixmap = intel_dri3_fd_from_pixmap,

        .pixmap_from_fds = intel_dri3_pixmap_from_fds,
        .fds_from_pixmap = intel_dri3_fds_from_pixmap,
        .get_formats = intel_dri3_get_formats,
        .get_modifiers = intel_dri3_get_modifiers,
        .get_drawable_modifiers = intel_dri3_get_drawable_modifiers,
};

Bool
intel_dri3_screen_init(ScreenPtr screen)
{
        return dri3_screen_init(screen, &intel_dri3_screen_info);
}
