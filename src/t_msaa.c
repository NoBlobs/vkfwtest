// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * msaa: a 4x render resolved at the end of the render pass (resolve
 * attachment). A triangle covers the pixels above the main diagonal (x > y):
 * those must be C1, those below C0. On the diagonal, the standard 4x sample
 * positions put exactly 2 of 4 samples inside, so the resolve gives
 * (C1 + C0) / 2 (+-1); with non-standard positions the diagonal is skipped.
 */
#include <inttypes.h>
#include <stdio.h>

#include "vkfwtest.h"

#include "poly.vert.h"
#include "poly.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct poly_pc {
	float p01[4], p2[4];
	uint32_t color;
};

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

static bool close8(uint8_t a, uint8_t b)
{
	return (a > b ? a - b : b - a) <= 1;
}

enum result test_msaa(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	uint32_t h = w;         /* square: the diagonal runs corner to corner */
	bool standard = v->props.limits.standardSampleLocations;
	struct img ms, res;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;

	if (!(v->props.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "4x colour multisampling not supported");
		return R_SKIP;
	}
	CHECK(img_create(v, &ms, w, h, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			 VK_SAMPLE_COUNT_4_BIT));
	CHECK(img_create(v, &res, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	struct rp_desc rd = { .color = FMT, .samples = VK_SAMPLE_COUNT_4_BIT, .resolve = true };

	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { ms.view, res.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, 1));
	CHECK(shader_create(v, &vs, spv_poly_vert, sizeof(spv_poly_vert)));
	CHECK(shader_create(v, &fs, spv_poly_frag, sizeof(spv_poly_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct poly_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs,
			      .samples = VK_SAMPLE_COUNT_4_BIT };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		/* even channels: the 2-of-4 average is an integer */
		uint32_t c0 = (lcg(seed + f, 2) & 0x00FEFEFEu) | 0xFF000000u;
		uint32_t c1 = (lcg(seed + f, 3) & 0x00FEFEFEu) | 0xFF000000u;
		VkClearValue clear = clear_rgba(c0);
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		struct poly_pc pc = { { -1, -1, 1, -1 }, { 1, 1, 0, 0 }, c1 };
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;
		uint64_t bad = 0, checked = 0;

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, sizeof(pc), &pc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &res, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		struct rgba a = rgba_of(c0), b = rgba_of(c1);
		struct rgba mid = { (uint8_t)((a.r + b.r) / 2), (uint8_t)((a.g + b.g) / 2),
				    (uint8_t)((a.b + b.b) / 2), 0xFF };

		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				const uint8_t *p = rb.map + ((size_t)y * w + x) * 4;
				struct rgba e = x > y ? b : x < y ? a : mid;
				bool ok;

				if (x == y && !standard)
					continue;
				checked++;
				ok = x == y ? close8(p[0], e.r) && close8(p[1], e.g) &&
						      close8(p[2], e.b) && close8(p[3], e.a)
					    : p[0] == e.r && p[1] == e.g && p[2] == e.b && p[3] == e.a;
				if (ok)
					continue;
				if (bad < 8 && v->verbose >= 0)
					fprintf(stderr,
						"vkfwtest: frame %u: (%u,%u) got %02x%02x%02x%02x expected"
						" %02x%02x%02x%02x\n",
						f, x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
				bad++;
			}
		r->checked += checked;
		r->mismatches += bad;
		if (bad) {
			snprintf(r->detail, sizeof(r->detail),
				 "frame %u: %" PRIu64 " of %" PRIu64 " resolved pixels wrong", f, bad,
				 checked);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u 4x, resolve attachment, diagonal edge%s, %u frames", w, h,
		 standard ? " (standard sample positions)" : " (not checked: custom positions)",
		 frames);
	return R_PASS;
}
