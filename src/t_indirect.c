// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * indirect: draws and dispatches whose parameters come from a GPU buffer.
 *  1. vkCmdDrawIndirect (at a non-zero offset) draws the first K cells of a
 *     grid, vkCmdDrawIndexedIndirect the next K through an index buffer;
 *     with multiDrawIndirect, a third, two-command draw adds 2K more;
 *  2. vkCmdDispatchIndirect runs G workgroups over a buffer pre-filled with
 *     a sentinel: the first G*64 words get the LCG pattern, the rest must keep
 *     the sentinel.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fill.comp.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t SENTINEL = 0xA5A5A5A5u;

struct ind_expect {
	struct grid g;
	uint32_t cells_drawn;
};

static struct rgba ind_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct ind_expect *e = ctx;
	uint32_t cx = x / (e->g.w / e->g.cells), cy = y / (e->g.h / e->g.cells);

	if (cy * e->g.cells + cx >= e->cells_drawn)
		return rgba_of(0);
	return grid_expect(x, y, &e->g);
}

static void put_u32(uint8_t *p, uint32_t x)
{
	memcpy(p, &x, 4);
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t x;

	memcpy(&x, p, 4);
	return x;
}

static enum result indirect_draws(struct vk *v, struct report *r, uint32_t seed, uint32_t f)
{
	uint32_t w = 128, h = 128;
	struct ind_expect e;
	struct img color;
	struct buf rb, args, idx;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	uint32_t k;
	char what[48];

	grid_plan(&e.g, w, h, 2 * 16 * 16, 4);  /* 16x16 cells, one layer */
	e.g.seed = seed;
	e.g.final_layer = 0;
	k = e.g.cells * e.g.cells / 5;          /* cells per draw command */
	e.cells_drawn = v->has_multidraw ? 4 * k : 2 * k;
	CHECK(img_create(v, &color, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(grid_pipeline(v, &pipe, &layout, rp, false));

	/* args: [16] DrawIndirect, [64] DrawIndexedIndirect, [128] 2 x DrawIndirect */
	CHECK(buf_create(v, &args, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true));
	memset(args.map, 0, 256);
	put_u32(args.map + 16, k * 6);          /* vertexCount */
	put_u32(args.map + 20, 1);              /* instanceCount */
	put_u32(args.map + 64, k * 6);          /* indexCount */
	put_u32(args.map + 68, 1);
	put_u32(args.map + 72, 0);              /* firstIndex */
	put_u32(args.map + 76, k * 6);          /* vertexOffset: cells k .. 2k */
	for (uint32_t i = 0; i < 2; i++) {
		put_u32(args.map + 128 + i * 16, k * 6);
		put_u32(args.map + 132 + i * 16, 1);
		put_u32(args.map + 136 + i * 16, (2 + i) * k * 6);     /* firstVertex */
	}
	CHECK(buf_create(v, &idx, (VkDeviceSize)k * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true));
	for (uint32_t i = 0; i < k * 6; i++)
		put_u32(idx.map + i * 4, i);

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
	struct { uint32_t cells, layers, seed, depth; } pc = { e.g.cells, 1, seed, 0 };

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			   sizeof(pc), &pc);
	vkCmdDrawIndirect(cb, args.b, 16, 1, 0);
	vkCmdBindIndexBuffer(cb, idx.b, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cb, args.b, 64, 1, 0);
	if (v->has_multidraw)
		vkCmdDrawIndirect(cb, args.b, 128, 2, 16);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	snprintf(what, sizeof(what), "frame %u indirect draws", f);
	return check_rgba(v, r, what, rb.map, w, h, ind_fn, &e);
}

static enum result indirect_dispatch(struct vk *v, struct report *r, uint32_t seed, uint32_t f,
				     uint32_t groups)
{
	uint32_t words = groups * 2 * 64;
	struct buf b, args;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSet set;
	VkCommandBuffer cb;
	uint64_t bad = 0;
	struct { uint32_t seed, iters; } pc = { seed, 5 };

	CHECK(buf_create(v, &b, (VkDeviceSize)words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	CHECK(buf_create(v, &args, 64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true));
	for (uint32_t i = 0; i < words; i++)
		put_u32(b.map + (size_t)i * 4, SENTINEL);
	memset(args.map, 0, 64);
	put_u32(args.map + 32, groups);         /* x, at offset 32 */
	put_u32(args.map + 36, 1);
	put_u32(args.map + 40, 1);
	CHECK(compute_pipeline(v, spv_fill_comp, sizeof(spv_fill_comp), sizeof(pc), &b, &layout,
			       &pipe, &set));
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = b.b,
		.size = VK_WHOLE_SIZE,
	};

	CHECK(cmd_begin(v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatchIndirect(cb, args.b, 32);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
			     0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t got = get_u32(b.map + (size_t)i * 4);
		uint32_t want = i < groups * 64 ? lcg(i ^ seed, 5) : SENTINEL;

		if (got == want)
			continue;
		if (bad < 8 && v->verbose >= 0)
			fprintf(stderr, "vkfwtest: frame %u indirect dispatch: word %u got %08x"
				" expected %08x\n", f, i, got, want);
		bad++;
	}
	r->checked += words;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail),
			 "frame %u indirect dispatch: %" PRIu64 " of %u words wrong", f, bad, words);
		return R_MISMATCH;
	}
	return R_PASS;
}

enum result test_indirect(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1), groups = OPT(o->groups, 37);

	for (uint32_t f = 0; f < frames; f++) {
		CHECK(indirect_draws(v, r, seed + f, f));
		CHECK(indirect_dispatch(v, r, seed + f * 3u, f, groups));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "draw / indexed%s indirect into a grid, dispatch indirect of %u groups; %u frames",
		 v->has_multidraw ? " / multi-draw" : "", groups, frames);
	return R_PASS;
}
