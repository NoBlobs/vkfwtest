// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * ds-copy: vkCmdClearDepthStencilImage and depth/stencil image <-> buffer
 * copies.
 *  1. D32_SFLOAT 64x64: clear to 0.375f -> copy to buffer -> verify;
 *  2. D32_SFLOAT 64x64: upload exact binary floats -> copy back -> verify;
 *  3. D24S8 or D32S8 (when supported for transfer): clear stencil to 0x5A ->
 *     copy stencil aspect to buffer -> verify.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

constexpr uint32_t W = 64;
constexpr uint32_t H = 64;

enum result test_ds_copy(struct vk *v, const struct opts *o, struct report *r)
{
	(void)o;
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_D32_SFLOAT, &fp);
	if (!(fp.optimalTilingFeatures &
	      (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))) {
		snprintf(r->detail, sizeof(r->detail), "D32_SFLOAT transfer not supported");
		return R_SKIP;
	}

	struct img dimg;
	struct buf stg, rb;
	VkCommandBuffer cb;

	CHECK(img_create(v, &dimg, W, H, VK_FORMAT_D32_SFLOAT,
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &stg, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	CHECK(buf_create(v, &rb, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	/* 1. vkCmdClearDepthStencilImage -> copy depth to buffer */
	VkClearDepthStencilValue clr = { .depth = 0.375f, .stencil = 0 };
	VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	VkBufferImageCopy bic = {
		.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 },
	};
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = rb.b,
		.size = VK_WHOLE_SIZE,
	};

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &dimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    0, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdClearDepthStencilImage(cb, dimg.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
	img_barrier(cb, &dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(cb, dimg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &bic);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	for (uint32_t i = 0; i < W * H; i++) {
		float got;

		memcpy(&got, rb.map + i * 4, 4);
		r->checked++;
		if (got != 0.375f) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "D32 clear: texel %u got %g expected 0.375", i, (double)got);
			return R_MISMATCH;
		}
	}

	/* 2. D32_SFLOAT buffer -> image -> buffer round-trip */
	static const float vals[4] = { 0.125f, 0.25f, 0.5f, 0.75f };

	for (uint32_t i = 0; i < W * H; i++) {
		float val = vals[(i + (i / W)) & 3u];

		memcpy(stg.map + i * 4, &val, 4);
	}
	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &dimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyBufferToImage(cb, stg.b, dimg.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	img_barrier(cb, &dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(cb, dimg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &bic);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	for (uint32_t i = 0; i < W * H; i++) {
		float got, exp = vals[(i + (i / W)) & 3u];

		memcpy(&got, rb.map + i * 4, 4);
		r->checked++;
		if (got != exp) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "D32 round-trip: texel %u got %g expected %g", i, (double)got,
				 (double)exp);
			return R_MISMATCH;
		}
	}

	/* 3. Optional stencil clear + stencil copy to buffer */
	VkFormat ds_fmt = VK_FORMAT_UNDEFINED;
	static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };

	for (int i = 0; i < 2; i++) {
		vkGetPhysicalDeviceFormatProperties(v->phys, cand[i], &fp);
		if ((fp.optimalTilingFeatures &
		     (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
		    (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
			ds_fmt = cand[i];
			break;
		}
	}
	if (ds_fmt != VK_FORMAT_UNDEFINED) {
		constexpr uint32_t SW = 32;
		constexpr uint32_t SH = 32;
		struct img simg;
		struct buf srb;

		CHECK(img_create(v, &simg, SW, SH, ds_fmt,
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		simg.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		CHECK(buf_create(v, &srb, SW * SH, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
		memset(srb.map, 0, SW * SH);

		VkClearDepthStencilValue sclr = { .depth = 0.5f, .stencil = 0x5Au };
		VkImageSubresourceRange srng = { simg.aspect, 0, 1, 0, 1 };
		VkBufferImageCopy sbic = {
			.imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
			.imageExtent = { SW, SH, 1 },
		};
		hb.buffer = srb.b;
		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &simg, VK_IMAGE_LAYOUT_UNDEFINED,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdClearDepthStencilImage(cb, simg.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					    &sclr, 1, &srng);
		img_barrier(cb, &simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImageToBuffer(cb, simg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srb.b, 1,
				       &sbic);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		for (uint32_t i = 0; i < SW * SH; i++) {
			r->checked++;
			if (srb.map[i] != 0x5Au) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "stencil clear/copy: texel %u got 0x%02x expected 0x5a",
					 i, srb.map[i]);
				return R_MISMATCH;
			}
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "D32_SFLOAT clear + round-trip copy (%ux%u)%s", W, H,
		 ds_fmt != VK_FORMAT_UNDEFINED ? " + stencil clear/copy" : "");
	return R_PASS;
}
