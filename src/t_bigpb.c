// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * bigpb: millions of tiny triangles in one render pass into 1024x1024 colour
 * plus D32 depth (cleared and stored), enough to run the parameter buffer out
 * of memory: free-list grows and partial renders. Every later layer is farther
 * away (depth test LESS), so the first layer must remain everywhere. A partial
 * render that loses depth or colour shows up as wrong pixels.
 */
#include <inttypes.h>
#include <stdio.h>

#include "vkfwtest.h"

enum result test_bigpb(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 1024), h = OPT(o->height, 1024);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	struct grid g;
	struct img color, depth;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_D32_SFLOAT, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "D32_SFLOAT depth not supported");
		return R_SKIP;
	}
	grid_plan(&g, w, h, OPT(o->tris, 4000000), 2);
	CHECK(img_create(v, &color, w, h, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &depth, w, h, VK_FORMAT_D32_SFLOAT,
			 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT));
	CHECK(fb_create(v, &fb, rp, &color, &depth));
	CHECK(grid_pipeline(v, &pipe, &layout, rp, true));

	for (uint32_t f = 0; f < frames; f++) {
		VkClearValue clear[2] = {
			{ .color.float32 = { 0, 0, 0, 0 } },
			{ .depthStencil = { 1.0f, 0 } },
		};
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 2,
			.pClearValues = clear,
		};
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;
		char what[32];

		g.seed = seed + f * 104729u;
		g.final_layer = 0;
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		grid_draw(cb, layout, &g, true);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, grid_expect, &g));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u + D32, %" PRIu64 " triangles (%u layers of %ux%u cells), %u frames", w, h,
		 grid_tris(&g), g.layers, g.cells, g.cells, frames);
	return R_PASS;
}
