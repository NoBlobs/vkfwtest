// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * raster: rasterisation and render-pass edge cases.
 *  1. viewport with non-zero origin and negative height (VK 1.1 Y-flip);
 *  2. triangle strip with primitive restart (0xFFFF separating two quads);
 *  3. triangle fan;
 *  4. line strip;
 *  5. 4x MSAA sample mask (0x5: 2 of 4 samples enabled -> 50% blend);
 *  6. render pass with no attachments (fragment atomic counter, if supported).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "pos.vert.h"
#include "posz.vert.h"
#include "pcolor.frag.h"
#include "noatt.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t W = 64;
constexpr uint32_t H = 64;

struct box_exp {
	uint32_t c0, c1;
	uint32_t x1, y1, x2, y2;
	uint32_t bx1, by1, bx2, by2;    /* optional second box (all 0 if unused) */
};

static struct rgba box_expect_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct box_exp *e = ctx;

	if ((x >= e->x1 && x < e->x2 && y >= e->y1 && y < e->y2) ||
	    (x >= e->bx1 && x < e->bx2 && y >= e->by1 && y < e->by2))
		return rgba_of(e->c1);
	return rgba_of(e->c0);
}

struct solid {
	uint32_t c;
};

static struct rgba solid_fn(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(((const struct solid *)ctx)->c);
}

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

static enum result make_pipe(struct vk *v, VkPipeline *p, VkRenderPass rp, VkPipelineLayout layout,
			     VkShaderModule vs, VkShaderModule fs,
			     const VkPipelineVertexInputStateCreateInfo *vi,
			     VkPrimitiveTopology topo, bool restart, VkSampleCountFlagBits samples,
			     const VkSampleMask *mask, uint32_t colors)
{
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo empty_vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = topo,
		.primitiveRestartEnable = restart,
	};
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = samples,
		.pSampleMask = mask,
	};
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = colors,
		.pAttachments = colors ? &cba : nullptr,
	};
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dyn,
	};
	VkGraphicsPipelineCreateInfo gci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = st,
		.pVertexInputState = vi ? vi : &empty_vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pColorBlendState = &cb,
		.pDynamicState = &dy,
		.layout = layout,
		.renderPass = rp,
	};

	VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gci, nullptr, p));
	vk_track(v, OBJ_PIPELINE, H2U(*p));
	return R_PASS;
}

static float ndc(float px, uint32_t dim)
{
	return px * 2.0f / (float)dim - 1.0f;
}

