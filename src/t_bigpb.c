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

/*
 * bigpb-layers: the same workload in a 4-layer colour + D32 array. The grid
 * goes to layer 0 (no gl_Layer), and layers 1-3 get a clear of their own
 * colour, so with render-target arrays one render covers all layers and the
 * partial renders the parameter buffer forces must keep every layer.
 */
constexpr uint32_t BL = 4;

static uint32_t bl_color(uint32_t layer, uint32_t seed)
{
	return (lcg(layer * 7919u + seed, 5) & 0x00FFFFFFu) | 0xFF000000u;
}

static struct rgba bl_solid(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(*(const uint32_t *)ctx);
}

enum result test_bigpb_layers(struct vk *v, const struct opts *o, struct report *r)
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
	struct rp_desc rd = { .color = VK_FORMAT_R8G8B8A8_UNORM, .depth = VK_FORMAT_D32_SFLOAT };

	vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_D32_SFLOAT, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "D32_SFLOAT depth not supported");
		return R_SKIP;
	}
	grid_plan(&g, w, h, OPT(o->tris, 4000000), 2);
	CHECK(img_create_layers(v, &color, w, h, BL, VK_FORMAT_R8G8B8A8_UNORM,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(img_create_layers(v, &depth, w, h, BL, VK_FORMAT_D32_SFLOAT,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * BL, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			 true));
	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { color.view, depth.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, BL));
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
		uint32_t s = seed + f * 104729u, lc[BL];
		VkCommandBuffer cb;
		char what[48];

		g.seed = s;
		g.final_layer = 0;
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		grid_draw(cb, layout, &g, true);
		for (uint32_t l = 1; l < BL; l++) {
			struct rgba c;

			lc[l] = bl_color(l, s);
			c = rgba_of(lc[l]);
			VkClearAttachment ca = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.clearValue.color.float32 = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
							      c.a / 255.0f },
			};
			VkClearRect cr = { sc, l, 1 };

			vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
		}
		vkCmdEndRenderPass(cb);
		for (uint32_t l = 0; l < BL; l++)
			cmd_copy_layer_to_buffer(cb, &color, l, &rb, (VkDeviceSize)l * w * h * 4);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u layer 0 (grid)", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, grid_expect, &g));
		for (uint32_t l = 1; l < BL; l++) {
			snprintf(what, sizeof(what), "frame %u layer %u (clear)", f, l);
			CHECK(check_rgba(v, r, what, rb.map + (size_t)l * w * h * 4, w, h, bl_solid,
					 &lc[l]));
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u x %u layers + D32, %" PRIu64 " triangles on layer 0, clears on 1-%u, %u frames",
		 w, h, BL, grid_tris(&g), BL - 1, frames);
	return R_PASS;
}
