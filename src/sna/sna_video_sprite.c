/***************************************************************************

 Copyright 2000-2011 Intel Corporation.  All Rights Reserved.

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sub license, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial portions
 of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 IN NO EVENT SHALL INTEL, AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
 THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sna.h"
#include "sna_video.h"

#include "intel_options.h"

#include <xf86drm.h>
#include <xf86xv.h>
#include <xf86Crtc.h>
#include <X11/extensions/Xv.h>
#include <fourcc.h>
#include <i915_drm.h>
#include <errno.h>

#define fourcc_code(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_RGB565       fourcc_code('R', 'G', '1', '6') /* [15:0] R:G:B 5:6:5 little endian */
#define DRM_FORMAT_XRGB8888     fourcc_code('X', 'R', '2', '4') /* [31:0] x:R:G:B 8:8:8:8 little endian */
#define DRM_FORMAT_YUYV         fourcc_code('Y', 'U', 'Y', 'V') /* [31:0] Cr0:Y1:Cb0:Y0 8:8:8:8 little endian */
#define DRM_FORMAT_UYVY         fourcc_code('U', 'Y', 'V', 'Y') /* [31:0] Y1:Cr0:Y0:Cb0 8:8:8:8 little endian */
#define DRM_FORMAT_NV12         fourcc_code('N', 'V', '1', '2') /* 2x2 subsampled Cr:Cb plane */
#define DRM_FORMAT_XYUV8888     fourcc_code('X', 'Y', 'U', 'V') /* [31:0] x:Y:U:V 8:8:8:8 little endian */

#define has_hw_scaling(sna, video) ((sna)->kgem.gen < 071 || \
				    (sna)->kgem.gen >= 0110)

#define LOCAL_IOCTL_MODE_SETPLANE	DRM_IOWR(0xB7, struct local_mode_set_plane)
struct local_mode_set_plane {
	uint32_t plane_id;
	uint32_t crtc_id;
	uint32_t fb_id; /* fb object contains surface format type */
	uint32_t flags;

	/* Signed dest location allows it to be partially off screen */
	int32_t crtc_x, crtc_y;
	uint32_t crtc_w, crtc_h;

	/* Source values are 16.16 fixed point */
	uint32_t src_x, src_y;
	uint32_t src_h, src_w;
};

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, true)

static Atom xvColorKey, xvAlwaysOnTop, xvSyncToVblank, xvColorspace;

static XvFormatRec formats[] = {
	{ .depth = 8, },
	{ .depth = 15, },
	{ .depth = 16, },
	{ .depth = 24, },
	{ .depth = 30, },
};
static const XvImageRec images[] = { XVIMAGE_YUY2, XVIMAGE_UYVY,
				     XVMC_RGB888 };
static const XvImageRec images_rgb565[] = { XVIMAGE_YUY2, XVIMAGE_UYVY,
					    XVMC_RGB888, XVMC_RGB565 };
static const XvImageRec images_nv12[] = { XVIMAGE_YUY2, XVIMAGE_UYVY,
					  XVIMAGE_NV12, XVMC_RGB888, XVMC_RGB565 };
static const XvImageRec images_ayuv[] = { XVIMAGE_AYUV, XVIMAGE_YUY2, XVIMAGE_UYVY,
					  XVIMAGE_NV12, XVMC_RGB888, XVMC_RGB565 };
static const XvAttributeRec attribs[] = {
	{ XvSettable | XvGettable, 0, 1, (char *)"XV_COLORSPACE" }, /* BT.601, BT.709 */
	{ XvSettable | XvGettable, 0, 0xffffff, (char *)"XV_COLORKEY" },
	{ XvSettable | XvGettable, 0, 1, (char *)"XV_ALWAYS_ON_TOP" },
};

#define DRM_I915_SET_SPRITE_COLORKEY 0x2b
#define LOCAL_IOCTL_I915_SET_SPRITE_COLORKEY DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_SET_SPRITE_COLORKEY, struct local_intel_sprite_colorkey)
#define LOCAL_IOCTL_MODE_ADDFB2 DRM_IOWR(0xb8, struct local_mode_fb_cmd2)

struct local_intel_sprite_colorkey {
	uint32_t plane_id;
	uint32_t min_value;
	uint32_t channel_mask;
	uint32_t max_value;
	uint32_t flags;
};

