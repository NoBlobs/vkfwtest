// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * blit-mips:
 *  1. scaled LINEAR blits: 64x64 four-quadrant image upscaled to 128x128 and
 *     downscaled to 32x32;
 *  2. mip-chain generation via vkCmdBlitImage (LINEAR) across 7 mip levels
 *     (64x64 -> 1x1) from a checkerboard mip 0 whose 2x2 average is exact.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t W = 64;
constexpr uint32_t H = 64;
constexpr uint32_t MIPS = 7;

struct quad_exp {
	uint32_t c[4];
	uint32_t w, h;
};

static struct rgba quad_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct quad_exp *e = ctx;
	uint32_t q = (y >= e->h / 2 ? 2u : 0u) + (x >= e->w / 2 ? 1u : 0u);

	return rgba_of(e->c[q]);
}

struct solid {
	uint32_t c;
};

static struct rgba solid_fn(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(((const struct solid *)ctx)->c);
}

static void mip_barrier(VkCommandBuffer cb, VkImage img, uint32_t mip, VkImageLayout from,
			VkImageLayout to, VkAccessFlags src_a, VkAccessFlags dst_a)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_a,
		.dstAccessMask = dst_a,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 0, nullptr, 0, nullptr, 1, &b);
}

enum result test_blit_mips(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, FMT, &fp);
	VkFormatFeatureFlags req = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
				   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if ((fp.optimalTilingFeatures & req) != req) {
		snprintf(r->detail, sizeof(r->detail), "RGBA8 linear blit not supported");
		return R_SKIP;
	}

	/* 1. Scaled LINEAR blits: 64x64 -> 128x128 and 64x64 -> 32x32 */
	struct img src, up, dn;
	struct buf stg, rb_up, rb_dn;
	VkCommandBuffer cb;
	uint32_t qcol[4];

	for (int i = 0; i < 4; i++)
		qcol[i] = (lcg(seed + (uint32_t)i, 3) & 0x00FEFEFEu) | 0xFF000000u;

	CHECK(img_create(v, &src, W, H, FMT,
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &up, W * 2, H * 2, FMT,
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &dn, W / 2, H / 2, FMT,
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &stg, W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	CHECK(buf_create(v, &rb_up, (W * 2) * (H * 2) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(buf_create(v, &rb_dn, (W / 2) * (H / 2) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t q = (y >= H / 2 ? 2u : 0u) + (x >= W / 2 ? 1u : 0u);
			struct rgba c = rgba_of(qcol[q]);
			uint8_t *p = stg.map + ((size_t)y * W + x) * 4;

			p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
		}

	VkBufferImageCopy bic = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 },
	};
	VkImageBlit b_up = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { (int32_t)W, (int32_t)H, 1 } },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { (int32_t)(W * 2), (int32_t)(H * 2), 1 } },
	};
	VkImageBlit b_dn = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { (int32_t)W, (int32_t)H, 1 } },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { (int32_t)(W / 2), (int32_t)(H / 2), 1 } },
	};

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    0, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &up, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    0, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &dn, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    0, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyBufferToImage(cb, stg.b, src.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	img_barrier(cb, &src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdBlitImage(cb, src.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		       up.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b_up, VK_FILTER_LINEAR);
	vkCmdBlitImage(cb, src.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		       dn.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b_dn, VK_FILTER_LINEAR);
	img_barrier(cb, &up, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &dn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	cmd_copy_to_buffer(cb, &up, &rb_up);
	cmd_copy_to_buffer(cb, &dn, &rb_dn);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	/* Downscale 32x32: every 2x2 block is uniform inside its quadrant */
	struct quad_exp qdn = { { qcol[0], qcol[1], qcol[2], qcol[3] }, W / 2, H / 2 };

	CHECK(check_rgba_tol(v, r, "downscale 32x32", rb_dn.map, W / 2, H / 2, 1, quad_fn, &qdn));

	/* Upscale 128x128: check interior away from the 2-pixel seam at W and H */
	uint64_t bad = 0, checked = 0;

	for (uint32_t y = 0; y < H * 2; y++)
		for (uint32_t x = 0; x < W * 2; x++) {
			if ((x >= W - 2 && x < W + 2) || (y >= H - 2 && y < H + 2))
				continue;
			uint32_t q = (y >= H ? 2u : 0u) + (x >= W ? 1u : 0u);
			struct rgba e = rgba_of(qcol[q]);
			const uint8_t *p = rb_up.map + ((size_t)y * (W * 2) + x) * 4;

			checked++;
			if (p[0] == e.r && p[1] == e.g && p[2] == e.b && p[3] == e.a)
				continue;
			if (bad < 8 && v->verbose >= 0)
				fprintf(stderr, "vkfwtest: upscale 128x128: (%u,%u) got %02x%02x%02x%02x"
					" expected %02x%02x%02x%02x\n",
					x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
			bad++;
		}
	r->checked += checked;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail),
			 "upscale 128x128: %" PRIu64 " of %" PRIu64 " pixels wrong", bad, checked);
		return R_MISMATCH;
	}

	/* 2. Mip-chain generation (7 levels: 64 -> 1) */
	VkImage mip_img;
	VkDeviceMemory mip_mem;
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FMT,
		.extent = { W, H, 1 },
		.mipLevels = MIPS,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VK_CHECK(v, vkCreateImage(v->dev, &ici, nullptr, &mip_img));
	vk_track(v, OBJ_IMAGE, H2U(mip_img));
	VkMemoryRequirements reqm;

	vkGetImageMemoryRequirements(v->dev, mip_img, &reqm);
	uint32_t mt = 0;

	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((reqm.memoryTypeBits & (1u << i)) &&
		    (v->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mt = i;
			break;
		}
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqm.size,
		.memoryTypeIndex = mt,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, &mip_mem));
	vk_track(v, OBJ_MEMORY, H2U(mip_mem));
	VK_CHECK(v, vkBindImageMemory(v->dev, mip_img, mip_mem, 0));

	/* Checkerboard of two even colours: every 2x2 averages to mid */
	struct rgba a = rgba_of(qcol[0]), b = rgba_of(qcol[1]);
	struct solid mid = { (uint32_t)((a.r + b.r) / 2) | ((uint32_t)((a.g + b.g) / 2) << 8) |
			     ((uint32_t)((a.b + b.b) / 2) << 16) | 0xFF000000u };

	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			struct rgba c = ((x + y) & 1u) ? b : a;
			uint8_t *p = stg.map + ((size_t)y * W + x) * 4;

			p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
		}

	struct buf rb_mip;

	CHECK(buf_create(v, &rb_mip, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	CHECK(cmd_begin(v, &cb));
	VkImageMemoryBarrier init_b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = mip_img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, 1 },
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 0, nullptr, 0, nullptr, 1, &init_b);
	vkCmdCopyBufferToImage(cb, stg.b, mip_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	mip_barrier(cb, mip_img, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

	VkDeviceSize off = 0, mip_off[MIPS] = { 0 };

	for (uint32_t m = 1; m < MIPS; m++) {
		uint32_t sw = W >> (m - 1), sh = H >> (m - 1);
		uint32_t dw = W >> m, dh = H >> m;
		VkImageBlit mb = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1 },
			.srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
			.dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } },
		};
		vkCmdBlitImage(cb, mip_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       mip_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mb,
			       VK_FILTER_LINEAR);
		mip_barrier(cb, mip_img, m, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy mc = {
			.bufferOffset = off,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
			.imageExtent = { dw, dh, 1 },
		};
		vkCmdCopyImageToBuffer(cb, mip_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_mip.b,
				       1, &mc);
		mip_off[m] = off;
		off += (VkDeviceSize)dw * dh * 4;
	}
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = rb_mip.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	for (uint32_t m = 1; m < MIPS; m++) {
		char what[32];

		snprintf(what, sizeof(what), "mip %u (%ux%u)", m, W >> m, H >> m);
		CHECK(check_rgba_tol(v, r, what, rb_mip.map + mip_off[m], W >> m, H >> m, 1,
				     solid_fn, &mid));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "LINEAR blits (64->128, 64->32) + %u-level mip chain", MIPS);
	return R_PASS;
}
