// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * mrt: up to 8 colour attachments in one subpass (the pixel back end's words
 * for every render target). Output i is a hash of (i, 4x4 block, seed); every
 * pixel of every attachment is checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "mrt.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct mrt_expect {
	uint32_t i, seed;
};

static struct rgba mrt_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct mrt_expect *e = ctx;
	uint32_t bx = x / 4, by = y / 4;
	uint32_t h = (bx * 73856093u) ^ (by * 19349663u) ^ (e->i * 83492791u) ^ e->seed;

	h ^= h >> 16;
	return rgba_of(h);
}

enum result test_mrt(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 96), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	uint32_t n = v->props.limits.maxColorAttachments;
	struct img color[8];
	struct buf rb[8];
	VkImageView views[8];
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;

	if (n > v->props.limits.maxFragmentOutputAttachments)
		n = v->props.limits.maxFragmentOutputAttachments;
	if (n > 8)
		n = 8;
	if (n < 2) {
		snprintf(r->detail, sizeof(r->detail), "only %u colour attachment(s)", n);
		return R_SKIP;
	}
	for (uint32_t i = 0; i < n; i++) {
		CHECK(img_create(v, &color[i], w, h, FMT,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(buf_create(v, &rb[i], (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				 true));
		views[i] = color[i].view;
	}
	struct rp_desc rd = { .color = FMT, .colors = n };

	CHECK(rp_build(v, &rp, &rd));
	CHECK(fb_create_views(v, &fb, rp, views, n, w, h, 1));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_mrt_frag, sizeof(spv_mrt_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs, .colors = n };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 7919u;
		VkClearValue clear[8] = { 0 };
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = n,
			.pClearValues = clear,
		};
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(s), &s);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		for (uint32_t i = 0; i < n; i++)
			cmd_copy_to_buffer(cb, &color[i], &rb[i]);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		for (uint32_t i = 0; i < n; i++) {
			struct mrt_expect e = { i, s };
			char what[40];

			snprintf(what, sizeof(what), "frame %u attachment %u", f, i);
			CHECK(check_rgba(v, r, what, rb[i].map, w, h, mrt_fn, &e));
		}
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u, %u colour attachments, %u frames", w, h, n,
		 frames);
	return R_PASS;
}
