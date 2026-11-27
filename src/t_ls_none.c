// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * load-store-none: attachments the render pass must neither load nor store
 * (STORE_OP_NONE: core 1.3; LOAD_OP_NONE: core 1.4, or VK_KHR/EXT_load_store_op_none).
 * Attachment 0 is drawn as usual (LOAD, a rectangle, STORE). Attachment 1
 * holds a per-pixel pattern uploaded before the pass; it is bound but never
 * written (write mask 0), with STORE_OP_NONE and, when available,
 * LOAD_OP_NONE. Its pattern must survive the pass untouched.
 * A firmware that stores (or loads and clears) every attachment breaks it.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "rect.vert.h"
#include "rect.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct rect_pc {
	float rect[4];
	uint32_t color;
	float z;
};

static uint32_t pattern(uint32_t x, uint32_t y, uint32_t seed)
{
	return lcg(seed + x + y * 65536u, 1) | 0xFF000000u;
}

struct lsn_expect {
	uint32_t w, seed, c0, c1;
	bool drawn;             /* attachment 0, else the untouched attachment 1 */
};

static struct rgba lsn_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct lsn_expect *e = ctx;

	if (e->drawn)
		return rgba_of(x < e->w / 2 ? e->c1 : e->c0);
	return rgba_of(pattern(x, y, e->seed));
}

static enum result one_pass(struct vk *v, struct report *r, bool load_none, uint32_t w,
			    uint32_t h, uint32_t seed)
{
	struct img a0, a1;
	struct buf up, rb0, rb1;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
					VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	struct lsn_expect e = { w, seed, 0xFF204060u, (lcg(seed, 4) & 0x00FFFFFFu) | 0xFF000000u,
				true };

	CHECK(img_create(v, &a0, w, h, FMT, usage, VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &a1, w, h, FMT, usage, VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &up, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	CHECK(buf_create(v, &rb0, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(buf_create(v, &rb1, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			struct rgba p = rgba_of(pattern(x, y, seed));
			uint8_t *q = up.map + ((size_t)y * w + x) * 4;

			q[0] = p.r, q[1] = p.g, q[2] = p.b, q[3] = p.a;
		}

	VkAttachmentDescription at[2] = {
		{
			.format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		},
		{
			.format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = load_none ? VK_ATTACHMENT_LOAD_OP_NONE_EXT : VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_NONE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		},
	};
	VkAttachmentReference refs[2] = {
		{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
	};
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 2,
		.pColorAttachments = refs,
	};
	VkSubpassDependency dep[2] = {
		{ VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
		  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
		{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		  VK_ACCESS_TRANSFER_READ_BIT, 0 },
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = at,
		.subpassCount = 1,
		.pSubpasses = &sp,
		.dependencyCount = 2,
		.pDependencies = dep,
	};
	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, &rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(rp));
	VkImageView views[2] = { a0.view, a1.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, 1));
	CHECK(shader_create(v, &vs, spv_rect_vert, sizeof(spv_rect_vert)));
	CHECK(shader_create(v, &fs, spv_rect_frag, sizeof(spv_rect_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct rect_pc), nullptr));
	const VkColorComponentFlags masks[2] = { 0xF, 0 };
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs, .colors = 2,
			      .write_masks = masks };
	CHECK(gfx_pipeline(v, &pipe, &d));

	struct rgba c0 = rgba_of(e.c0);
	VkClearColorValue cc = { .float32 = { c0.r / 255.0f, c0.g / 255.0f, c0.b / 255.0f,
					      c0.a / 255.0f } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				  .imageExtent = { w, h, 1 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
	};
	struct rect_pc pc = { { -1, -1, 0, 1 }, e.c1, 0.5f };
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &a0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	img_barrier(cb, &a1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdClearColorImage(cb, a0.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
	vkCmdCopyBufferToImage(cb, up.b, a1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			   sizeof(pc), &pc);
	vkCmdDraw(cb, 6, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &a0, &rb0);
	cmd_copy_to_buffer(cb, &a1, &rb1);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	CHECK(check_rgba(v, r, "drawn attachment", rb0.map, w, h, lsn_fn, &e));
	e.drawn = false;
	return check_rgba(v, r, load_none ? "LOAD_OP_NONE + STORE_OP_NONE attachment" :
					    "STORE_OP_NONE attachment",
			  rb1.map, w, h, lsn_fn, &e);
}

enum result test_load_store_none(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64), seed = OPT(o->seed, 1);

	if (!v->has_store_none) {
		snprintf(r->detail, sizeof(r->detail),
			 "needs STORE_OP_NONE (Vulkan 1.3 or VK_KHR/EXT_load_store_op_none)");
		return R_SKIP;
	}
	CHECK(one_pass(v, r, false, w, h, seed));
	if (v->has_load_none)
		CHECK(one_pass(v, r, true, w, h, seed + 1));
	snprintf(r->detail, sizeof(r->detail), "%ux%u: STORE_OP_NONE%s attachment kept intact", w, h,
		 v->has_load_none ? " and LOAD_OP_NONE + STORE_OP_NONE" : "");
	return R_PASS;
}