static void sna_video_sprite_hide(xf86CrtcPtr crtc, struct sna_video *video)
{
	struct local_mode_set_plane s = {
		.plane_id = sna_crtc_to_sprite(crtc, video->idx),
	};
	struct local_intel_sprite_colorkey key = {
		.plane_id = sna_crtc_to_sprite(crtc, video->idx),
	};
	int index = sna_crtc_index(crtc);

	if (drmIoctl(video->sna->kgem.fd, LOCAL_IOCTL_MODE_SETPLANE, &s))
		xf86DrvMsg(video->sna->scrn->scrnIndex, X_ERROR,
			   "failed to disable plane\n");

	drmIoctl(video->sna->kgem.fd, LOCAL_IOCTL_I915_SET_SPRITE_COLORKEY, &key);
	video->color_key_changed |= 1 << index;
}

static int sna_video_sprite_stop(ddStopVideo_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(video->sna->scrn);
	int i;

	for (i = 0; i < video->sna->mode.num_real_crtc; i++) {
		xf86CrtcPtr crtc = config->crtc[i];
		int index;

		index = sna_crtc_index(crtc);
		assert(index < ARRAY_SIZE(video->bo));
		if (video->bo[index] == NULL)
			continue;

		sna_video_sprite_hide(crtc, video);
		kgem_bo_replace(&video->sna->kgem, &video->bo[index], NULL);
	}

	sna_window_set_port((WindowPtr)draw, NULL);

	return Success;
}

static int sna_video_sprite_set_attr(ddSetPortAttribute_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;

	if (attribute == xvColorKey) {
		video->color_key_changed = ~0;
		video->color_key = value;
		RegionEmpty(&video->clip);
		DBG(("COLORKEY = %ld\n", (long)value));
	} else if (attribute == xvColorspace) {
		video->colorspace_changed = ~0;
		video->colorspace = value;
		DBG(("COLORSPACE = %ld\n", (long)value));
	} else if (attribute == xvSyncToVblank) {
		DBG(("%s: SYNC_TO_VBLANK: %d -> %d\n", __FUNCTION__,
		     video->SyncToVblank, !!value));
		video->SyncToVblank = !!value;
	} else if (attribute == xvAlwaysOnTop) {
		DBG(("%s: ALWAYS_ON_TOP: %d -> %d\n", __FUNCTION__,
		     video->AlwaysOnTop, !!value));
		video->color_key_changed = ~0;
		video->AlwaysOnTop = !!value;
	} else
		return BadMatch;

	return Success;
}

static int sna_video_sprite_get_attr(ddGetPortAttribute_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;

	if (attribute == xvColorKey)
		*value = video->color_key;
	else if (attribute == xvColorspace)
		*value = video->colorspace;
	else if (attribute == xvAlwaysOnTop)
		*value = video->AlwaysOnTop;
	else if (attribute == xvSyncToVblank)
		*value = video->SyncToVblank;
	else
		return BadMatch;

	return Success;
}

static int sna_video_sprite_best_size(ddQueryBestSize_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;
	struct sna *sna = video->sna;

	if (!has_hw_scaling(sna, video) && !sna->render.video) {
		*p_w = vid_w;
		*p_h = vid_h;
	} else {
		*p_w = drw_w;
		*p_h = drw_h;
	}

	return Success;
}

