/*
 * Copyright 2012 Red Hat, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Dave Airlie <airlied@redhat.com>
 */

/* this file provides API compat between server post 1.13 and pre it,
   it should be reused inside as many drivers as possible */
#ifndef COMPAT_API_H
#define COMPAT_API_H

#include <xorg-server.h>
#include <xorgVersion.h>
#include <xf86Module.h>

#include <picturestr.h>

#define xf86ScrnToScreen(s) ((s)->pScreen)

static inline int
region_num_rects(const RegionRec *r)
{
	return r->data ? r->data->numRects : 1;
}

static inline BoxPtr
region_boxptr(const RegionRec *r)
{
	return (BoxPtr)(r->data + 1);
}

static inline const BoxRec *
region_rects(const RegionRec *r)
{
	return r->data ? (const BoxRec *)(r->data + 1) :  &r->extents;
}

inline static void
region_get_boxes(const RegionRec *r, const BoxRec **s, const BoxRec **e)
{
	int n;
	if (r->data)
		*s = region_boxptr(r), n = r->data->numRects;
	else
		*s = &r->extents, n = 1;
	*e = *s + n;
}

#define __get_private(p, key) dixGetPrivateAddr(&(p)->devPrivates, &(key))

#define SourceValidate(d, x, y, w, h, mode) \
	if ((d)->pScreen->SourceValidate) (d)->pScreen->SourceValidate(d, x, y, w, h, mode)

#define ddGetPortAttribute_ARGS XvPortPtr port, Atom attribute, INT32 *value
#define ddQueryBestSize_ARGS XvPortPtr port, CARD8 motion, CARD16 vid_w, CARD16 vid_h, CARD16 drw_w, CARD16 drw_h, unsigned int *p_w, unsigned int *p_h
#define ddPutImage_ARGS DrawablePtr draw, XvPortPtr port, GCPtr gc, INT16 src_x, INT16 src_y, CARD16 src_w, CARD16 src_h, INT16 drw_x, INT16 drw_y, CARD16 drw_w, CARD16 drw_h, XvImagePtr format, unsigned char *buf, Bool sync, CARD16 width, CARD16 height
#define ddQueryImageAttributes_ARGS XvPortPtr port, XvImagePtr format, unsigned short *w, unsigned short *h, int *pitches, int *offsets

#define PixmapSyncDirtyHelper(d, dd) PixmapSyncDirtyHelper(d)

#define PixmapDirtyDst(d) ((d)->secondary_dst)
#define PixmapDirtyPrimary(d) (PixmapDirtyDst(d)->primary_pixmap)

#endif
