// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * sizes: unusual render-target sizes (1x1, odd, thin, not tile aligned, HD,
 * 4096x4096) with a per-pixel LCG value, so every pixel is unique and a wrong
 * tile or macrotile mapping shows. Then a partial render area: the image is
 * cleared to C0, a render pass with LOAD and a render area inside it draws,
 * and the pixels outside the area must keep C0.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct color_pc {
	uint32_t color, iters, seed;
};

struct area {
	uint32_t seed, c0;
	int32_t x0, y0, x1, y1;         /* render area; everything if x1 == 0 */
};

static struct rgba area_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct area *a = ctx;

	if (a->x1 && ((int32_t)x < a->x0 || (int32_t)x >= a->x1 || (int32_t)y < a->y0 ||
		      (int32_t)y >= a->y1))
		return rgba_of(a->c0);
	return rgba_of(lcg(a->seed + x + y * 65536u, 1) | 0xFF000000u);
}

static enum result one_size(struct vk *v, struct report *r, VkPipeline pipe,
			    VkPipelineLayout layout, VkRenderPass rp_clear, VkRenderPass rp_load,
			    uint32_t w, uint32_t h, uint32_t seed, bool partial)
{
	struct img color;
	struct buf rb;
	VkFramebuffer fb;
	VkCommandBuffer cb;
	struct area a = { .seed = seed, .c0 = 0xFF336699u };
	char what[64];

	CHECK(img_create(v, &color, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(fb_create(v, &fb, rp_clear, &color, nullptr));
	if (partial) {
		a.x0 = 17;
		a.y0 = 9;
		a.x1 = (int32_t)w - 33;
		a.y1 = (int32_t)h - 21;
	}
	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = partial ? rp_load : rp_clear,
		.framebuffer = fb,
		.renderArea = { { a.x0, a.y0 },
				{ partial ? (uint32_t)(a.x1 - a.x0) : w,
				  partial ? (uint32_t)(a.y1 - a.y0) : h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = rbi.renderArea;
	struct color_pc pc = { 0, 1, seed };

	CHECK(cmd_begin(v, &cb));
	if (partial) {
		struct rgba c = rgba_of(a.c0);
		VkClearColorValue cc = { .float32 = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
						      c.a / 255.0f } };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		img_barrier(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdClearColorImage(cb, color.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
	}
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	snprintf(what, sizeof(what), "%ux%u%s", w, h, partial ? " partial render area" : "");
	return check_rgba(v, r, what, rb.map, w, h, area_fn, &a);
}

enum result test_sizes(struct vk *v, const struct opts *o, struct report *r)
{
	static const uint32_t sizes[][2] = {
		{ 1, 1 }, { 7, 5 }, { 33, 17 }, { 97, 61 }, { 256, 1 }, { 1, 256 },
		{ 1000, 3 }, { 1920, 1080 }, { 4096, 4096 },
	};
	uint32_t seed = OPT(o->seed, 1), done = 0, skipped = 0;
	uint32_t maxw = v->props.limits.maxFramebufferWidth;
	uint32_t maxh = v->props.limits.maxFramebufferHeight;
	VkRenderPass rp_clear, rp_load;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;
	struct rp_desc rd = { .color = FMT };

	CHECK(rp_build(v, &rp_clear, &rd));
	rd.color_load = true;
	CHECK(rp_build(v, &rp_load, &rd));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc d = { .rp = rp_clear, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	if (o->width) {         /* --size: just that size, plus its partial area */
		CHECK(one_size(v, r, pipe, layout, rp_clear, rp_load, o->width, o->height, seed,
			       false));
		if (o->width > 64 && o->height > 40)
			CHECK(one_size(v, r, pipe, layout, rp_clear, rp_load, o->width, o->height,
				       seed, true));
		snprintf(r->detail, sizeof(r->detail), "%ux%u (+ partial render area)", o->width,
			 o->height);
		return R_PASS;
	}
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (sizes[i][0] > maxw || sizes[i][1] > maxh) {
			skipped++;
			continue;
		}
		CHECK(one_size(v, r, pipe, layout, rp_clear, rp_load, sizes[i][0], sizes[i][1],
			       seed + (uint32_t)i, false));
		done++;
	}
	CHECK(one_size(v, r, pipe, layout, rp_clear, rp_load, 300, 200, seed, true));
	snprintf(r->detail, sizeof(r->detail),
		 "%u sizes from 1x1 to 4096x4096%s, plus a partial render area in 300x200", done,
		 skipped ? " (some above the device limit skipped)" : "");
	return R_PASS;
}
