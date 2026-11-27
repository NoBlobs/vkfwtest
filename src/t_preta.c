// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * pre-ta: geometry-heavy frames. Layers of tiny tiled triangles (100 k by
 * default) without depth: later layers overwrite earlier ones, so every pixel
 * must end with the last layer's colour for its cell.
 */
#include <inttypes.h>
#include <stdio.h>

#include "vkfwtest.h"

enum result test_preta(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 512), h = OPT(o->height, 512);
	uint32_t frames = OPT(o->frames, 4), seed = OPT(o->seed, 1);
	struct grid g;
	struct img color;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;

	grid_plan(&g, w, h, OPT(o->tris, 100000), 4);
	CHECK(img_create(v, &color, w, h, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(grid_pipeline(v, &pipe, &layout, rp, false));

	for (uint32_t f = 0; f < frames; f++) {
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
		char what[32];

		g.seed = seed + f * 7919u;
		g.final_layer = g.layers - 1;
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		grid_draw(cb, layout, &g, false);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, grid_expect, &g));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u, %" PRIu64 " triangles (%u layers of %ux%u cells), %u frames", w, h,
		 grid_tris(&g), g.layers, g.cells, g.cells, frames);
	return R_PASS;
}
