// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * image-copy:
 *  1. 3D image (16x16x8) sub-volume vkCmdCopyImage;
 *  2. 2D array (32x32 x 4 layers) multi-layer vkCmdCopyImage;
 *  3. format-compatible copy: R32_UINT -> R8G8B8A8_UNORM (same 32-bit texel size).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

static enum result make_img3d(struct vk *v, VkImage *im, uint32_t w, uint32_t h, uint32_t d,
			      VkFormat fmt, VkImageUsageFlags usage)
{
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = fmt,
		.extent = { w, h, d },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements req;
	VkDeviceMemory mem;

	VK_CHECK(v, vkCreateImage(v->dev, &ici, nullptr, im));
	vk_track(v, OBJ_IMAGE, H2U(*im));
	vkGetImageMemoryRequirements(v->dev, *im, &req);
	uint32_t mt = 0;

	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (v->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mt = i;
			break;
		}
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = mt,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, &mem));
	vk_track(v, OBJ_MEMORY, H2U(mem));
	VK_CHECK(v, vkBindImageMemory(v->dev, *im, mem, 0));
	return R_PASS;
}

static void raw_barrier(VkCommandBuffer cb, VkImage im, uint32_t layers, VkImageLayout from,
			VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
			VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = sa,
		.dstAccessMask = da,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = im,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers },
	};
	vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

enum result test_image_copy(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	VkCommandBuffer cb;

	/* 1. 3D image sub-volume copy (16x16x8) */
	{
		constexpr uint32_t W = 16;
		constexpr uint32_t H = 16;
		constexpr uint32_t D = 8;
		VkImage s3, d3;
		struct buf stg, sent, rb;

		CHECK(make_img3d(v, &s3, W, H, D, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
		CHECK(make_img3d(v, &d3, W, H, D, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
		CHECK(buf_create(v, &stg, W * H * D * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &sent, W * H * D * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &rb, W * H * D * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

		for (uint32_t z = 0; z < D; z++)
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					size_t off = ((size_t)(z * H + y) * W + x) * 4;
					uint32_t c = lcg(seed + x + y * 31u + z * 997u, 2);
					uint32_t s = 0x11223344u;

					memcpy(stg.map + off, &c, 4);
					memcpy(sent.map + off, &s, 4);
				}

		VkBufferImageCopy full = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { W, H, D },
		};
		VkImageCopy sub = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.srcOffset = { 2, 3, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstOffset = { 4, 5, 2 },
			.extent = { 10, 9, 5 },
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
		raw_barrier(cb, s3, 1, VK_IMAGE_LAYOUT_UNDEFINED,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		raw_barrier(cb, d3, 1, VK_IMAGE_LAYOUT_UNDEFINED,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cb, stg.b, s3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
		vkCmdCopyBufferToImage(cb, sent.b, d3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
		raw_barrier(cb, s3, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		raw_barrier(cb, d3, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImage(cb, s3, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       d3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sub);
		raw_barrier(cb, d3, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImageToBuffer(cb, d3, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &full);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		for (uint32_t z = 0; z < D; z++)
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					size_t off = ((size_t)(z * H + y) * W + x) * 4;
					bool in = x >= 4 && x < 14 && y >= 5 && y < 14 &&
						  z >= 2 && z < 7;
					uint32_t got, exp = 0x11223344u;

					if (in)
						exp = lcg(seed + (x - 2) + (y - 2) * 31u +
							  (z - 1) * 997u, 2);
					memcpy(&got, rb.map + off, 4);
					r->checked++;
					if (got != exp) {
						r->mismatches++;
						snprintf(r->detail, sizeof(r->detail),
							 "3D copy (%u,%u,%u): got 0x%08x exp 0x%08x",
							 x, y, z, got, exp);
						return R_MISMATCH;
					}
				}
	}

	/* 2. Multi-layer 2D array copy (32x32 x 4 layers: src[1..2] -> dst[2..3]) */
	{
		constexpr uint32_t W = 32;
		constexpr uint32_t H = 32;
		constexpr uint32_t L = 4;
		struct img sa, da;
		struct buf stg, sent, rb;

		CHECK(img_create_layers(v, &sa, W, H, L, VK_FORMAT_R8G8B8A8_UNORM,
					VK_IMAGE_USAGE_TRANSFER_DST_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
		CHECK(img_create_layers(v, &da, W, H, L, VK_FORMAT_R8G8B8A8_UNORM,
					VK_IMAGE_USAGE_TRANSFER_DST_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
		CHECK(buf_create(v, &stg, W * H * L * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &sent, W * H * L * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &rb, W * H * L * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

		for (uint32_t l = 0; l < L; l++)
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					size_t off = ((size_t)(l * H + y) * W + x) * 4;
					uint32_t c = lcg(seed + l * 4099u + y * 61u + x, 3);
					uint32_t s = 0x55667788u;

					memcpy(stg.map + off, &c, 4);
					memcpy(sent.map + off, &s, 4);
				}

		VkBufferImageCopy all = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, L },
			.imageExtent = { W, H, 1 },
		};
		VkImageCopy cpy = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 2 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 2 },
			.extent = { W, H, 1 },
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
		img_barrier(cb, &sa, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		img_barrier(cb, &da, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cb, stg.b, sa.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &all);
		vkCmdCopyBufferToImage(cb, sent.b, da.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &all);
		img_barrier(cb, &sa, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		img_barrier(cb, &da, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImage(cb, sa.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       da.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
		img_barrier(cb, &da, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImageToBuffer(cb, da.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &all);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		for (uint32_t l = 0; l < L; l++)
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					size_t off = ((size_t)(l * H + y) * W + x) * 4;
					uint32_t got, exp = 0x55667788u;

					if (l >= 2)
						exp = lcg(seed + (l - 1) * 4099u + y * 61u + x, 3);
					memcpy(&got, rb.map + off, 4);
					r->checked++;
					if (got != exp) {
						r->mismatches++;
						snprintf(r->detail, sizeof(r->detail),
							 "array copy L%u (%u,%u): got 0x%08x exp 0x%08x",
							 l, x, y, got, exp);
						return R_MISMATCH;
					}
				}
	}

	/* 3. Format-compatible copy: R32_UINT -> R8G8B8A8_UNORM (32x32) */
	{
		constexpr uint32_t W = 32;
		constexpr uint32_t H = 32;
		struct img iu, ic;
		struct buf stg, rb;

		CHECK(img_create(v, &iu, W, H, VK_FORMAT_R32_UINT,
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(img_create(v, &ic, W, H, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(buf_create(v, &stg, W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &rb, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

		for (uint32_t i = 0; i < W * H; i++) {
			uint32_t val = lcg(seed + i, 4);

			memcpy(stg.map + i * 4, &val, 4);
		}
		VkBufferImageCopy bic = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { W, H, 1 },
		};
		VkImageCopy cpy = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.extent = { W, H, 1 },
		};
		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &iu, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		img_barrier(cb, &ic, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cb, stg.b, iu.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
		img_barrier(cb, &iu, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImage(cb, iu.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       ic.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
		img_barrier(cb, &ic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		cmd_copy_to_buffer(cb, &ic, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		for (uint32_t i = 0; i < W * H; i++) {
			uint32_t got, exp = lcg(seed + i, 4);

			memcpy(&got, rb.map + i * 4, 4);
			r->checked++;
			if (got != exp) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "R32_UINT->RGBA8 texel %u: got 0x%08x exp 0x%08x",
					 i, got, exp);
				return R_MISMATCH;
			}
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "3D sub-volume (16x16x8), 2D array (4 layers), R32_UINT->RGBA8 copy");
	return R_PASS;
}