static void
update_dst_box_to_crtc_coords(struct sna *sna, xf86CrtcPtr crtc, BoxPtr dstBox)
{
	ScrnInfoPtr scrn = sna->scrn;
	int tmp;

	switch (crtc->rotation & 0xf) {
	case RR_Rotate_0:
		dstBox->x1 -= crtc->x;
		dstBox->x2 -= crtc->x;
		dstBox->y1 -= crtc->y;
		dstBox->y2 -= crtc->y;
		break;

	case RR_Rotate_90:
		tmp = dstBox->x1;
		dstBox->x1 = dstBox->y1 - crtc->x;
		dstBox->y1 = scrn->virtualX - tmp - crtc->y;
		tmp = dstBox->x2;
		dstBox->x2 = dstBox->y2 - crtc->x;
		dstBox->y2 = scrn->virtualX - tmp - crtc->y;
		tmp = dstBox->y1;
		dstBox->y1 = dstBox->y2;
		dstBox->y2 = tmp;
		break;

	case RR_Rotate_180:
		tmp = dstBox->x1;
		dstBox->x1 = scrn->virtualX - dstBox->x2 - crtc->x;
		dstBox->x2 = scrn->virtualX - tmp - crtc->x;
		tmp = dstBox->y1;
		dstBox->y1 = scrn->virtualY - dstBox->y2 - crtc->y;
		dstBox->y2 = scrn->virtualY - tmp - crtc->y;
		break;

	case RR_Rotate_270:
		tmp = dstBox->x1;
		dstBox->x1 = scrn->virtualY - dstBox->y1 - crtc->x;
		dstBox->y1 = tmp - crtc->y;
		tmp = dstBox->x2;
		dstBox->x2 = scrn->virtualY - dstBox->y2 - crtc->x;
		dstBox->y2 = tmp - crtc->y;
		tmp = dstBox->x1;
		dstBox->x1 = dstBox->x2;
		dstBox->x2 = tmp;
		break;
	}
}

static uint32_t ckey_chan(uint32_t value, int weight)
{
	return value << 8 >> weight;
}

static uint32_t ckey_value_chan(uint32_t value, uint32_t mask,
				int offset, int weight)
{
	return ckey_chan((value & mask) >> offset, weight);
}

static uint32_t ckey_value(struct sna *sna,
			   struct sna_video *video)
{
	ScrnInfoPtr scrn = sna->scrn;
	uint32_t r, g ,b;

	if (scrn->depth == 8) {
		r = g = b = video->color_key & 0xff;
	} else {
		r = ckey_value_chan(video->color_key, scrn->mask.red,
				    scrn->offset.red, scrn->weight.red);
		g = ckey_value_chan(video->color_key, scrn->mask.green,
				    scrn->offset.green, scrn->weight.green);
		b = ckey_value_chan(video->color_key, scrn->mask.blue,
				    scrn->offset.blue, scrn->weight.blue);
	}

	return r << 16 | g << 8 | b;
}

static uint32_t ckey_mask_chan(int weight)
{
	return ckey_chan((1 << weight) - 1, weight);
}

static uint32_t ckey_mask(struct sna *sna)
{
	ScrnInfoPtr scrn = sna->scrn;
	uint32_t r = ckey_mask_chan(scrn->weight.red);
	uint32_t g = ckey_mask_chan(scrn->weight.green);
	uint32_t b = ckey_mask_chan(scrn->weight.blue);

	return 0x7 << 24 | r << 16 | g << 8 | b;
}

