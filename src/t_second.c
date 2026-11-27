// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * second-*: what makes the second render on a device fail. Every test does
 * the same two renders on a fresh device (a full-screen triangle of a solid
 * colour into a 64x64 RGBA8 target, read back and checked), and changes one
 * thing before the second:
 *
 *   second-same      the same framebuffer, a new command buffer (the minimal
 *                    multiframe);
 *   second-resubmit  the same command buffer, submitted again (the same
 *                    command stream; it clears the image to magenta with a
 *                    transfer first, since the colour is the same both times);
 *   second-newfb     a new image and framebuffer, same render pass and
 *                    pipeline;
 *   second-newall    a new image, framebuffer, render pass, pipeline layout
 *                    and pipeline (what indirect frame 1 does);
 *   second-compute   a compute dispatch between the renders, same framebuffer;
 *   second-xfer      a transfer (buffer fill + image clear) between them;
 *   second-split     same framebuffer, but the second render and its read-back
 *                    copy are two submissions, the copy sent only after the
 *                    render's fence signalled (no in-command-buffer ordering);
 *   second-barrier   same framebuffer; the copy is ordered after the render by
 *                    an explicit vkCmdPipelineBarrier as well as by the render
 *                    pass's external subpass dependency.
 *
 * The first render of each must pass; which second renders fail tells which
 * state change the firmware mishandles.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"
#include "fill.comp.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

enum variant { V_SAME, V_RESUBMIT, V_NEWFB, V_NEWALL, V_COMPUTE, V_XFER, V_SPLIT, V_BARRIER };

struct color_pc {
	uint32_t color, iters, seed;
};

struct gfx {
	VkRenderPass rp;
	VkPipelineLayout layout;
	VkPipeline pipe;
};

struct target {
	struct img img;
	VkFramebuffer fb;
};

static struct rgba solid_fn(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(*(const uint32_t *)ctx);
}

static enum result gfx_make(struct vk *v, struct gfx *g)
{
	VkShaderModule vs, fs;

