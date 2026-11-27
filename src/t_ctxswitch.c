// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * ctx-switch: two devices, so two firmware contexts, take turns. In every
 * round device A renders and device B renders (even rounds) or runs a compute
 * dispatch (odd rounds); both are submitted before either is waited for, so
 * the firmware has kicks of two contexts queued back to back. Every round's
 * output is checked: a colour of its own per device and round, an LCG pattern
 * for the dispatch. --frames sets the rounds (default 4).
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"
#include "fill.comp.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t CWORDS = 8 * 64;

struct color_pc {
	uint32_t color, iters, seed;
};

struct side {
	struct vk *v;
	struct img img;
	struct buf rb, cbuf;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout, clayout;
	VkPipeline pipe, cpipe;
	VkDescriptorSet cset;
};

static struct rgba solid_fn(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(*(const uint32_t *)ctx);
}

static enum result side_make(struct side *s, uint32_t w, uint32_t h)
{
	struct vk *v = s->v;
	VkShaderModule vs, fs;

	CHECK(img_create(v, &s->img, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &s->rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &s->rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &s->fb, s->rp, &s->img, nullptr));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &s->layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc d = { .rp = s->rp, .layout = s->layout, .vs = vs, .fs = fs };

	CHECK(gfx_pipeline(v, &s->pipe, &d));
	CHECK(buf_create(v, &s->cbuf, CWORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	return compute_pipeline(v, spv_fill_comp, sizeof(spv_fill_comp), 2 * sizeof(uint32_t),
				&s->cbuf, &s->clayout, &s->cpipe, &s->cset);
}

static enum result submit_render(struct side *s, uint32_t color, VkFence *fence)
{
	uint32_t w = s->img.w, h = s->img.h;
	struct color_pc pc = { color, 0, 0 };
	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = s->rp,
		.framebuffer = s->fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };
	VkCommandBuffer cb;

	memset(s->rb.map, 0x5A, s->rb.size);
	CHECK(cmd_begin(s->v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, s->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &s->img, &s->rb);
	return cmd_submit_async(s->v, cb, fence);
}

static enum result submit_compute(struct side *s, uint32_t seed, VkFence *fence)
{
	struct { uint32_t seed, iters; } pc = { seed, 9 };
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = s->cbuf.b,
		.size = VK_WHOLE_SIZE,
	};
	VkCommandBuffer cb;

	memset(s->cbuf.map, 0, CWORDS * 4);
	CHECK(cmd_begin(s->v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->cpipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->clayout, 0, 1, &s->cset, 0,
				nullptr);
	vkCmdPushConstants(cb, s->clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cb, CWORDS / 64, 1, 1);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
			     0, nullptr, 1, &hb, 0, nullptr);
	return cmd_submit_async(s->v, cb, fence);
}

static enum result check_compute(struct report *r, const struct side *s, uint32_t seed,
				 const char *what)
{
	for (uint32_t i = 0; i < CWORDS; i++) {
		uint32_t got;

		memcpy(&got, s->cbuf.map + (size_t)i * 4, 4);
		r->checked++;
		if (got != lcg(i ^ seed, 9)) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "%s: word %u is %08x, expected %08x",
				 what, i, got, lcg(i ^ seed, 9));
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

static enum result rounds(struct side *a, struct side *b, const struct opts *o, struct report *r)
{
	uint32_t n = OPT(o->frames, 4), seed = OPT(o->seed, 1);

	for (uint32_t k = 0; k < n; k++) {
		uint32_t ca = (lcg(seed + 2 * k, 3) & 0x00FFFFFFu) | 0xFF000000u;
		uint32_t cb = (lcg(seed + 2 * k + 1, 3) & 0x00FFFFFFu) | 0xFF000000u;
		bool comp = k & 1;
		VkFence fa, fb;
		char what[64];

		CHECK(submit_render(a, ca, &fa));
		if (comp)
			CHECK(submit_compute(b, seed + k, &fb));
		else
			CHECK(submit_render(b, cb, &fb));
		CHECK(fence_wait(a->v, fa, a->v->timeout_ns));
		CHECK(fence_wait(b->v, fb, b->v->timeout_ns));
		r->frames += 2;
		snprintf(what, sizeof(what), "round %u device A render", k);
		CHECK(check_rgba(a->v, r, what, a->rb.map, a->img.w, a->img.h, solid_fn, &ca));
		if (comp) {
			snprintf(what, sizeof(what), "round %u device B compute", k);
			CHECK(check_compute(r, b, seed + k, what));
		} else {
			snprintf(what, sizeof(what), "round %u device B render", k);
			CHECK(check_rgba(b->v, r, what, b->rb.map, b->img.w, b->img.h, solid_fn, &cb));
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "2 devices, %u rounds: A renders, B renders / computes, both queued together", n);
	return R_PASS;
}

enum result test_ctx_switch(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	static struct vk v2;
	struct side a = { .v = v }, b = { .v = &v2 };
	enum result res;

	memset(&v2, 0, sizeof(v2));
	CHECK(vk_init(&v2, o));
	v2.test = v->test;
	res = side_make(&a, w, h);
	if (res == R_PASS)
		res = side_make(&b, w, h);
	if (res == R_PASS)
		res = rounds(&a, &b, o, r);
	vk_fini(&v2);
	return res;
}