static bool
sna_video_sprite_show(struct sna *sna,
		      struct sna_video *video,
		      struct sna_video_frame *frame,
		      xf86CrtcPtr crtc,
		      BoxPtr dstBox)
{
	struct local_mode_set_plane s;
	int index = sna_crtc_index(crtc);

	/* XXX handle video spanning multiple CRTC */

	VG_CLEAR(s);
	s.plane_id = sna_crtc_to_sprite(crtc, video->idx);

	if (video->color_key_changed & (1 << index) && video->has_color_key) {
		struct local_intel_sprite_colorkey set;

		DBG(("%s: updating color key: %x\n",
		     __FUNCTION__, video->color_key));

		set.plane_id = s.plane_id;
		set.min_value = ckey_value(sna, video);
		set.max_value = 0; /* not used for destkey */
		set.channel_mask = ckey_mask(sna);
		set.flags = 0;
		if (!video->AlwaysOnTop)
			set.flags |= 1 << 1; /* COLORKEY_DESTINATION */

		if (drmIoctl(sna->kgem.fd,
			     LOCAL_IOCTL_I915_SET_SPRITE_COLORKEY,
			     &set)) {
			/* try to disable the plane first */
			sna_video_sprite_hide(crtc, video);

			if (drmIoctl(sna->kgem.fd, LOCAL_IOCTL_I915_SET_SPRITE_COLORKEY, &set)) {
				xf86DrvMsg(sna->scrn->scrnIndex, X_ERROR,
					   "failed to update color key, disabling future updates\n");
				video->has_color_key = false;
			}
		}

		video->color_key_changed &= ~(1 << index);
	}

	if (video->colorspace_changed & (1 << index)) {
		DBG(("%s: updating colorspace: %x\n",
		     __FUNCTION__, video->colorspace));

		sna_crtc_set_sprite_colorspace(crtc, video->idx,
					       video->colorspace);

		video->colorspace_changed &= ~(1 << index);
	}

	update_dst_box_to_crtc_coords(sna, crtc, dstBox);
	if (frame->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
		int tmp = frame->width;
		frame->width = frame->height;
		frame->height = tmp;
	}

	if (frame->bo->delta == 0) {
		struct local_mode_fb_cmd2 {
			uint32_t fb_id;
			uint32_t width, height;
			uint32_t pixel_format;
			uint32_t flags;

			uint32_t handles[4];
			uint32_t pitches[4]; /* pitch for each plane */
			uint32_t offsets[4]; /* offset of each plane */
			uint64_t modifiers[4];
		} f;
		bool purged = true;

		memset(&f, 0, sizeof(f));
		f.width = frame->width;
		f.height = frame->height;
		f.flags = 1 << 1; /* +modifiers */

		switch (frame->bo->tiling) {
		case I915_TILING_NONE:
			break;
		case I915_TILING_X:
			/* I915_FORMAT_MOD_X_TILED */
			f.modifiers[0] = (uint64_t)1 << 56 | 1;
			break;
		case I915_TILING_Y:
			/* I915_FORMAT_MOD_X_TILED */
			f.modifiers[0] = (uint64_t)1 << 56 | 2;
			break;
		}

		if (is_nv12_fourcc(frame->id)) {
			f.handles[0] = frame->bo->handle;
			f.handles[1] = frame->bo->handle;
			f.pitches[0] = frame->pitch[1];
			f.pitches[1] = frame->pitch[0];
			f.offsets[0] = 0;
			f.offsets[1] = frame->UBufOffset;
		} else {
			f.handles[0] = frame->bo->handle;
			f.pitches[0] = frame->pitch[0];
		}

		switch (frame->id) {
		case FOURCC_RGB565:
			f.pixel_format = DRM_FORMAT_RGB565;
			purged = sna->scrn->depth != 16;
			break;
		case FOURCC_RGB888:
			f.pixel_format = DRM_FORMAT_XRGB8888;
			purged = sna->scrn->depth != 24;
			break;
		case FOURCC_NV12:
			f.pixel_format = DRM_FORMAT_NV12;
			break;
		case FOURCC_UYVY:
			f.pixel_format = DRM_FORMAT_UYVY;
			break;
		case FOURCC_AYUV:
			/* i915 doesn't support alpha, so we use XYUV */
			f.pixel_format = DRM_FORMAT_XYUV8888;
			break;
		case FOURCC_YUY2:
		default:
			f.pixel_format = DRM_FORMAT_YUYV;
			break;
		}

		DBG(("%s: creating new fb for handle=%d, width=%d, height=%d, stride=%d, format=%x\n",
		     __FUNCTION__, frame->bo->handle, frame->width, frame->height,
		     f.pitches[0], f.pixel_format));

		if (drmIoctl(sna->kgem.fd, LOCAL_IOCTL_MODE_ADDFB2, &f)) {
			ERR(("%s: ADDFB2 failed, errno=%d\n", __FUNCTION__, errno));
			xf86DrvMsg(sna->scrn->scrnIndex,
				   X_ERROR, "failed to add fb, unable to update video\n");
			return false;
		}

		frame->bo->delta = f.fb_id;

		frame->bo->scanout = true;
		/* Don't allow the scanout to be cached if not suitable for front */
		frame->bo->purged = purged;
	}

	assert(frame->bo->scanout);
	assert(frame->bo->delta);

	s.crtc_id = sna_crtc_id(crtc);
	s.fb_id = frame->bo->delta;
	s.flags = 0;
	s.crtc_x = dstBox->x1;
	s.crtc_y = dstBox->y1;
	s.crtc_w = dstBox->x2 - dstBox->x1;
	s.crtc_h = dstBox->y2 - dstBox->y1;
	s.src_x = 0;
	s.src_y = 0;
	s.src_w = (frame->image.x2 - frame->image.x1) << 16;
	s.src_h = (frame->image.y2 - frame->image.y1) << 16;

	DBG(("%s: updating crtc=%d, plane=%d, handle=%d [fb %d], dst=(%d,%d)x(%d,%d), src=(%d,%d)x(%d,%d)\n",
	     __FUNCTION__, s.crtc_id, s.plane_id, frame->bo->handle, s.fb_id,
	     s.crtc_x, s.crtc_y, s.crtc_w, s.crtc_h,
	     s.src_x >> 16, s.src_y >> 16, s.src_w >> 16, s.src_h >> 16));

	if (drmIoctl(sna->kgem.fd, LOCAL_IOCTL_MODE_SETPLANE, &s)) {
		DBG(("SET_PLANE failed: ret=%d\n", errno));
		kgem_bo_replace(&sna->kgem, &video->bo[index], NULL);
		return false;
	}

	__kgem_bo_clear_dirty(frame->bo);

	kgem_bo_replace(&sna->kgem, &video->bo[index], frame->bo);
	return true;
}

