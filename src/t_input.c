// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * input-attach: one render pass, two subpasses (two HWRTData in the driver).
 * Subpass 0 clears colour 0 to A and depth to 1, then draws the left half in
 * colour B at depth 0.5. Subpass 1 reads colour 0 and depth as input
 * attachments and writes colour 1 = (b, g, r, depth < 0.75 ? 1 : 0). Both
 * colour attachments are read back and checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "rect.vert.h"
#include "rect.frag.h"
#include "input.frag.h"

constexpr VkFormat CFMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat DFMT = VK_FORMAT_D32_SFLOAT;

struct rect_pc {
	float rect[4];
	uint32_t color;
	float z;
};
static_assert(sizeof(struct rect_pc) == 24);

struct ia_expect {
	uint32_t w, a, b;
	bool derived;           /* colour 1 (subpass 1) instead of colour 0 */
};

static struct rgba ia_expect_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct ia_expect *e = ctx;
	bool left = x < e->w / 2;
	struct rgba c = rgba_of(left ? e->b : e->a);

	(void)y;
	if (!e->derived)
		return c;
	return (struct rgba){ c.b, c.g, c.r, left ? 0xFF : 0x00 };
}

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

static enum result make_render_pass(struct vk *v, VkRenderPass *rp)
{
	VkAttachmentDescription a[3] = {
		{ /* 0: colour written in subpass 0, input in subpass 1 */
			.format = CFMT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		},
		{ /* 1: depth written in subpass 0, input in subpass 1 */
			.format = DFMT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		},
		{ /* 2: colour written in subpass 1 */
			.format = CFMT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		},
	};
	VkAttachmentReference c0 = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference d0 = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference in[2] = {
		{ 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
	};
	VkAttachmentReference c1 = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp[2] = {
		{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &c0,
			.pDepthStencilAttachment = &d0,
		},
		{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.inputAttachmentCount = 2,
			.pInputAttachments = in,
			.colorAttachmentCount = 1,
			.pColorAttachments = &c1,
		},
	};
	VkSubpassDependency dep[2] = {
		{
			.srcSubpass = 0,
			.dstSubpass = 1,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
					VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
					 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
		{
			.srcSubpass = 1,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		},
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 3,
		.pAttachments = a,
		.subpassCount = 2,
		.pSubpasses = sp,
		.dependencyCount = 2,
		.pDependencies = dep,
	};

	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(*rp));
	return R_PASS;
}

enum result test_input_attach(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 4), seed = OPT(o->seed, 1);
	struct img col0, depth, col1;
	struct buf rb0, rb1;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, DFMT, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "D32_SFLOAT depth not supported");
		return R_SKIP;
	}
	CHECK(img_create(v, &col0, w, h, CFMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &depth, w, h, DFMT,
			 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &col1, w, h, CFMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb0, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(buf_create(v, &rb1, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(make_render_pass(v, &rp));
	VkImageView views[3] = { col0.view, depth.view, col1.view };
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp,
		.attachmentCount = 3,
		.pAttachments = views,
		.width = w,
		.height = h,
		.layers = 1,
	};
	VK_CHECK(v, vkCreateFramebuffer(v->dev, &fci, nullptr, &fb));
	vk_track(v, OBJ_FRAMEBUFFER, H2U(fb));

	/* subpass 0: the rectangle */
	VkShaderModule rvs, rfs, fvs, ifs;
	VkPipelineLayout l0, l1;
	VkPipeline p0, p1;

	CHECK(shader_create(v, &rvs, spv_rect_vert, sizeof(spv_rect_vert)));
	CHECK(shader_create(v, &rfs, spv_rect_frag, sizeof(spv_rect_frag)));
	CHECK(layout_create(v, &l0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct rect_pc), nullptr));
	struct gfx_desc d0 = { .rp = rp, .layout = l0, .vs = rvs, .fs = rfs, .depth = true };
	CHECK(gfx_pipeline(v, &p0, &d0));

	/* subpass 1: input attachments */
	VkDescriptorSetLayoutBinding b[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
	};
	VkDescriptorSetLayoutCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = b,
	};
	VkDescriptorSetLayout dsl;
	VkDescriptorPool pool;
	VkDescriptorSet set;

	VK_CHECK(v, vkCreateDescriptorSetLayout(v->dev, &dci, nullptr, &dsl));
	vk_track(v, OBJ_SET_LAYOUT, H2U(dsl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2 };
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &ps,
	};
	VK_CHECK(v, vkCreateDescriptorPool(v->dev, &pci, nullptr, &pool));
	vk_track(v, OBJ_DESC_POOL, H2U(pool));
	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &dsl,
	};
	VK_CHECK(v, vkAllocateDescriptorSets(v->dev, &dai, &set));
	VkDescriptorImageInfo ii[2] = {
		{ VK_NULL_HANDLE, col0.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 2,
		.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
		.pImageInfo = ii,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
	CHECK(shader_create(v, &fvs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &ifs, spv_input_frag, sizeof(spv_input_frag)));
	CHECK(layout_create(v, &l1, 0, 0, &dsl));
	struct gfx_desc d1 = { .rp = rp, .layout = l1, .vs = fvs, .fs = ifs, .subpass = 1 };
	CHECK(gfx_pipeline(v, &p1, &d1));

	for (uint32_t f = 0; f < frames; f++) {
		struct ia_expect e = {
			.w = w,
			.a = (lcg(seed + f, 2) & 0x00FFFFFFu) | 0xFF000000u,
			.b = (lcg(seed + f, 5) & 0x00FFFFFFu) | 0xFF000000u,
		};
		VkClearValue clear[3] = {
			clear_rgba(e.a),
			{ .depthStencil = { 1.0f, 0 } },
			clear_rgba(0),
		};
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 3,
			.pClearValues = clear,
		};
		struct rect_pc pc = { { -1.0f, -1.0f, 0.0f, 1.0f }, e.b, 0.5f };
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;
		char what[48];

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p0);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, l0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				   sizeof(pc), &pc);
		vkCmdDraw(cb, 6, 1, 0, 0);
		vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p1);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, l1, 0, 1, &set, 0, nullptr);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &col0, &rb0);
		cmd_copy_to_buffer(cb, &col1, &rb1);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		snprintf(what, sizeof(what), "frame %u subpass 0 colour", f);
		CHECK(check_rgba(v, r, what, rb0.map, w, h, ia_expect_fn, &e));
		e.derived = true;
		snprintf(what, sizeof(what), "frame %u subpass 1 colour", f);
		CHECK(check_rgba(v, r, what, rb1.map, w, h, ia_expect_fn, &e));
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u, 2 subpasses (colour + D32 inputs), %u frames",
		 w, h, frames);
	return R_PASS;
}
