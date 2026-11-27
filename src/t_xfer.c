// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * xfer: transfers through the 3D pipe, on an NPOT (137x91) and a POT
 * (256x256) image: buffer -> image upload, image copy with offsets onto a
 * cleared image, a mirrored 1:1 blit, and a 4x MSAA clear + resolve. Every
 * byte is checked.
 */
#include <inttypes.h>
#include <stdio.h>

#include "vkfwtest.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t CLEAR_DST = 0x44332211u;     /* bytes 11 22 33 44 */
constexpr uint32_t CLEAR_MSAA = 0x80C04020u;

static uint32_t pattern(uint32_t x, uint32_t y, uint32_t w, uint32_t seed)
{
	return ((x + y * w) * 2654435761u) ^ seed;
}

struct xf {
	uint32_t w, h, seed;
	/* copy region */
	int32_t sx, sy, dx, dy;
	uint32_t cw, ch;
	uint32_t clear;
	enum { X_UPLOAD, X_COPY, X_BLIT, X_SOLID } kind;
};

static struct rgba xf_expect(uint32_t x, uint32_t y, const void *ctx)
{
	const struct xf *e = ctx;

	switch (e->kind) {
	case X_UPLOAD:
		return rgba_of(pattern(x, y, e->w, e->seed));
	case X_COPY:
		if ((int32_t)x >= e->dx && (int32_t)x < e->dx + (int32_t)e->cw &&
		    (int32_t)y >= e->dy && (int32_t)y < e->dy + (int32_t)e->ch)
			return rgba_of(pattern((uint32_t)((int32_t)x - e->dx + e->sx),
					       (uint32_t)((int32_t)y - e->dy + e->sy), e->w, e->seed));
		return rgba_of(e->clear);
	case X_BLIT:
		return rgba_of(pattern(e->w - 1 - x, y, e->w, e->seed));
	case X_SOLID:
		return rgba_of(e->clear);
	}
	return rgba_of(0);
}

static VkClearColorValue clear_of(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearColorValue){ .float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						 b.a / 255.0f } };
}

static const VkImageSubresourceRange COLOR_RANGE = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
static const VkImageSubresourceLayers COLOR_LAYERS = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };

/* Read `im` (in TRANSFER_DST layout) back and check it. */
static enum result readback_check(struct vk *v, struct report *r, struct img *im, struct buf *rb,
				  const char *what, const struct xf *e)
{
	VkCommandBuffer cb;

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	cmd_copy_to_buffer(cb, im, rb);
	img_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	CHECK(cmd_submit_wait(v, cb));
	return check_rgba(v, r, what, rb->map, im->w, im->h, xf_expect, e);
}