static bool need_scaling(const struct sna_video_frame *frame,
			 const BoxRec *dst)
{
	/* SKL+ need the plane scaler even for unscaled NV12 */
	return frame->id == FOURCC_NV12 ||
		frame->src.x2 - frame->src.x1 != dst->x2 - dst->x1 ||
		frame->src.y2 - frame->src.y1 != dst->y2 - dst->y1;
}

static int sna_video_sprite_put_image(ddPutImage_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;
	struct sna *sna = video->sna;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(sna->scrn);
	RegionRec clip;
	BoxRec draw_extents;
	int ret, i;

	init_video_region(&clip, draw, drw_x, drw_y, drw_w, drw_h);
	draw_extents = clip.extents;

	DBG(("%s: always_on_top=%d\n", __FUNCTION__, video->AlwaysOnTop));
	if (!video->AlwaysOnTop) {
		ValidateGC(draw, gc);
		RegionIntersect(&clip, &clip, gc->pCompositeClip);
	}

	DBG(("%s: src=(%d, %d),(%d, %d), dst=(%d, %d),(%d, %d), id=%d, sizep=%dx%d, sync?=%d\n",
	     __FUNCTION__,
	     src_x, src_y, src_w, src_h,
	     drw_x, drw_y, drw_w, drw_h,
	     format->id, width, height, sync));

	DBG(("%s: region %d:(%d, %d), (%d, %d)\n", __FUNCTION__,
	     region_num_rects(&clip),
	     clip.extents.x1, clip.extents.y1,
	     clip.extents.x2, clip.extents.y2));

	if (RegionNil(&clip)) {
		ret = Success;
		goto err;
	}

	for (i = 0; i < video->sna->mode.num_real_crtc; i++) {
		xf86CrtcPtr crtc = config->crtc[i];
		struct sna_video_frame frame;
		const int index = sna_crtc_index(crtc);
		bool hw_scaling = has_hw_scaling(sna, video);
		INT32 x1, x2, y1, y2;
		Rotation rotation;
		RegionRec reg;
		BoxRec dst;
		bool cache_bo;

retry:
		dst = draw_extents;

		sna_video_frame_init(video, format->id, width, height, &frame);

		reg.extents = crtc->bounds;
		reg.data = NULL;
		RegionIntersect(&reg, &reg, &clip);
		if (RegionNil(&reg)) {
off:
			assert(index < ARRAY_SIZE(video->bo));
			if (video->bo[index]) {
				sna_video_sprite_hide(crtc, video);
				kgem_bo_replace(&sna->kgem, &video->bo[index], NULL);
			}
			continue;
		}

		x1 = src_x;
		x2 = src_x + src_w;
		y1 = src_y;
		y2 = src_y + src_h;

		ret = xf86XVClipVideoHelper(&dst, &x1, &x2, &y1, &y2,
					    &reg, frame.width, frame.height);
		RegionUninit(&reg);
		if (!ret)
			goto off;

		frame.src.x1 = x1 >> 16;
		frame.src.y1 = y1 >> 16;
		frame.src.x2 = (x2 + 0xffff) >> 16;
		frame.src.y2 = (y2 + 0xffff) >> 16;

		frame.image.x1 = frame.src.x1 & ~1;
		frame.image.x2 = ALIGN(frame.src.x2, 2);
		if (is_planar_fourcc(frame.id)) {
			frame.image.y1 = frame.src.y1 & ~1;
			frame.image.y2 = ALIGN(frame.src.y2, 2);
		} else {
			frame.image.y1 = frame.src.y1;
			frame.image.y2 = frame.src.y2;
		}

		/* if sprite can't handle rotation natively, store it for the copy func */
		rotation = RR_Rotate_0;
		if (!sna_crtc_set_sprite_rotation(crtc, video->idx, crtc->rotation)) {
			sna_crtc_set_sprite_rotation(crtc, video->idx, RR_Rotate_0);
			rotation = crtc->rotation;
		}
		sna_video_frame_set_rotation(video, &frame, rotation);

		if (xvmc_passthrough(format->id)) {
			DBG(("%s: using passthough, name=%d\n",
			     __FUNCTION__, *(uint32_t *)buf));

			if (*(uint32_t*)buf == 0)
				goto err;

			frame.bo = kgem_create_for_name(&sna->kgem, *(uint32_t*)buf);
			if (frame.bo == NULL) {
				ret = BadAlloc;
				goto err;
			}

			if (kgem_bo_size(frame.bo) < frame.size) {
				DBG(("%s: bo size=%d, expected=%d\n",
				     __FUNCTION__, kgem_bo_size(frame.bo), frame.size));
				kgem_bo_destroy(&sna->kgem, frame.bo);
				ret = BadAlloc;
				goto err;
			}

			frame.image.x1 = 0;
			frame.image.y1 = 0;
			frame.image.x2 = frame.width;
			frame.image.y2 = frame.height;

			cache_bo = false;
		} else {
			frame.bo = sna_video_buffer(video, &frame);
			if (frame.bo == NULL) {
				DBG(("%s: failed to allocate video bo\n", __FUNCTION__));
				ret = BadAlloc;
				goto err;
			}

			if (!sna_video_copy_data(video, &frame, buf)) {
				DBG(("%s: failed to copy video data\n", __FUNCTION__));
				ret = BadAlloc;
				goto err;
			}

			cache_bo = true;
		}

		if (!hw_scaling && sna->render.video &&
		    need_scaling(&frame, &dst)) {
			ScreenPtr screen = to_screen_from_sna(sna);
			PixmapPtr scaled;
			RegionRec r;

			r.extents.x1 = r.extents.y1 = 0;
			r.extents.x2 = dst.x2 - dst.x1;
			r.extents.y2 = dst.y2 - dst.y1;
			r.data = NULL;

			DBG(("%s: scaling from (%d, %d) to (%d, %d)\n",
			     __FUNCTION__,
			     frame.src.x2 - frame.src.x1,
			     frame.src.y2 - frame.src.y1,
			     r.extents.x2, r.extents.y2));

			scaled = screen->CreatePixmap(screen,
						      r.extents.x2,
						      r.extents.y2,
						      24,
						      CREATE_PIXMAP_USAGE_SCRATCH);
			if (scaled == NULL) {
				ret = BadAlloc;
				goto err;
			}

			if (!sna->render.video(sna, video, &frame, &r, scaled)) {
				dixDestroyPixmap(scaled, 0);
				ret = BadAlloc;
				goto err;
			}

			if (cache_bo)
				sna_video_buffer_fini(video);
			else
				kgem_bo_destroy(&sna->kgem, frame.bo);

			frame.bo = kgem_bo_reference(__sna_pixmap_get_bo(scaled));
			kgem_bo_submit(&sna->kgem, frame.bo);

			frame.id = FOURCC_RGB888;
			frame.src = frame.image = r.extents;
			frame.width = frame.image.x2;
			frame.height = frame.image.y2;
			frame.pitch[0] = frame.bo->pitch;

			dixDestroyPixmap(scaled, 0);
			cache_bo = false;
		}

		ret = Success;
		if (!sna_video_sprite_show(sna, video, &frame, crtc, &dst)) {
			DBG(("%s: failed to show video frame\n", __FUNCTION__));
			ret = BadAlloc;
		}

		if (cache_bo)
			sna_video_buffer_fini(video);
		else
			kgem_bo_destroy(&sna->kgem, frame.bo);

		if (ret != Success) {
			/* retry with GPU scaling */
			if (hw_scaling) {
				hw_scaling = false;
				goto retry;
			}
			goto err;
		}
	}

	sna_video_fill_colorkey(video, &clip);
	sna_window_set_port((WindowPtr)draw, port);

	return Success;

err:
#if XORG_XV_VERSION < 2
	(void)sna_video_sprite_stop(client, port, draw);
#else
	(void)sna_video_sprite_stop(port, draw);
#endif
	return ret;
}