enum result test_raster(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	uint32_t c0 = (lcg(seed, 2) & 0x00FEFEFEu) | 0xFF000000u;
	uint32_t c1 = (lcg(seed, 5) & 0x00FEFEFEu) | 0xFF000000u;
	struct img color;
	struct buf rb, vb, ib;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs_pos, vs_fs, fs_col;
	VkPipelineLayout layout;
	VkPipeline p_strip, p_fan, p_line;

	CHECK(img_create(v, &color, W, H, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs_pos, spv_pos_vert, sizeof(spv_pos_vert)));
	CHECK(shader_create(v, &vs_fs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs_col, spv_pcolor_frag, sizeof(spv_pcolor_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t), nullptr));

	/*
	 * Vertex buffer layout (all vec2 NDC):
	 *  [0..3]   quad covering NDC [-1, 1] x [-1, 0] (for negative-height viewport)
	 *  [4..7]   strip box 1: [8..28) x [8..28)
	 *  [8..11]  strip box 2: [36..56) x [36..56)
	 *  [12..15] fan box: [16..48) x [16..48) (CCW corners)
	 *  [16..18] line strip at y = 16.5 from x = 8.5 to x = 56.5
	 */
	float verts[19][2] = {
		{ -1, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 },
		{ ndc(8, W), ndc(8, H) },   { ndc(28, W), ndc(8, H) },
		{ ndc(8, W), ndc(28, H) },  { ndc(28, W), ndc(28, H) },
		{ ndc(36, W), ndc(36, H) }, { ndc(56, W), ndc(36, H) },
		{ ndc(36, W), ndc(56, H) }, { ndc(56, W), ndc(56, H) },
		{ ndc(16, W), ndc(16, H) }, { ndc(48, W), ndc(16, H) },
		{ ndc(48, W), ndc(48, H) }, { ndc(16, W), ndc(48, H) },
		{ ndc(8.5f, W), ndc(16.5f, H) }, { ndc(32.5f, W), ndc(16.5f, H) },
		{ ndc(56.5f, W), ndc(16.5f, H) },
	};
	static const uint16_t idx[9] = { 4, 5, 6, 7, 0xFFFFu, 8, 9, 10, 11 };

	CHECK(buf_create(v, &vb, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
	memcpy(vb.map, verts, sizeof(verts));
	CHECK(buf_create(v, &ib, sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true));
	memcpy(ib.map, idx, sizeof(idx));

	VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &bind,
		.vertexAttributeDescriptionCount = 1,
		.pVertexAttributeDescriptions = &attr,
	};
	CHECK(make_pipe(v, &p_strip, rp, layout, vs_pos, fs_col, &vi,
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true, VK_SAMPLE_COUNT_1_BIT, nullptr, 1));
	CHECK(make_pipe(v, &p_fan, rp, layout, vs_pos, fs_col, &vi,
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, false, VK_SAMPLE_COUNT_1_BIT, nullptr, 1));
	CHECK(make_pipe(v, &p_line, rp, layout, vs_pos, fs_col, &vi,
			VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, false, VK_SAMPLE_COUNT_1_BIT, nullptr, 1));

	VkClearValue clear = clear_rgba(c0);
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp_std = { 0, 0, (float)W, (float)H, 0, 1 };
	VkViewport vp_neg = { 8, 56, 48, -48, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkDeviceSize zero = 0;
	VkCommandBuffer cb;

	/* 1. viewport offset + negative height: NDC y in [-1, 0] -> fb y in [32, 56) */
	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_strip);
	vkCmdSetViewport(cb, 0, 1, &vp_neg);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
	vkCmdDraw(cb, 4, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	struct box_exp e = { c0, c1, 8, 32, 56, 56, 0, 0, 0, 0 };

	CHECK(check_rgba(v, r, "negative-height viewport", rb.map, W, H, box_expect_fn, &e));

	/* 2. triangle strip + primitive restart */
	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_strip);
	vkCmdSetViewport(cb, 0, 1, &vp_std);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero);
	vkCmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT16);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
	vkCmdDrawIndexed(cb, 9, 1, 0, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	e = (struct box_exp){ c0, c1, 8, 8, 28, 28, 36, 36, 56, 56 };
	CHECK(check_rgba(v, r, "strip + primitive restart", rb.map, W, H, box_expect_fn, &e));

	/* 3. triangle fan */
	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_fan);
	vkCmdSetViewport(cb, 0, 1, &vp_std);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
	vkCmdDraw(cb, 4, 1, 12, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	e = (struct box_exp){ c0, c1, 16, 16, 48, 48, 0, 0, 0, 0 };
	CHECK(check_rgba(v, r, "triangle fan", rb.map, W, H, box_expect_fn, &e));

	/* 4. line strip */
	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_line);
	vkCmdSetViewport(cb, 0, 1, &vp_std);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
	vkCmdDraw(cb, 3, 1, 16, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;
	{
		uint64_t bad = 0, checked = 0;
		struct rgba b0 = rgba_of(c0), b1 = rgba_of(c1);

		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++) {
				if (y == 16 && (x == 8 || x == 56))
					continue;       /* skip exact line endpoints */
				const uint8_t *p = rb.map + ((size_t)y * W + x) * 4;
				struct rgba ex = (y == 16 && x > 8 && x < 56) ? b1 : b0;

				checked++;
				if (p[0] == ex.r && p[1] == ex.g && p[2] == ex.b && p[3] == ex.a)
					continue;
				if (bad < 8 && v->verbose >= 0)
					fprintf(stderr, "vkfwtest: line strip: (%u,%u) got %02x%02x%02x%02x"
						" expected %02x%02x%02x%02x\n",
						x, y, p[0], p[1], p[2], p[3], ex.r, ex.g, ex.b, ex.a);
				bad++;
			}
		r->checked += checked;
		r->mismatches += bad;
		if (bad) {
			snprintf(r->detail, sizeof(r->detail),
				 "line strip: %" PRIu64 " of %" PRIu64 " pixels wrong", bad, checked);
			return R_MISMATCH;
		}
	}

	/* 5. sample mask (4x MSAA, 2 of 4 samples) */
	bool did_mask = false;

	if (v->props.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) {
		struct img ms, res;
		VkRenderPass rp_ms;
		VkFramebuffer fb_ms;
		VkPipeline p_mask;
		VkSampleMask mask = 0x5u;

		CHECK(img_create(v, &ms, W, H, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				 VK_SAMPLE_COUNT_4_BIT));
		CHECK(img_create(v, &res, W, H, FMT,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		struct rp_desc rd = { .color = FMT, .samples = VK_SAMPLE_COUNT_4_BIT, .resolve = true };

		CHECK(rp_build(v, &rp_ms, &rd));
		VkImageView views[2] = { ms.view, res.view };

		CHECK(fb_create_views(v, &fb_ms, rp_ms, views, 2, W, H, 1));
		CHECK(make_pipe(v, &p_mask, rp_ms, layout, vs_fs, fs_col, nullptr,
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, VK_SAMPLE_COUNT_4_BIT,
				&mask, 1));
		VkRenderPassBeginInfo mrbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp_ms,
			.framebuffer = fb_ms,
			.renderArea = { { 0, 0 }, { W, H } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &mrbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_mask);
		vkCmdSetViewport(cb, 0, 1, &vp_std);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &res, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		struct rgba a = rgba_of(c0), b = rgba_of(c1);
		struct solid mid = { (uint32_t)((a.r + b.r) / 2) | ((uint32_t)((a.g + b.g) / 2) << 8) |
				     ((uint32_t)((a.b + b.b) / 2) << 16) | 0xFF000000u };

		CHECK(check_rgba_tol(v, r, "sample mask 0x5", rb.map, W, H, 1, solid_fn, &mid));
		did_mask = true;
	}

	/* 6. render pass with no attachments */
	bool did_noatt = false;

	if (v->feat.fragmentStoresAndAtomics) {
		constexpr uint32_t NW = 32;
		constexpr uint32_t NH = 32;
		struct buf cnt;
		VkRenderPass rp_na;
		VkFramebuffer fb_na;
		VkShaderModule fs_na;
		VkDescriptorSetLayout dsl;
		VkDescriptorSet set;
		VkPipelineLayout l_na;
		VkPipeline p_na;

		CHECK(buf_create(v, &cnt, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
		memset(cnt.map, 0, 16);
		VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS };
		VkRenderPassCreateInfo rci = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.subpassCount = 1,
			.pSubpasses = &sp,
		};
		VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, &rp_na));
		vk_track(v, OBJ_RENDER_PASS, H2U(rp_na));
		CHECK(fb_create_views(v, &fb_na, rp_na, nullptr, 0, NW, NH, 1));
		CHECK(shader_create(v, &fs_na, spv_noatt_frag, sizeof(spv_noatt_frag)));
		VkDescriptorSetLayoutBinding dsb = {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		CHECK(desc_create(v, &dsb, 1, &dsl, &set));
		VkDescriptorBufferInfo dbi = { cnt.b, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet wr = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &dbi,
		};
		vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
		CHECK(layout_create(v, &l_na, 0, 0, &dsl));
		CHECK(make_pipe(v, &p_na, rp_na, l_na, vs_fs, fs_na, nullptr,
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, VK_SAMPLE_COUNT_1_BIT,
				nullptr, 0));

		VkRenderPassBeginInfo nrbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp_na,
			.framebuffer = fb_na,
			.renderArea = { { 0, 0 }, { NW, NH } },
		};
		VkViewport nvp = { 0, 0, (float)NW, (float)NH, 0, 1 };
		VkRect2D nsc = { { 0, 0 }, { NW, NH } };
		VkBufferMemoryBarrier hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = cnt.b,
			.size = VK_WHOLE_SIZE,
		};
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &nrbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_na);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, l_na, 0, 1, &set, 0, nullptr);
		vkCmdSetViewport(cb, 0, 1, &nvp);
		vkCmdSetScissor(cb, 0, 1, &nsc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		uint32_t got;

		memcpy(&got, cnt.map, 4);
		r->checked++;
		if (got != NW * NH) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "no-attachment pass: atomic count got %u expected %u", got, NW * NH);
			return R_MISMATCH;
		}
		did_noatt = true;
	}

	/* 7. Depth bias, depth clamp, depth bounds, wide lines, large points */
	bool did_dclamp = false, did_dbounds = false, did_wide = false, did_pts = false;
	bool did_ss = false, did_dyn = false;
	{
		struct img depth;
		VkRenderPass rp_d;
		VkFramebuffer fb_d;
		VkShaderModule vs_z;
		VkPipelineLayout l_z;
		struct { uint32_t color; float z; float ptsz; } zpc;
		struct buf zvb;
		static const float fsq[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
		static const float center_pt[2] = { 0.0f, 0.0f };

		CHECK(img_create(v, &depth, W, H, VK_FORMAT_D32_SFLOAT,
				 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(rp_create(v, &rp_d, FMT, VK_FORMAT_D32_SFLOAT));
		CHECK(fb_create(v, &fb_d, rp_d, &color, &depth));
		CHECK(shader_create(v, &vs_z, spv_posz_vert, sizeof(spv_posz_vert)));
		CHECK(layout_create(v, &l_z,
				    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				    sizeof(zpc), nullptr));
		CHECK(buf_create(v, &zvb, sizeof(fsq) + sizeof(center_pt),
				 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
		memcpy(zvb.map, fsq, sizeof(fsq));
		memcpy(zvb.map + sizeof(fsq), center_pt, sizeof(center_pt));

		VkPipelineShaderStageCreateInfo stz[2] = {
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_z, .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_col, .pName = "main" },
		};
		VkPipelineInputAssemblyStateCreateInfo iaz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
		};
		VkPipelineViewportStateCreateInfo vpz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .scissorCount = 1,
		};
		VkPipelineRasterizationStateCreateInfo rsz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
		};
		VkPipelineMultisampleStateCreateInfo msz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		VkPipelineDepthStencilStateCreateInfo dsz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_LESS,
		};
		VkPipelineColorBlendAttachmentState cbaz = { .colorWriteMask = 0xF };
		VkPipelineColorBlendStateCreateInfo cbz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1, .pAttachments = &cbaz,
		};
		VkDynamicState dynz[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dyz = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2, .pDynamicStates = dynz,
		};
		VkGraphicsPipelineCreateInfo gciz = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2, .pStages = stz, .pVertexInputState = &vi,
			.pInputAssemblyState = &iaz, .pViewportState = &vpz,
			.pRasterizationState = &rsz, .pMultisampleState = &msz,
			.pDepthStencilState = &dsz, .pColorBlendState = &cbz,
			.pDynamicState = &dyz, .layout = l_z, .renderPass = rp_d,
		};
		VkClearValue clrs[2] = { clear_rgba(c0), { .depthStencil = { 1.0f, 0 } } };
		VkRenderPassBeginInfo drbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp_d, .framebuffer = fb_d,
			.renderArea = { { 0, 0 }, { W, H } },
			.clearValueCount = 2, .pClearValues = clrs,
		};

		/* 7a. Depth bias: draw at z=0.5 with c0, then at z=0.5 with negative bias and c1 */
		VkPipeline p_nobias, p_bias;

		VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz, nullptr,
						      &p_nobias));
		vk_track(v, OBJ_PIPELINE, H2U(p_nobias));
		rsz.depthBiasEnable = VK_TRUE;
		rsz.depthBiasConstantFactor = -4.0f;
		VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz, nullptr,
						      &p_bias));
		vk_track(v, OBJ_PIPELINE, H2U(p_bias));
		rsz.depthBiasEnable = VK_FALSE;
		rsz.depthBiasConstantFactor = 0.0f;

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &drbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdSetViewport(cb, 0, 1, &vp_std);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdBindVertexBuffers(cb, 0, 1, &zvb.b, &zero);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_nobias);
		zpc = (typeof(zpc)){ c0, 0.5f, 1.0f };
		vkCmdPushConstants(cb, l_z, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, sizeof(zpc), &zpc);
		vkCmdDraw(cb, 4, 1, 0, 0);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_bias);
		zpc.color = c1;
		vkCmdPushConstants(cb, l_z, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, sizeof(zpc), &zpc);
		vkCmdDraw(cb, 4, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		struct solid sc1 = { c1 };

		CHECK(check_rgba(v, r, "depth bias", rb.map, W, H, solid_fn, &sc1));

		/* 7b. Depth clamp: z = 1.5 clamped to 1.0 passes LESS_OR_EQUAL against clear 1.0 */
		if (v->feat.depthClamp) {
			VkPipeline p_clamp;

			rsz.depthClampEnable = VK_TRUE;
			dsz.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz,
							      nullptr, &p_clamp));
			vk_track(v, OBJ_PIPELINE, H2U(p_clamp));
			rsz.depthClampEnable = VK_FALSE;
			dsz.depthCompareOp = VK_COMPARE_OP_LESS;

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &drbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp_std);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdBindVertexBuffers(cb, 0, 1, &zvb.b, &zero);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_clamp);
			zpc = (typeof(zpc)){ c1, 1.5f, 1.0f };
			vkCmdPushConstants(cb, l_z,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof(zpc), &zpc);
			vkCmdDraw(cb, 4, 1, 0, 0);
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			CHECK(check_rgba(v, r, "depth clamp", rb.map, W, H, solid_fn, &sc1));
			did_dclamp = true;
		}

		/* 7c. Depth bounds: clear depth=0.5; [0.25, 0.75] passes, [0.6, 0.9] fails */
		if (v->feat.depthBounds) {
			VkPipeline p_db_in, p_db_out;

			dsz.depthTestEnable = VK_FALSE;
			dsz.depthWriteEnable = VK_FALSE;
			dsz.depthBoundsTestEnable = VK_TRUE;
			dsz.minDepthBounds = 0.25f;
			dsz.maxDepthBounds = 0.75f;
			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz,
							      nullptr, &p_db_in));
			vk_track(v, OBJ_PIPELINE, H2U(p_db_in));
			dsz.minDepthBounds = 0.6f;
			dsz.maxDepthBounds = 0.9f;
			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz,
							      nullptr, &p_db_out));
			vk_track(v, OBJ_PIPELINE, H2U(p_db_out));
			dsz.depthBoundsTestEnable = VK_FALSE;
			dsz.depthTestEnable = VK_TRUE;
			dsz.depthWriteEnable = VK_TRUE;

			clrs[1].depthStencil.depth = 0.5f;
			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &drbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp_std);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdBindVertexBuffers(cb, 0, 1, &zvb.b, &zero);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_db_in);
			zpc = (typeof(zpc)){ c1, 0.5f, 1.0f };
			vkCmdPushConstants(cb, l_z,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof(zpc), &zpc);
			vkCmdDraw(cb, 4, 1, 0, 0);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_db_out);
			zpc.color = c0;
			vkCmdPushConstants(cb, l_z,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof(zpc), &zpc);
			vkCmdDraw(cb, 4, 1, 0, 0);
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			CHECK(check_rgba(v, r, "depth bounds", rb.map, W, H, solid_fn, &sc1));
			did_dbounds = true;
		}

		/* 7d. Wide lines (lineWidth = 3.0) */
		if (v->feat.wideLines && v->props.limits.lineWidthRange[1] >= 3.0f) {
			VkPipeline p_wl;

			iaz.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			rsz.lineWidth = 3.0f;
			dsz.depthTestEnable = VK_FALSE;
			dsz.depthWriteEnable = VK_FALSE;
			gciz.renderPass = rp;
			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz,
							      nullptr, &p_wl));
			vk_track(v, OBJ_PIPELINE, H2U(p_wl));
			rsz.lineWidth = 1.0f;

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp_std);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_wl);
			zpc = (typeof(zpc)){ c1, 0.0f, 1.0f };
			vkCmdPushConstants(cb, l_z,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof(zpc), &zpc);
			vkCmdDraw(cb, 3, 1, 16, 0);
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			/* Rows 15, 16, 17 for x in [12, 52] must be c1 */
			struct rgba b1 = rgba_of(c1);

			for (uint32_t y = 15; y <= 17; y++)
				for (uint32_t x = 12; x <= 52; x++) {
					const uint8_t *p = rb.map + ((size_t)y * W + x) * 4;

					r->checked++;
					if (p[0] != b1.r || p[1] != b1.g || p[2] != b1.b) {
						r->mismatches++;
						snprintf(r->detail, sizeof(r->detail),
							 "wide line (%u,%u): got %02x%02x%02x",
							 x, y, p[0], p[1], p[2]);
						return R_MISMATCH;
					}
				}
			did_wide = true;
		}

		/* 7e. Large points (gl_PointSize = 4.0 at center (32,32)) */
		if (v->feat.largePoints && v->props.limits.pointSizeRange[1] >= 4.0f) {
			VkPipeline p_lp;

			iaz.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			dsz.depthTestEnable = VK_FALSE;
			dsz.depthWriteEnable = VK_FALSE;
			gciz.renderPass = rp;
			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gciz,
							      nullptr, &p_lp));
			vk_track(v, OBJ_PIPELINE, H2U(p_lp));

			VkDeviceSize pt_off = sizeof(fsq);

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp_std);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdBindVertexBuffers(cb, 0, 1, &zvb.b, &pt_off);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_lp);
			zpc = (typeof(zpc)){ c1, 0.0f, 4.0f };
			vkCmdPushConstants(cb, l_z,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof(zpc), &zpc);
			vkCmdDraw(cb, 1, 1, 0, 0);
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			struct box_exp pe = { c0, c1, 30, 30, 34, 34, 0, 0, 0, 0 };

			CHECK(check_rgba(v, r, "large point 4x4", rb.map, W, H, box_expect_fn, &pe));
			did_pts = true;
		}
	}

	/* 8. Sample shading + alpha-to-coverage (4x MSAA) */
	if (did_mask && v->feat.sampleRateShading) {
		struct img ms, res;
		VkRenderPass rp_ms;
		VkFramebuffer fb_ms;
		VkPipeline p_ss;

		CHECK(img_create(v, &ms, W, H, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				 VK_SAMPLE_COUNT_4_BIT));
		CHECK(img_create(v, &res, W, H, FMT,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		struct rp_desc rd = { .color = FMT, .samples = VK_SAMPLE_COUNT_4_BIT, .resolve = true };

		CHECK(rp_build(v, &rp_ms, &rd));
		VkImageView views[2] = { ms.view, res.view };

		CHECK(fb_create_views(v, &fb_ms, rp_ms, views, 2, W, H, 1));
		VkPipelineShaderStageCreateInfo st[2] = {
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_fs, .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_col, .pName = "main" },
		};
		VkPipelineVertexInputStateCreateInfo evi = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		};
		VkPipelineInputAssemblyStateCreateInfo ia = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		VkPipelineViewportStateCreateInfo vp = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .scissorCount = 1,
		};
		VkPipelineRasterizationStateCreateInfo rs = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
		};
		VkPipelineMultisampleStateCreateInfo ms_ci = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT,
			.sampleShadingEnable = VK_TRUE,
			.minSampleShading = 1.0f,
			.alphaToCoverageEnable = VK_TRUE,
		};
		VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
		VkPipelineColorBlendStateCreateInfo cbs = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1, .pAttachments = &cba,
		};
		VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dy = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2, .pDynamicStates = dyn,
		};
		VkGraphicsPipelineCreateInfo gci = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2, .pStages = st, .pVertexInputState = &evi,
			.pInputAssemblyState = &ia, .pViewportState = &vp,
			.pRasterizationState = &rs, .pMultisampleState = &ms_ci,
			.pColorBlendState = &cbs, .pDynamicState = &dy,
			.layout = layout, .renderPass = rp_ms,
		};
		VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gci, nullptr, &p_ss));
		vk_track(v, OBJ_PIPELINE, H2U(p_ss));

		/* Draw c1 with alpha=0 (0 coverage -> keeps clear c0), then c1 with alpha=255 */
		uint32_t c_zero_a = c1 & 0x00FFFFFFu;
		VkRenderPassBeginInfo mrbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp_ms, .framebuffer = fb_ms,
			.renderArea = { { 0, 0 }, { W, H } },
			.clearValueCount = 1, .pClearValues = &clear,
		};
		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &mrbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_ss);
		vkCmdSetViewport(cb, 0, 1, &vp_std);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c_zero_a);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &res, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		struct solid sc0 = { c0 };

		CHECK(check_rgba(v, r, "alpha-to-coverage 0", rb.map, W, H, solid_fn, &sc0));
		did_ss = true;
	}

	/* 9. Dynamic rendering (VK_KHR_dynamic_rendering) with LOAD/STORE DONT_CARE */
	if (v->has_dyn_rendering) {
		PFN_vkCmdBeginRenderingKHR begin_ren =
			(PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(v->dev, "vkCmdBeginRenderingKHR");
		PFN_vkCmdEndRenderingKHR end_ren =
			(PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(v->dev, "vkCmdEndRenderingKHR");
		if (begin_ren && end_ren) {
			VkFormat cfmt = FMT;
			VkPipelineRenderingCreateInfo prci = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &cfmt,
			};
			VkPipelineShaderStageCreateInfo st[2] = {
				{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_fs, .pName = "main" },
				{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_col, .pName = "main" },
			};
			VkPipelineVertexInputStateCreateInfo evi = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			};
			VkPipelineInputAssemblyStateCreateInfo ia = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
				.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			};
			VkPipelineViewportStateCreateInfo vp = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
				.viewportCount = 1, .scissorCount = 1,
			};
			VkPipelineRasterizationStateCreateInfo rs = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
				.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
				.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
			};
			VkPipelineMultisampleStateCreateInfo ms = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
				.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			};
			VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
			VkPipelineColorBlendStateCreateInfo cbs = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
				.attachmentCount = 1, .pAttachments = &cba,
			};
			VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dy = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
				.dynamicStateCount = 2, .pDynamicStates = dyn,
			};
			VkGraphicsPipelineCreateInfo gci = {
				.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
				.pNext = &prci,
				.stageCount = 2, .pStages = st, .pVertexInputState = &evi,
				.pInputAssemblyState = &ia, .pViewportState = &vp,
				.pRasterizationState = &rs, .pMultisampleState = &ms,
				.pColorBlendState = &cbs, .pDynamicState = &dy,
				.layout = layout,
			};
			VkPipeline p_dyn;

			VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gci,
							      nullptr, &p_dyn));
			vk_track(v, OBJ_PIPELINE, H2U(p_dyn));

			VkRenderingAttachmentInfo ca = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = color.view,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			};
			VkRenderingInfo ri = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
				.renderArea = { { 0, 0 }, { W, H } },
				.layerCount = 1,
				.colorAttachmentCount = 1,
				.pColorAttachments = &ca,
			};
			CHECK(cmd_begin(v, &cb));
			img_barrier(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED,
				    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
				    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			begin_ren(cb, &ri);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_dyn);
			vkCmdSetViewport(cb, 0, 1, &vp_std);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &c1);
			vkCmdDraw(cb, 3, 1, 0, 0);
			end_ren(cb);
			img_barrier(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_ACCESS_TRANSFER_READ_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			struct solid sc1 = { c1 };

			CHECK(check_rgba(v, r, "dynamic rendering", rb.map, W, H, solid_fn, &sc1));
			did_dyn = true;
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "neg-vp, strip+restart, fan, line, bias%s%s%s%s%s%s%s%s",
		 did_dclamp ? ", clamp" : "", did_dbounds ? ", bounds" : "",
		 did_wide ? ", wide-line" : "", did_pts ? ", large-pt" : "",
		 did_mask ? ", mask" : "", did_ss ? ", ss+a2c" : "",
		 did_noatt ? ", no-att" : "", did_dyn ? ", dyn-render" : "");
	return R_PASS;
}