static bool can_blit(struct vk *v)
{
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, FMT, &fp);
	return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
	       (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

static bool can_msaa4(struct vk *v)
{
	VkImageFormatProperties ip;

	if (vkGetPhysicalDeviceImageFormatProperties(
		    v->phys, FMT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    0, &ip) != VK_SUCCESS)
		return false;
	return ip.sampleCounts & VK_SAMPLE_COUNT_4_BIT;
}

static enum result xfer_size(struct vk *v, struct report *r, struct buf *rb, uint32_t w,
			     uint32_t h, uint32_t seed, bool blit, bool msaa)
{
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
					VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	struct img src, dst;
	struct buf up;
	VkCommandBuffer cb;
	char what[48];
	struct xf e = { .w = w, .h = h, .seed = seed };

	CHECK(img_create(v, &src, w, h, FMT, usage, VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &dst, w, h, FMT, usage, VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &up, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));

	/* 1. buffer -> image upload of the pattern, written byte by byte */
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			struct rgba p = rgba_of(pattern(x, y, w, seed));
			uint8_t *q = up.map + ((size_t)y * w + x) * 4;

			q[0] = p.r;
			q[1] = p.g;
			q[2] = p.b;
			q[3] = p.a;
		}
	VkBufferImageCopy bic = { .imageSubresource = COLOR_LAYERS, .imageExtent = { w, h, 1 } };

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyBufferToImage(cb, up.b, src.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	CHECK(cmd_submit_wait(v, cb));
	e.kind = X_UPLOAD;
	snprintf(what, sizeof(what), "%ux%u upload", w, h);
	CHECK(readback_check(v, r, &src, rb, what, &e));

	/* 2. clear dst, copy a sub-region with offsets */
	e.kind = X_COPY;
	e.clear = CLEAR_DST;
	e.sx = 5;
	e.sy = 3;
	e.dx = 17;
	e.dy = 21;
	e.cw = w - 17 < 100 ? w - 17 : 100;
	e.ch = h - 21 < 60 ? h - 21 : 60;
	VkClearColorValue cc = clear_of(CLEAR_DST);
	VkImageCopy ic = {
		.srcSubresource = COLOR_LAYERS,
		.srcOffset = { e.sx, e.sy, 0 },
		.dstSubresource = COLOR_LAYERS,
		.dstOffset = { e.dx, e.dy, 0 },
		.extent = { e.cw, e.ch, 1 },
	};

	CHECK(cmd_begin(v, &cb));
	vkCmdClearColorImage(cb, dst.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &COLOR_RANGE);
	img_barrier(cb, &src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImage(cb, src.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.i,
		       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	CHECK(cmd_submit_wait(v, cb));
	snprintf(what, sizeof(what), "%ux%u copy with offsets", w, h);
	CHECK(readback_check(v, r, &dst, rb, what, &e));

	/* 3. mirrored 1:1 blit (x flipped) */
	if (blit) {
		VkImageBlit ib = {
			.srcSubresource = COLOR_LAYERS,
			.srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
			.dstSubresource = COLOR_LAYERS,
			.dstOffsets = { { (int32_t)w, 0, 0 }, { 0, (int32_t)h, 1 } },
		};

		e.kind = X_BLIT;
		CHECK(cmd_begin(v, &cb));
		vkCmdBlitImage(cb, src.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.i,
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, VK_FILTER_NEAREST);
		CHECK(cmd_submit_wait(v, cb));
		snprintf(what, sizeof(what), "%ux%u mirrored blit", w, h);
		CHECK(readback_check(v, r, &dst, rb, what, &e));
	}

	/* 4. 4x MSAA clear + resolve */
	if (msaa) {
		struct img ms;
		VkClearColorValue mc = clear_of(CLEAR_MSAA);
		VkImageResolve res = {
			.srcSubresource = COLOR_LAYERS,
			.dstSubresource = COLOR_LAYERS,
			.extent = { w, h, 1 },
		};

		CHECK(img_create(v, &ms, w, h, FMT,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | usage, VK_SAMPLE_COUNT_4_BIT));
		e.kind = X_SOLID;
		e.clear = CLEAR_MSAA;
		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &ms, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdClearColorImage(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mc, 1,
				     &COLOR_RANGE);
		img_barrier(cb, &ms, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdResolveImage(cb, ms.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.i,
				  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
		CHECK(cmd_submit_wait(v, cb));
		snprintf(what, sizeof(what), "%ux%u MSAA 4x resolve", w, h);
		CHECK(readback_check(v, r, &dst, rb, what, &e));
	}
	return R_PASS;
}

enum result test_xfer(struct vk *v, const struct opts *o, struct report *r)
{
	static const uint32_t sizes[2][2] = { { 137, 91 }, { 256, 256 } };
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	bool blit = can_blit(v), msaa = can_msaa4(v);
	struct buf rb;

	CHECK(buf_create(v, &rb, 256 * 256 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	for (uint32_t f = 0; f < frames; f++) {
		for (uint32_t s = 0; s < 2; s++) {
			uint32_t w = sizes[s][0], h = sizes[s][1];

			if (o->width && s == 1) {       /* --size replaces the POT case */
				w = o->width;
				h = o->height;
				if ((uint64_t)w * h > 256 * 256 || w < 18 || h < 22) {
					snprintf(r->detail, sizeof(r->detail),
						 "xfer --size must be 18x22 .. 256x256 pixels");
					return R_SETUP;
				}
			}
			CHECK(xfer_size(v, r, &rb, w, h, seed + f * 13u + s, blit, msaa));
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "137x91 + %ux%u: upload, offset copy%s%s; %u rounds", o->width ? o->width : 256,
		 o->width ? o->height : 256, blit ? ", mirrored blit" : "",
		 msaa ? ", MSAA resolve" : "", frames);
	return R_PASS;
}