static int sna_video_sprite_query(ddQueryImageAttributes_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;
	struct sna_video_frame frame;
	int size, tmp;

	if (*w > video->sna->mode.max_crtc_width)
		*w = video->sna->mode.max_crtc_width;
	if (*h > video->sna->mode.max_crtc_height)
		*h = video->sna->mode.max_crtc_height;

	if (offsets)
		offsets[0] = 0;

	switch (format->id) {
	case FOURCC_RGB888:
	case FOURCC_RGB565:
		if (pitches) {
			sna_video_frame_init(video, format->id, *w, *h, &frame);
			sna_video_frame_set_rotation(video, &frame, RR_Rotate_0);
			pitches[0] = frame.pitch[0];
		}
		size = 4;
		break;

	case FOURCC_NV12:
		*h = (*h + 1) & ~1;
		size = (*w + 3) & ~3;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		if (offsets)
			offsets[1] = size;
		tmp = (*w + 3) & ~3;
		if (pitches)
			pitches[1] = tmp;
		tmp *= (*h >> 1);
		size += tmp;
		break;
	case FOURCC_AYUV:
		tmp = *w << 2;
		if (pitches)
			pitches[0] = tmp;
		size = *h * tmp;
		break;
	default:
		*w = (*w + 1) & ~1;
		*h = (*h + 1) & ~1;

		size = *w << 1;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		break;
	}

	return size;
}