	CHECK(rp_create(v, &g->rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &g->layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc d = { .rp = g->rp, .layout = g->layout, .vs = vs, .fs = fs };

	return gfx_pipeline(v, &g->pipe, &d);
}

static enum result target_make(struct vk *v, struct target *t, const struct gfx *g, uint32_t w,
			       uint32_t h)
{
	CHECK(img_create(v, &t->img, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	return fb_create(v, &t->fb, g->rp, &t->img, nullptr);
}

static bool explicit_barrier;   /* second-barrier: a pipeline barrier before the copy */

/* A render pass clearing to black, the triangle, and (rb != NULL) a copy into `rb`. */
static void record(VkCommandBuffer cb, const struct gfx *g, const struct target *t,
		   const struct color_pc *pc, const struct buf *rb)
{
	uint32_t w = t->img.w, h = t->img.h;
	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = g->rp,
		.framebuffer = t->fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };

	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*pc), pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	if (explicit_barrier)
		img_barrier(cb, &t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT);
	if (rb)
		cmd_copy_to_buffer(cb, &t->img, rb);
}

/* The render, waited for; then the read-back copy in a submission of its own. */
static enum result render_split(struct vk *v, struct report *r, const char *what,
				const struct gfx *g, const struct target *t, uint32_t color,
				const struct buf *rb)
{
	struct color_pc pc = { color, 0, 0 };
	VkCommandBuffer cb;

	memset(rb->map, 0x5A, rb->size);
	CHECK(cmd_begin(v, &cb));
	record(cb, g, t, &pc, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	CHECK(cmd_begin(v, &cb));
	cmd_copy_to_buffer(cb, &t->img, rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	return check_rgba(v, r, what, rb->map, t->img.w, t->img.h, solid_fn, &color);
}

static enum result render(struct vk *v, struct report *r, const char *what, const struct gfx *g,
			  const struct target *t, uint32_t color, const struct buf *rb)
{
	struct color_pc pc = { color, 0, 0 };
	VkCommandBuffer cb;

	memset(rb->map, 0x5A, rb->size);
	CHECK(cmd_begin(v, &cb));
	record(cb, g, t, &pc, rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	return check_rgba(v, r, what, rb->map, t->img.w, t->img.h, solid_fn, &color);
}

/* A reusable command buffer (no ONE_TIME_SUBMIT), submitted twice. */
static enum result resubmit(struct vk *v, struct report *r, const struct gfx *g,
			    const struct target *t, uint32_t color, const struct buf *rb)
{
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = v->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	struct color_pc pc = { color, 0, 0 };
	VkCommandBuffer cb;

	VkClearColorValue magenta = { .float32 = { 1, 0, 1, 1 } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &cai, &cb));
	VK_CHECK(v, vkBeginCommandBuffer(cb, &bi));
	/*
	 * The image is cleared to magenta first, so a second render that writes
	 * nothing shows (the colour is the same both times).
	 */
	img_barrier(cb, &t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdClearColorImage(cb, t->img.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
	img_barrier(cb, &t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	record(cb, g, t, &pc, rb);
	VK_CHECK(v, vkEndCommandBuffer(cb));
	for (uint32_t i = 0; i < 2; i++) {
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cb,
		};

		memset(rb->map, 0x5A, rb->size);
		VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
		VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si, v->fence));
		VK_CHECK(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, v->timeout_ns));
		r->frames++;
		CHECK(check_rgba(v, r, i ? "second render (same command buffer)" : "first render",
				 rb->map, t->img.w, t->img.h, solid_fn, &color));
	}
	vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
	return R_PASS;
}

/* A compute dispatch writing an LCG pattern, checked on the CPU. */
static enum result compute(struct vk *v, struct report *r)
{
	constexpr uint32_t GROUPS = 8;
	constexpr uint32_t WORDS = GROUPS * 64;
	struct buf b;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSet set;
	VkCommandBuffer cb;
	struct { uint32_t seed, iters; } pc = { 1234, 7 };

	CHECK(buf_create(v, &b, WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(b.map, 0, WORDS * 4);
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
	vkCmdDispatch(cb, GROUPS, 1, 1);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
			     0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	for (uint32_t i = 0; i < WORDS; i++) {
		uint32_t got;

		memcpy(&got, b.map + (size_t)i * 4, 4);
		r->checked++;
		if (got != lcg(i ^ pc.seed, pc.iters)) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "the compute dispatch between the renders: word %u wrong", i);
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

/* A transfer: fill a buffer and clear the target image (the next render clears it again). */
static enum result transfer(struct vk *v, struct report *r, struct target *t)
{
	struct buf b;
	VkCommandBuffer cb;
	VkClearColorValue cc = { .float32 = { 1, 0, 1, 1 } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	CHECK(buf_create(v, &b, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	memset(b.map, 0, 4096);
	CHECK(cmd_begin(v, &cb));
	vkCmdFillBuffer(cb, b.b, 0, 4096, 0xC0FFEE11u);
	img_barrier(cb, &t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdClearColorImage(cb, t->img.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = b.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
			     nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	for (uint32_t i = 0; i < 1024; i++) {
		uint32_t got;

		memcpy(&got, b.map + (size_t)i * 4, 4);
		r->checked++;
		if (got != 0xC0FFEE11u) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "the transfer between the renders: word %u is %08x", i, got);
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

static enum result second_run(struct vk *v, const struct opts *o, struct report *r,
			      enum variant var, const char *name)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64), seed = OPT(o->seed, 1);
	uint32_t c0 = (lcg(seed, 3) & 0x00FFFFFFu) | 0xFF000000u;
	uint32_t c1 = (lcg(seed + 1, 3) & 0x00FFFFFFu) | 0xFF000000u;
	struct gfx g, g2;
	struct target t, t2;
	struct buf rb;

	explicit_barrier = var == V_BARRIER;
	copy_barrier = false;   /* only the subpass dependency (or the explicit barrier) */
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(gfx_make(v, &g));
	CHECK(target_make(v, &t, &g, w, h));
	if (var == V_RESUBMIT) {
		CHECK(resubmit(v, r, &g, &t, c0, &rb));
		snprintf(r->detail, sizeof(r->detail), "%ux%u: %s", w, h, name);
		return R_PASS;
	}
	CHECK(render(v, r, "first render", &g, &t, c0, &rb));
	switch (var) {
	case V_NEWFB:
		CHECK(target_make(v, &t2, &g, w, h));
		CHECK(render(v, r, "second render (new framebuffer)", &g, &t2, c1, &rb));
		break;
	case V_NEWALL:
		CHECK(gfx_make(v, &g2));
		CHECK(target_make(v, &t2, &g2, w, h));
		CHECK(render(v, r, "second render (new render pass, pipeline, framebuffer)", &g2,
			     &t2, c1, &rb));
		break;
	case V_COMPUTE:
		CHECK(compute(v, r));
		CHECK(render(v, r, "second render (after compute)", &g, &t, c1, &rb));
		break;
	case V_SPLIT:
		CHECK(render_split(v, r, "second render (copy submitted after it)", &g, &t, c1, &rb));
		break;
	case V_XFER:
		CHECK(transfer(v, r, &t));
		CHECK(render(v, r, "second render (after a transfer)", &g, &t, c1, &rb));
		break;
	default:
		CHECK(render(v, r, "second render (same framebuffer)", &g, &t, c1, &rb));
		break;
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u: %s", w, h, name);
	return R_PASS;
}

static enum result second(struct vk *v, const struct opts *o, struct report *r, enum variant var,
			  const char *name)
{
	enum result e = second_run(v, o, r, var, name);

	copy_barrier = true;
	explicit_barrier = false;
	return e;
}

enum result test_second_same(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_SAME, "two renders, same framebuffer");
}

enum result test_second_resubmit(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_RESUBMIT, "one command buffer submitted twice");
}

enum result test_second_newfb(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_NEWFB, "second render into a new image + framebuffer");
}

enum result test_second_newall(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_NEWALL, "second render with new render pass, pipeline, framebuffer");
}

enum result test_second_compute(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_COMPUTE, "render, compute dispatch, render");
}

enum result test_second_xfer(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_XFER, "render, transfer (fill + clear image), render");
}

enum result test_second_split(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_SPLIT, "second render, read back by a later submission");
}

enum result test_second_barrier(struct vk *v, const struct opts *o, struct report *r)
{
	return second(v, o, r, V_BARRIER, "two renders, copy after an explicit pipeline barrier");
}
