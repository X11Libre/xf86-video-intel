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

#include <xf86xv.h>
#include <X11/extensions/Xv.h>

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, true)

static Atom xvBrightness, xvContrast, xvSyncToVblank, xvColorspace;

static XvFormatRec Formats[] = {
	{ .depth = 15, },
	{ .depth = 16, },
	{ .depth = 24, },
	{ .depth = 30, },
};

static const XvAttributeRec Attributes[] = {
	{XvSettable | XvGettable, -1, 1, (char *)"XV_SYNC_TO_VBLANK"},
	{XvSettable | XvGettable, 0, 1, (char *)"XV_COLORSPACE"}, /* BT.601, BT.709 */
	//{XvSettable | XvGettable, -128, 127, (char *)"XV_BRIGHTNESS"},
	//{XvSettable | XvGettable, 0, 255, (char *)"XV_CONTRAST"},
};

static const XvImageRec gen2_Images[] = {
	XVIMAGE_YUY2,
	XVIMAGE_UYVY,
};

static const XvImageRec gen3_Images[] = {
	XVIMAGE_YUY2,
	XVIMAGE_YV12,
	XVIMAGE_I420,
	XVIMAGE_UYVY,
	XVMC_YUV,
};

static const XvImageRec gen4_Images[] = {
	XVIMAGE_YUY2,
	XVIMAGE_YV12,
	XVIMAGE_I420,
	XVIMAGE_NV12,
	XVIMAGE_UYVY,
	XVMC_YUV,
};

static const XvImageRec gen9_Images[] = {
	XVIMAGE_YUY2,
	XVIMAGE_YV12,
	XVIMAGE_I420,
	XVIMAGE_NV12,
	XVIMAGE_UYVY,
	XVIMAGE_AYUV,
	XVMC_YUV,
};

static int sna_video_textured_stop(ddStopVideo_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;

	DBG(("%s()\n", __FUNCTION__));

	RegionUninit(&video->clip);
	sna_video_free_buffers(video);

	return Success;
}

static int
sna_video_textured_set_attribute(ddSetPortAttribute_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;

	if (attribute == xvBrightness) {
		if (value < -128 || value > 127)
			return BadValue;

		video->brightness = value;
	} else if (attribute == xvContrast) {
		if (value < 0 || value > 255)
			return BadValue;

		video->contrast = value;
	} else if (attribute == xvSyncToVblank) {
		if (value < -1 || value > 1)
			return BadValue;

		video->SyncToVblank = value;
	} else if (attribute == xvColorspace) {
		if (value < 0 || value > 1)
			return BadValue;

		video->colorspace = value;
	} else
		return BadMatch;

	return Success;
}

static int
sna_video_textured_get_attribute(ddGetPortAttribute_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;

	if (attribute == xvBrightness)
		*value = video->brightness;
	else if (attribute == xvContrast)
		*value = video->contrast;
	else if (attribute == xvSyncToVblank)
		*value = video->SyncToVblank;
	else if (attribute == xvColorspace)
		*value = video->colorspace;
	else
		return BadMatch;

	return Success;
}

static int
sna_video_textured_best_size(ddQueryBestSize_ARGS)
{
	if (vid_w > (drw_w << 1))
		drw_w = vid_w >> 1;
	if (vid_h > (drw_h << 1))
		drw_h = vid_h >> 1;

	*p_w = drw_w;
	*p_h = drw_h;

	return Success;
}

/*
 * The source rectangle of the video is defined by (src_x, src_y, src_w, src_h).
 * The dest rectangle of the video is defined by (drw_x, drw_y, drw_w, drw_h).
 * id is a fourcc code for the format of the video.
 * buf is the pointer to the source data in system memory.
 * width and height are the w/h of the source data.
 * If "sync" is true, then we must be finished with *buf at the point of return
 * (which we always are).
 * clip is the clipping region in screen space.
 * data is a pointer to our port private.
 * drawable is some Drawable, which might not be the screen in the case of
 * compositing.  It's a new argument to the function in the 1.1 server.
 */