static int sna_video_sprite_color_key(struct sna *sna)
{
	ScrnInfoPtr scrn = sna->scrn;
	int color_key;

	if (xf86GetOptValInteger(sna->Options, OPTION_VIDEO_KEY,
				 &color_key)) {
	} else if (xf86GetOptValInteger(sna->Options, OPTION_COLOR_KEY,
					&color_key)) {
	} else {
		color_key =
		    (1 << scrn->offset.red) |
		    (1 << scrn->offset.green) |
		    (((scrn->mask.blue >> scrn->offset.blue) - 1) << scrn->offset.blue);
	}

	return color_key & ((1 << scrn->depth) - 1);
}

static int sna_video_has_sprites(struct sna *sna)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(sna->scrn);
	unsigned min;
	int i;

	DBG(("%s: num_crtc=%d\n", __FUNCTION__, sna->mode.num_real_crtc));

	if (sna->mode.num_real_crtc == 0)
		return 0;

	min = -1;
	for (i = 0; i < sna->mode.num_real_crtc; i++) {
		unsigned count =  sna_crtc_count_sprites(config->crtc[i]);
		DBG(("%s: %d sprites found on crtc %d\n", __FUNCTION__,
		     count, sna_crtc_index(config->crtc[i])));
		if (count < min)
			min = count;
	}

	DBG(("%s: min=%d\n", __FUNCTION__, min));
	return min;
}

