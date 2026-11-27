// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * multiview: VK_KHR_multiview (core 1.1). One draw writes a distinct colour
 * into each of 2 array layers using gl_ViewIndex. Every pixel of both layers
 * is checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "mv.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct mv_pc {
	uint32_t c0, c1;
};

struct solid {
	uint32_t c;
};

static struct rgba solid_fn(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(((const struct solid *)ctx)->c);
}

enum result test_multiview(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	struct img im;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;

	if (!v->has_multiview) {
		snprintf(r->detail, sizeof(r->detail), "multiview feature not supported");
		return R_SKIP;
	}
	CHECK(img_create_layers(v, &im, w, h, 2, FMT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	VkAttachmentDescription a = {
		.format = FMT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &cref,
	};
	VkSubpassDependency dep = {
		.srcSubpass = 0,
		.dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	uint32_t view_mask = 0x3u, corr_mask = 0x3u;
	VkRenderPassMultiviewCreateInfo mv = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
		.subpassCount = 1,
		.pViewMasks = &view_mask,
		.correlationMaskCount = 1,
		.pCorrelationMasks = &corr_mask,
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.pNext = &mv,
		.attachmentCount = 1,
		.pAttachments = &a,
		.subpassCount = 1,
		.pSubpasses = &sp,
		.dependencyCount = 1,
		.pDependencies = &dep,
	};
	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, &rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(rp));
	/* multiview framebuffers must have layers == 1 */
	CHECK(fb_create_views(v, &fb, rp, &im.view, 1, w, h, 1));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_mv_frag, sizeof(spv_mv_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct mv_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 73u;
		struct mv_pc pc = {
			.c0 = (lcg(s, 2) & 0x00FFFFFFu) | 0xFF000000u,
			.c1 = (lcg(s, 5) & 0x00FFFFFFu) | 0xFF000000u,
		};
		VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_layer_to_buffer(cb, &im, 0, &rb, 0);
		cmd_copy_layer_to_buffer(cb, &im, 1, &rb, (VkDeviceSize)w * h * 4);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		struct solid s0 = { pc.c0 }, s1 = { pc.c1 };
		char what[32];

		snprintf(what, sizeof(what), "frame %u view 0", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, solid_fn, &s0));
		snprintf(what, sizeof(what), "frame %u view 1", f);
		CHECK(check_rgba(v, r, what, rb.map + (size_t)w * h * 4, w, h, solid_fn, &s1));
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u x 2 views (gl_ViewIndex), %u frames", w, h, frames);
	return R_PASS;
}
