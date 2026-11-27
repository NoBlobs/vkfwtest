// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * multiframe (pre-3d): several frames on one device and one framebuffer, so
 * the driver alternates the two HWRTData of the render target. Each frame
 * draws a full-screen triangle of its own colour (or, with --iters, a per-pixel
 * LCG result), is read back and checked pixel by pixel.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"

struct color_pc {
	uint32_t color, iters, seed;
};

struct mf_expect {
	struct color_pc pc;
};

static struct rgba mf_expect_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct mf_expect *e = ctx;

	if (!e->pc.iters)
		return rgba_of(e->pc.color);
	return rgba_of(lcg(e->pc.seed + x + y * 65536u, e->pc.iters) | 0xFF000000u);
}

enum result test_multiframe(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 256), h = OPT(o->height, 256);
	uint32_t frames = OPT(o->frames, 8), seed = OPT(o->seed, 1);
	struct img color;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkShaderModule vs, fs;

	CHECK(img_create(v, &color, w, h, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		struct mf_expect e = {
			.pc = {
				/* a different opaque colour every frame */
				.color = (lcg(seed + f, 3) & 0x00FFFFFFu) | 0xFF000000u,
				.iters = o->iters,
				.seed = seed + f * 977u,
			},
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
		char what[32];

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(e.pc), &e.pc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, mf_expect_fn, &e));
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u, %u frames%s", w, h, frames,
		 o->iters ? ", LCG per pixel" : "");
	return R_PASS;
}