void sna_video_sprite_setup(struct sna *sna, ScreenPtr screen)
{
	XvAdaptorPtr adaptor;
	struct sna_video *video;
	XvPortPtr port;
	int count, i;

	count = sna_video_has_sprites(sna);
	if (!count)
		return;

	adaptor = sna_xv_adaptor_alloc(sna);
	if (!adaptor)
		return;

	video = calloc(count, sizeof(*video));
	port = calloc(count, sizeof(*port));
	if (video == NULL || port == NULL) {
		free(video);
		free(port);
		sna->xv.num_adaptors--;
		return;
	}

	adaptor->type = XvInputMask | XvImageMask;
	adaptor->pScreen = screen;
	adaptor->name = (char *)"Intel(R) Video Sprite";
	adaptor->nEncodings = 1;
	adaptor->pEncodings = XNFalloc(sizeof(XvEncodingRec));
	adaptor->pEncodings[0].id = 0;
	adaptor->pEncodings[0].pScreen = screen;
	adaptor->pEncodings[0].name = (char *)"XV_IMAGE";
	adaptor->pEncodings[0].width = sna->mode.max_crtc_width;
	adaptor->pEncodings[0].height = sna->mode.max_crtc_height;
	adaptor->pEncodings[0].rate.numerator = 1;
	adaptor->pEncodings[0].rate.denominator = 1;
	adaptor->pFormats = formats;
	adaptor->nFormats = sna_xv_fixup_formats(screen, formats,
						 ARRAY_SIZE(formats));
	adaptor->nAttributes = ARRAY_SIZE(attribs);
	adaptor->pAttributes = (XvAttributeRec *)attribs;

	if (sna_has_sprite_format(sna, DRM_FORMAT_XYUV8888)) {
		adaptor->pImages = (XvImageRec *)images_ayuv;
		adaptor->nImages = ARRAY_SIZE(images_ayuv);
	} else if (sna_has_sprite_format(sna, DRM_FORMAT_NV12)) {
		adaptor->pImages = (XvImageRec *)images_nv12;
		adaptor->nImages = ARRAY_SIZE(images_nv12);
	} else if (sna_has_sprite_format(sna, DRM_FORMAT_RGB565)) {
		adaptor->pImages = (XvImageRec *)images_rgb565;
		adaptor->nImages = ARRAY_SIZE(images_rgb565);
	} else {
		adaptor->pImages = (XvImageRec *)images;
		adaptor->nImages = ARRAY_SIZE(images);
	}

#if XORG_XV_VERSION < 2
	adaptor->ddAllocatePort = sna_xv_alloc_port;
	adaptor->ddFreePort = sna_xv_free_port;
#endif
	adaptor->ddPutVideo = NULL;
	adaptor->ddPutStill = NULL;
	adaptor->ddGetVideo = NULL;
	adaptor->ddGetStill = NULL;
	adaptor->ddStopVideo = sna_video_sprite_stop;
	adaptor->ddSetPortAttribute = sna_video_sprite_set_attr;
	adaptor->ddGetPortAttribute = sna_video_sprite_get_attr;
	adaptor->ddQueryBestSize = sna_video_sprite_best_size;
	adaptor->ddPutImage = sna_video_sprite_put_image;
	adaptor->ddQueryImageAttributes = sna_video_sprite_query;

	adaptor->nPorts = count;
	adaptor->pPorts = port;

	for (i = 0; i < count; i++) {
		port->id = FakeClientID(0);
		AddResource(port->id, XvGetRTPort(), port);
		port->pAdaptor = adaptor;
		port->pNotify =  NULL;
		port->pDraw =  NULL;
		port->client =  NULL;
		port->grab.client =  NULL;
		port->time = currentTime;
		port->devPriv.ptr = video;

		video->sna = sna;
		video->idx = i;
		video->alignment = 64;
		video->color_key = sna_video_sprite_color_key(sna);
		video->color_key_changed = ~0;
		video->colorspace = 1; /* BT.709 */
		video->colorspace_changed = ~0;
		video->has_color_key = true;
		video->brightness = -19;	/* (255/219) * -16 */
		video->contrast = 75;	/* 255/219 * 64 */
		video->saturation = 146;	/* 128/112 * 128 */
		video->desired_crtc = NULL;
		video->gamma5 = 0xc0c0c0;
		video->gamma4 = 0x808080;
		video->gamma3 = 0x404040;
		video->gamma2 = 0x202020;
		video->gamma1 = 0x101010;
		video->gamma0 = 0x080808;
		RegionNull(&video->clip);
		video->SyncToVblank = 1;

		port++;
		video++;
	}
	adaptor->base_id = adaptor->pPorts[0].id;

	xvColorKey = MAKE_ATOM("XV_COLORKEY");
	xvColorspace = MAKE_ATOM("XV_COLORSPACE");
	xvAlwaysOnTop = MAKE_ATOM("XV_ALWAYS_ON_TOP");
	xvSyncToVblank = MAKE_ATOM("XV_SYNC_TO_VBLANK");

	DBG(("%s: '%s' initialized %d ports\n", __FUNCTION__, adaptor->name, adaptor->nPorts));
}
