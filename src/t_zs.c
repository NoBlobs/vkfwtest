// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * zs-load: depth and stencil kept between render passes (ZLS load + store).
 *  pass 1 (clear): left half at depth 0.3, stencil 0x5A, colour C1;
 *  pass 2 (load, colour cleared): a full-screen rectangle at depth 0.5 with
 *          LESS and no depth write: only the right half (depth 1.0) passes;
 *  pass 3 (load, colour cleared): a full-screen rectangle with stencil EQUAL
 *          0x5A and no depth test: only the left half passes.
 * A depth or stencil buffer that is not loaded gives the wrong half.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "rect.vert.h"
#include "rect.frag.h"

constexpr VkFormat CFMT = VK_FORMAT_R8G8B8A8_UNORM;

struct rect_pc {
	float rect[4];
	uint32_t color;
	float z;
};

struct halves {
	uint32_t w, left, right;
};

static struct rgba halves_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct halves *e = ctx;

	(void)y;
	return rgba_of(x < e->w / 2 ? e->left : e->right);
}

static bool ds_format(struct vk *v, VkFormat f)
{
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, f, &fp);
	return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

enum result test_zs_load(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	VkFormat dfmt = VK_FORMAT_UNDEFINED;
	bool stencil = true;

	if (ds_format(v, VK_FORMAT_D24_UNORM_S8_UINT))
		dfmt = VK_FORMAT_D24_UNORM_S8_UINT;
	else if (ds_format(v, VK_FORMAT_D32_SFLOAT_S8_UINT))
		dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
	else if (ds_format(v, VK_FORMAT_D32_SFLOAT))
		dfmt = VK_FORMAT_D32_SFLOAT, stencil = false;
	else if (ds_format(v, VK_FORMAT_D16_UNORM))
		dfmt = VK_FORMAT_D16_UNORM, stencil = false;
	else {
		snprintf(r->detail, sizeof(r->detail), "no depth format");
		return R_SKIP;
	}

	struct img color, depth;
	struct buf rb;
	VkRenderPass rp_clear, rp_load;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline p_write, p_depth, p_stencil = VK_NULL_HANDLE;

	CHECK(img_create(v, &color, w, h, CFMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &depth, w, h, dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	if (stencil) {
		/* the view must cover both aspects for a depth/stencil attachment */
		depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		VkImageViewCreateInfo vci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = depth.i,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = dfmt,
			.subresourceRange = { depth.aspect, 0, 1, 0, 1 },
		};
		VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &depth.view));
		vk_track(v, OBJ_VIEW, H2U(depth.view));
	}
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	struct rp_desc rd = { .color = CFMT, .depth = dfmt, .stencil = stencil };

	CHECK(rp_build(v, &rp_clear, &rd));
	rd.depth_load = true;
	CHECK(rp_build(v, &rp_load, &rd));
	VkImageView views[2] = { color.view, depth.view };

	CHECK(fb_create_views(v, &fb, rp_clear, views, 2, w, h, 1));
	CHECK(shader_create(v, &vs, spv_rect_vert, sizeof(spv_rect_vert)));
	CHECK(shader_create(v, &fs, spv_rect_frag, sizeof(spv_rect_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct rect_pc), nullptr));
	VkStencilOpState write = {
		.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
		.depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
		.compareMask = 0xFF, .writeMask = 0xFF, .reference = 0x5A,
	};
	VkStencilOpState equal = {
		.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
		.depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_EQUAL,
		.compareMask = 0xFF, .writeMask = 0, .reference = 0x5A,
	};
	struct gfx_desc d = { .rp = rp_clear, .layout = layout, .vs = vs, .fs = fs, .depth = true,
			      .stencil = stencil ? &write : nullptr };
	CHECK(gfx_pipeline(v, &p_write, &d));
	d = (struct gfx_desc){ .rp = rp_load, .layout = layout, .vs = vs, .fs = fs, .depth = true,
			       .depth_no_write = true };
	CHECK(gfx_pipeline(v, &p_depth, &d));
	d = (struct gfx_desc){ .rp = rp_load, .layout = layout, .vs = vs, .fs = fs,
			       .stencil = &equal };
	if (stencil)
		CHECK(gfx_pipeline(v, &p_stencil, &d));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t c0 = (lcg(seed + f, 2) & 0x00FFFFFFu) | 0xFF000000u;
		uint32_t c1 = (lcg(seed + f, 3) & 0x00FFFFFFu) | 0xFF000000u;
		uint32_t c2 = (lcg(seed + f, 4) & 0x00FFFFFFu) | 0xFF000000u;
		uint32_t c3 = (lcg(seed + f, 5) & 0x00FFFFFFu) | 0xFF000000u;
		struct {
			VkRenderPass rp;
			VkPipeline p;
			struct rect_pc pc;
			struct halves e;
			const char *name;
		} pass[3] = {
			{ rp_clear, p_write, { { -1, -1, 0, 1 }, c1, 0.3f }, { w, c1, c0 },
			  "pass 1 (write depth/stencil)" },
			{ rp_load, p_depth, { { -1, -1, 1, 1 }, c2, 0.5f }, { w, c0, c2 },
			  "pass 2 (loaded depth)" },
			{ rp_load, p_stencil, { { -1, -1, 1, 1 }, c3, 0.9f }, { w, c3, c0 },
			  "pass 3 (loaded stencil)" },
		};

		for (uint32_t i = 0; i < (stencil ? 3u : 2u); i++) {
			VkClearValue clear[2] = { clear_rgba(c0), { .depthStencil = { 1.0f, 0 } } };
			VkRenderPassBeginInfo rbi = {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = pass[i].rp,
				.framebuffer = fb,
				.renderArea = { { 0, 0 }, { w, h } },
				.clearValueCount = 2,
				.pClearValues = clear,
			};
			VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
			VkRect2D sc = { { 0, 0 }, { w, h } };
			VkCommandBuffer cb;
			char what[64];

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pass[i].p);
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdPushConstants(cb, layout,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
					   sizeof(pass[i].pc), &pass[i].pc);
			vkCmdDraw(cb, 6, 1, 0, 0);
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			snprintf(what, sizeof(what), "frame %u %s", f, pass[i].name);
			CHECK(check_rgba(v, r, what, rb.map, w, h, halves_fn, &pass[i].e));
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u %s: depth%s loaded across passes, %u frames", w,
		 h, dfmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" :
		    dfmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" :
		    dfmt == VK_FORMAT_D32_SFLOAT ? "D32" : "D16",
		 stencil ? " + stencil" : "", frames);
	return R_PASS;
}