static int
sna_video_textured_put_image(ddPutImage_ARGS)
{
	struct sna_video *video = port->devPriv.ptr;
	struct sna *sna = video->sna;
	struct sna_video_frame frame;
	PixmapPtr pixmap = get_drawable_pixmap(draw);
	unsigned int flags;
	BoxRec dstBox;
	RegionRec clip;
	xf86CrtcPtr crtc;
	int16_t dx, dy;
	bool flush = false;
	bool ret;

	if (wedged(sna))
		return BadAlloc;

	init_video_region(&clip, draw, drw_x, drw_y, drw_w, drw_h);

	ValidateGC(draw, gc);
	RegionIntersect(&clip, &clip, gc->pCompositeClip);
	if (!RegionNotEmpty(&clip))
		return Success;

	DBG(("%s: src=(%d, %d),(%d, %d), dst=(%d, %d),(%d, %d), id=%d, sizep=%dx%d, sync?=%d\n",
	     __FUNCTION__,
	     src_x, src_y, src_w, src_h,
	     drw_x, drw_y, drw_w, drw_h,
	     format->id, width, height, sync));

	DBG(("%s: region %d:(%d, %d), (%d, %d)\n", __FUNCTION__,
	     region_num_rects(&clip),
	     clip.extents.x1, clip.extents.y1,
	     clip.extents.x2, clip.extents.y2));

	sna_video_frame_init(video, format->id, width, height, &frame);

	if (!sna_video_clip_helper(video, &frame, &crtc, &dstBox,
				   src_x, src_y, drw_x + draw->x, drw_y + draw->y,
				   src_w, src_h, drw_w, drw_h,
				   &clip))
		return Success;

	if (get_drawable_deltas(draw, pixmap, &dx, &dy))
		RegionTranslate(&clip, dx, dy);

	flags = MOVE_WRITE | __MOVE_FORCE;
	if (clip.data)
		flags |= MOVE_READ;

	if (!sna_pixmap_move_area_to_gpu(pixmap, &clip.extents, flags)) {
		DBG(("%s: attempting to render to a non-GPU pixmap\n",
		     __FUNCTION__));
		return BadAlloc;
	}

	sna_video_frame_set_rotation(video, &frame, RR_Rotate_0);

	if (xvmc_passthrough(format->id)) {
		DBG(("%s: using passthough, name=%d\n",
		     __FUNCTION__, *(uint32_t *)buf));

		frame.bo = kgem_create_for_name(&sna->kgem, *(uint32_t*)buf);
		if (frame.bo == NULL) {
			DBG(("%s: failed to open bo\n", __FUNCTION__));
			return BadAlloc;
		}

		if (kgem_bo_size(frame.bo) < frame.size) {
			DBG(("%s: bo size=%d, expected=%d\n",
			     __FUNCTION__, kgem_bo_size(frame.bo), frame.size));
			kgem_bo_destroy(&sna->kgem, frame.bo);
			return BadAlloc;
		}

		frame.image.x1 = 0;
		frame.image.y1 = 0;
		frame.image.x2 = frame.width;
		frame.image.y2 = frame.height;
	} else {
		if (!sna_video_copy_data(video, &frame, buf)) {
			DBG(("%s: failed to copy frame\n", __FUNCTION__));
			kgem_bo_destroy(&sna->kgem, frame.bo);
			return BadAlloc;
		}
	}

	if (crtc && video->SyncToVblank != 0 &&
	    sna_pixmap_is_scanout(sna, pixmap)) {
		kgem_set_mode(&sna->kgem, KGEM_RENDER, sna_pixmap(pixmap)->gpu_bo);
		flush = sna_wait_for_scanline(sna, pixmap, crtc,
					      &clip.extents);
	}

	ret = Success;
	if (!sna->render.video(sna, video, &frame, &clip, pixmap)) {
		DBG(("%s: failed to render video\n", __FUNCTION__));
		ret = BadAlloc;
	} else
		DamageDamageRegion(&pixmap->drawable, &clip);

	kgem_bo_destroy(&sna->kgem, frame.bo);

	/* Push the frame to the GPU as soon as possible so
	 * we can hit the next vsync.
	 */
	if (flush || sync)
		kgem_submit(&sna->kgem);

	RegionUninit(&clip);

	return ret;
}

static int
sna_video_textured_query(ddQueryImageAttributes_ARGS)
{
	int size, tmp;

	if (*w > 8192)
		*w = 8192;
	if (*h > 8192)
		*h = 8192;

	*w = (*w + 1) & ~1;
	if (offsets)
		offsets[0] = 0;

	switch (format->id) {
		/* IA44 is for XvMC only */
	case FOURCC_IA44:
	case FOURCC_AI44:
		if (pitches)
			pitches[0] = *w;
		size = *w * *h;
		break;
	case FOURCC_YV12:
	case FOURCC_I420:
		*h = (*h + 1) & ~1;
		size = (*w + 3) & ~3;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		if (offsets)
			offsets[1] = size;
		tmp = ((*w >> 1) + 3) & ~3;
		if (pitches)
			pitches[1] = pitches[2] = tmp;
		tmp *= (*h >> 1);
		size += tmp;
		if (offsets)
			offsets[2] = size;
		size += tmp;
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
	case FOURCC_UYVY:
	case FOURCC_YUY2:
	default:
		size = *w << 1;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		break;
	case FOURCC_AYUV:
		size = *w << 2;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		break;
	case FOURCC_XVMC:
		*h = (*h + 1) & ~1;
		size = sizeof(uint32_t);
		if (pitches)
			pitches[0] = size;
		break;
	}

	return size;
}

