// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * local-read: dynamic rendering local read (core 1.4, or
 * VK_KHR_dynamic_rendering_local_read on top of dynamic rendering). One
 * rendering with two colour attachments in RENDERING_LOCAL_READ layout:
 *  draw 1 writes the left half of attachment 0 (C1 over the clear C0);
 *  a by-region barrier inside the rendering;
 *  draw 2 reads attachment 0 as an input attachment and writes it, swizzled,
 *  to attachment 1.
 * On a tiler this is the multi-pass-in-one-render path that input attachments
 * also take. Both attachments are checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "rect.vert.h"
#include "rect.frag.h"
#include "lr.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct rect_pc {
	float rect[4];
	uint32_t color;
	float z;
};

struct lr_expect {
	uint32_t w, c0, c1;
	bool swizzled;
};

static struct rgba lr_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct lr_expect *e = ctx;
	struct rgba c = rgba_of(x < e->w / 2 ? e->c1 : e->c0);

	(void)y;
	return e->swizzled ? (struct rgba){ c.b, c.g, c.r, 0xFF } : c;
}

enum result test_local_read(struct vk *v, const struct opts *o, struct report *r)
{
#ifdef VK_KHR_dynamic_rendering_local_read
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);

	if (!v->has_local_read) {
		snprintf(r->detail, sizeof(r->detail),
			 "needs dynamicRenderingLocalRead (Vulkan 1.4 or VK_KHR_dynamic_rendering_local_read)");
		return R_SKIP;
	}
	bool core = v->api >= VK_API_VERSION_1_3;
	PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(
		v->dev, core ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
	PFN_vkCmdEndRendering end = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(
		v->dev, core ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");

	if (!begin || !end) {
		snprintf(r->detail, sizeof(r->detail), "dynamic rendering entry points missing");
		return R_SKIP;
	}

	const VkImageLayout LR = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR;
	struct img a0, a1;
	struct buf rb0, rb1;
	VkShaderModule rvs, rfs, fvs, lfs;
	VkPipelineLayout l0, l1;
	VkPipeline p0, p1;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;

	CHECK(img_create(v, &a0, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &a1, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb0, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(buf_create(v, &rb1, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	VkDescriptorSetLayoutBinding b = {
		0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
	};
	CHECK(desc_create(v, &b, 1, &dsl, &set));
	VkDescriptorImageInfo ii = { VK_NULL_HANDLE, a0.view, LR };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
		.pImageInfo = &ii,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);

	const VkFormat fmts[2] = { FMT, FMT };
	VkPipelineRenderingCreateInfo pri = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 2,
		.pColorAttachmentFormats = fmts,
	};
	const VkColorComponentFlags m0[2] = { 0xF, 0 }, m1[2] = { 0, 0xF };

	CHECK(shader_create(v, &rvs, spv_rect_vert, sizeof(spv_rect_vert)));
	CHECK(shader_create(v, &rfs, spv_rect_frag, sizeof(spv_rect_frag)));
	CHECK(shader_create(v, &fvs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &lfs, spv_lr_frag, sizeof(spv_lr_frag)));
	CHECK(layout_create(v, &l0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct rect_pc), nullptr));
	CHECK(layout_create(v, &l1, 0, 0, &dsl));
	struct gfx_desc d = { .layout = l0, .vs = rvs, .fs = rfs, .colors = 2, .write_masks = m0,
			      .pnext = &pri };
	CHECK(gfx_pipeline(v, &p0, &d));
	d = (struct gfx_desc){ .layout = l1, .vs = fvs, .fs = lfs, .colors = 2, .write_masks = m1,
			       .pnext = &pri };
	CHECK(gfx_pipeline(v, &p1, &d));

	for (uint32_t f = 0; f < frames; f++) {
		struct lr_expect e = { w, (lcg(seed + f, 2) & 0x00FFFFFFu) | 0xFF000000u,
				       (lcg(seed + f, 3) & 0x00FFFFFFu) | 0xFF000000u, false };
		struct rgba c0 = rgba_of(e.c0);
		VkRenderingAttachmentInfo att[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = a0.view,
				.imageLayout = LR,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.clearValue.color.float32 = { c0.r / 255.0f, c0.g / 255.0f,
							      c0.b / 255.0f, c0.a / 255.0f },
			},
			{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = a1.view,
				.imageLayout = LR,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			},
		};
		VkRenderingInfo ri = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { { 0, 0 }, { w, h } },
			.layerCount = 1,
			.colorAttachmentCount = 2,
			.pColorAttachments = att,
		};
		VkMemoryBarrier mb = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
		};
		struct rect_pc pc = { { -1, -1, 0, 1 }, e.c1, 0.5f };
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;
		char what[48];

		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &a0, VK_IMAGE_LAYOUT_UNDEFINED, LR, 0,
			    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		img_barrier(cb, &a1, VK_IMAGE_LAYOUT_UNDEFINED, LR, 0,
			    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		begin(cb, &ri);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p0);
		vkCmdPushConstants(cb, l0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				   sizeof(pc), &pc);
		vkCmdDraw(cb, 6, 1, 0, 0);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT,
				     1, &mb, 0, nullptr, 0, nullptr);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p1);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, l1, 0, 1, &set, 0, nullptr);
		vkCmdDraw(cb, 3, 1, 0, 0);
		end(cb);
		img_barrier(cb, &a0, LR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		img_barrier(cb, &a1, LR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		cmd_copy_to_buffer(cb, &a0, &rb0);
		cmd_copy_to_buffer(cb, &a1, &rb1);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u attachment 0", f);
		CHECK(check_rgba(v, r, what, rb0.map, w, h, lr_fn, &e));
		e.swizzled = true;
		snprintf(what, sizeof(what), "frame %u attachment 1 (local read)", f);
		CHECK(check_rgba(v, r, what, rb1.map, w, h, lr_fn, &e));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u: one rendering, draw -> by-region barrier -> local read; %u frames", w, h,
		 frames);
	return R_PASS;
#else
	(void)v;
	(void)o;
	snprintf(r->detail, sizeof(r->detail), "built with Vulkan headers without local read");
	return R_SKIP;
#endif
}
