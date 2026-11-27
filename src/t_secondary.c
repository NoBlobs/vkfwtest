// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * secondary: secondary command buffers (VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS)
 * and vkCmdClearAttachments with partial rectangles between draws.
 *  - secondary 0: full-screen draw of colour C0;
 *  - secondary 1: vkCmdClearAttachments of a centred partial rectangle to C1;
 *  - secondary 2: scissored draw of an inner rectangle to C2.
 * Every pixel is checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "pcolor.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct sec_exp {
	uint32_t c0, c1, c2;
	uint32_t x1, y1, x2, y2;
	uint32_t ix1, iy1, ix2, iy2;
};

static struct rgba sec_expect_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct sec_exp *e = ctx;

	if (x >= e->ix1 && x < e->ix2 && y >= e->iy1 && y < e->iy2)
		return rgba_of(e->c2);
	if (x >= e->x1 && x < e->x2 && y >= e->y1 && y < e->y2)
		return rgba_of(e->c1);
	return rgba_of(e->c0);
}

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

enum result test_secondary(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	struct img color;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;

	if (w < 16 || h < 16) {
		snprintf(r->detail, sizeof(r->detail), "--size must be at least 16x16");
		return R_SETUP;
	}
	CHECK(img_create(v, &color, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_pcolor_frag, sizeof(spv_pcolor_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 97u;
		struct sec_exp e = {
			.c0 = (lcg(s, 2) & 0x00FFFFFFu) | 0xFF000000u,
			.c1 = (lcg(s, 3) & 0x00FFFFFFu) | 0xFF000000u,
			.c2 = (lcg(s, 4) & 0x00FFFFFFu) | 0xFF000000u,
			.x1 = w / 8, .y1 = h / 8, .x2 = w - w / 8, .y2 = h - h / 8,
			.ix1 = w / 4, .iy1 = h / 4, .ix2 = w - w / 4, .iy2 = h - h / 4,
		};
		VkCommandBuffer sec[3], cb;
		VkCommandBufferAllocateInfo sai = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = v->pool,
			.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
			.commandBufferCount = 3,
		};
		VkCommandBufferInheritanceInfo inh = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
			.renderPass = rp,
			.subpass = 0,
			.framebuffer = fb,
		};
		VkCommandBufferBeginInfo sbi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
				 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			.pInheritanceInfo = &inh,
		};
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc_full = { { 0, 0 }, { w, h } };
		VkRect2D sc_in = { { (int32_t)e.ix1, (int32_t)e.iy1 },
				   { e.ix2 - e.ix1, e.iy2 - e.iy1 } };

		VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &sai, sec));

		/* secondary 0: full-screen draw C0 */
		VK_CHECK(v, vkBeginCommandBuffer(sec[0], &sbi));
		vkCmdBindPipeline(sec[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(sec[0], 0, 1, &vp);
		vkCmdSetScissor(sec[0], 0, 1, &sc_full);
		vkCmdPushConstants(sec[0], layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &e.c0);
		vkCmdDraw(sec[0], 3, 1, 0, 0);
		VK_CHECK(v, vkEndCommandBuffer(sec[0]));

		/* secondary 1: partial clear attachment to C1 */
		VK_CHECK(v, vkBeginCommandBuffer(sec[1], &sbi));
		VkClearAttachment ca = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.colorAttachment = 0,
			.clearValue = clear_rgba(e.c1),
		};
		VkClearRect cr = {
			.rect = { { (int32_t)e.x1, (int32_t)e.y1 }, { e.x2 - e.x1, e.y2 - e.y1 } },
			.baseArrayLayer = 0,
			.layerCount = 1,
		};
		vkCmdClearAttachments(sec[1], 1, &ca, 1, &cr);
		VK_CHECK(v, vkEndCommandBuffer(sec[1]));

		/* secondary 2: scissored inner draw C2 */
		VK_CHECK(v, vkBeginCommandBuffer(sec[2], &sbi));
		vkCmdBindPipeline(sec[2], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(sec[2], 0, 1, &vp);
		vkCmdSetScissor(sec[2], 0, 1, &sc_in);
		vkCmdPushConstants(sec[2], layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &e.c2);
		vkCmdDraw(sec[2], 3, 1, 0, 0);
		VK_CHECK(v, vkEndCommandBuffer(sec[2]));

		VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
		vkCmdExecuteCommands(cb, 3, sec);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		enum result res = cmd_submit_wait(v, cb);

		if (res == R_PASS)
			vkFreeCommandBuffers(v->dev, v->pool, 3, sec);
		CHECK(res);
		r->frames++;
		char what[32];

		snprintf(what, sizeof(what), "frame %u", f);
		CHECK(check_rgba(v, r, what, rb.map, w, h, sec_expect_fn, &e));
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u, 3 secondary command buffers (draw + partial clear + draw), %u frames",
		 w, h, frames);
	return R_PASS;
}