void sna_video_textured_setup(struct sna *sna, ScreenPtr screen)
{
	XvAdaptorPtr adaptor;
	struct sna_video *video;
	int nports, i;

	if (sna->scrn->depth == 8) {
		xf86DrvMsg(sna->scrn->scrnIndex, X_INFO,
			   "Textured video not supported in 8bpp mode\n");
		return;
	}

	if (!sna->render.video) {
		xf86DrvMsg(sna->scrn->scrnIndex, X_INFO,
			   "Textured video not supported on this hardware or backend\n");
		return;
	}

	if (wedged(sna)) {
		xf86DrvMsg(sna->scrn->scrnIndex, X_WARNING,
			   "cannot enable XVideo whilst the GPU is wedged\n");
		return;
	}

	adaptor = sna_xv_adaptor_alloc(sna);
	if (adaptor == NULL)
		return;

	nports = 16;
	if (sna->kgem.gen >= 060)
		nports = 32;
	if (sna->kgem.gen >= 0100)
		nports = 64;

	video = calloc(nports, sizeof(struct sna_video));
	adaptor->pPorts = calloc(nports, sizeof(XvPortRec));
	if (video == NULL || adaptor->pPorts == NULL) {
		free(video);
		free(adaptor->pPorts);
		sna->xv.num_adaptors--;
		return;
	}

	adaptor->type = XvInputMask | XvImageMask;
	adaptor->pScreen = screen;
	adaptor->name = (char *)"Intel(R) Textured Video";
	adaptor->nEncodings = 1;
	adaptor->pEncodings = XNFalloc(sizeof(XvEncodingRec));
	adaptor->pEncodings[0].id = 0;
	adaptor->pEncodings[0].pScreen = screen;
	adaptor->pEncodings[0].name = (char *)"XV_IMAGE";
	adaptor->pEncodings[0].width = sna->render.max_3d_size;
	adaptor->pEncodings[0].height = sna->render.max_3d_size;
	adaptor->pEncodings[0].rate.numerator = 1;
	adaptor->pEncodings[0].rate.denominator = 1;
	adaptor->pFormats = Formats;
	adaptor->nFormats = sna_xv_fixup_formats(screen, Formats,
						 ARRAY_SIZE(Formats));
	adaptor->nAttributes = ARRAY_SIZE(Attributes);
	adaptor->pAttributes = (XvAttributeRec *)Attributes;
	if (sna->kgem.gen < 030) {
		adaptor->nImages = ARRAY_SIZE(gen2_Images);
		adaptor->pImages = (XvImageRec *)gen2_Images;
	} else if (sna->kgem.gen < 040) {
		adaptor->nImages = ARRAY_SIZE(gen3_Images);
		adaptor->pImages = (XvImageRec *)gen3_Images;
	} else if (sna->kgem.gen < 0110) {
		adaptor->nImages = ARRAY_SIZE(gen4_Images);
		adaptor->pImages = (XvImageRec *)gen4_Images;
	} else {
		adaptor->nImages = ARRAY_SIZE(gen9_Images);
		adaptor->pImages = (XvImageRec *)gen9_Images;
	}
#if XORG_XV_VERSION < 2
	adaptor->ddAllocatePort = sna_xv_alloc_port;
	adaptor->ddFreePort = sna_xv_free_port;
#endif
	adaptor->ddPutVideo = NULL;
	adaptor->ddPutStill = NULL;
	adaptor->ddGetVideo = NULL;
	adaptor->ddGetStill = NULL;
	adaptor->ddStopVideo = sna_video_textured_stop;
	adaptor->ddSetPortAttribute = sna_video_textured_set_attribute;
	adaptor->ddGetPortAttribute = sna_video_textured_get_attribute;
	adaptor->ddQueryBestSize = sna_video_textured_best_size;
	adaptor->ddPutImage = sna_video_textured_put_image;
	adaptor->ddQueryImageAttributes = sna_video_textured_query;

	for (i = 0; i < nports; i++) {
		struct sna_video *v = &video[i];
		XvPortPtr port = &adaptor->pPorts[i];

		v->sna = sna;
		v->textured = true;
		v->alignment = 4;
		v->colorspace = 1; /* BT.709 */
		v->SyncToVblank = (sna->flags & SNA_NO_WAIT) == 0;

		RegionNull(&v->clip);

		port->id = FakeClientID(0);
		AddResource(port->id, XvGetRTPort(), port);

		port->pAdaptor = adaptor;
		port->pNotify =  NULL;
		port->pDraw =  NULL;
		port->client =  NULL;
		port->grab.client =  NULL;
		port->time = currentTime;
		port->devPriv.ptr = v;
	}
	adaptor->base_id = adaptor->pPorts[0].id;
	adaptor->nPorts = nports;

	xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
	xvContrast = MAKE_ATOM("XV_CONTRAST");
	xvColorspace = MAKE_ATOM("XV_COLORSPACE");
	xvSyncToVblank = MAKE_ATOM("XV_SYNC_TO_VBLANK");

	DBG(("%s: '%s' initialized %d ports\n", __FUNCTION__, adaptor->name, adaptor->nPorts));
}
