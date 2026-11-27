// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * rta: layered rendering into a 6-layer 2D array image (render-target arrays,
 * firmware 1.1 "gs_rta_support").
 *  1. layered clears: the render pass clears all layers to C, then
 *     vkCmdClearAttachments clears layers 1, 3, 5 to their own colours;
 *  2. layered draw, when VK_EXT_shader_viewport_index_layer is available:
 *     one full-screen triangle per instance, sent to layer gl_InstanceIndex.
 * Every pixel of every layer is checked.
 */
#include <stdio.h>

#include "vkfwtest.h"

#include "layer.vert.h"
#include "layer.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

static uint32_t layer_color(uint32_t layer, uint32_t seed)
{
	uint32_t hsh = (layer + 1u) * 0x9E3779B1u ^ seed;

	hsh ^= hsh >> 13;
	return (hsh & 0x00FFFFFFu) | 0xFF000000u;
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

/* Read every layer back and compare with expected(layer). */
static enum result check_layers(struct vk *v, struct report *r, const struct img *im,
				const struct buf *rb, const char *what, const uint32_t *expect)
{
	size_t layer_bytes = (size_t)im->w * im->h * 4;

	for (uint32_t l = 0; l < im->layers; l++) {
		struct solid s = { expect[l] };
		char w[48];

		snprintf(w, sizeof(w), "%s layer %u", what, l);
		CHECK(check_rgba(v, r, w, rb->map + l * layer_bytes, im->w, im->h, solid_fn, &s));
	}
	return R_PASS;
}

enum result test_rta(struct vk *v, const struct opts *o, struct report *r)
{
	constexpr uint32_t L = 6;
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64), seed = OPT(o->seed, 1);
	uint32_t frames = OPT(o->frames, 2);
	struct img im;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	bool draw = v->has_output_layer;

	CHECK(img_create_layers(v, &im, w, h, L, FMT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp,
		.attachmentCount = 1,
		.pAttachments = &im.view,
		.width = w,
		.height = h,
		.layers = L,
	};
	VK_CHECK(v, vkCreateFramebuffer(v->dev, &fci, nullptr, &fb));
	vk_track(v, OBJ_FRAMEBUFFER, H2U(fb));
	if (draw) {
		VkShaderModule vs, fs;

		CHECK(shader_create(v, &vs, spv_layer_vert, sizeof(spv_layer_vert)));
		CHECK(shader_create(v, &fs, spv_layer_frag, sizeof(spv_layer_frag)));
		CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t),
				    nullptr));
		struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
		CHECK(gfx_pipeline(v, &pipe, &d));
	}

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 101u;
		uint32_t base = layer_color(99, s), expect[L];
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };

		for (uint32_t pass = 0; pass < (draw ? 2u : 1u); pass++) {
			VkClearValue clear = clear_rgba(base);
			VkRenderPassBeginInfo rbi = {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = rp,
				.framebuffer = fb,
				.renderArea = { { 0, 0 }, { w, h } },
				.clearValueCount = 1,
				.pClearValues = &clear,
			};
			VkCommandBuffer cb;

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			if (pass == 0) {
				/* 1. per-layer clears of the odd layers */
				for (uint32_t l = 0; l < L; l++) {
					expect[l] = base;
					if (!(l & 1))
						continue;
					expect[l] = layer_color(l, s);
					VkClearAttachment ca = {
						.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
						.colorAttachment = 0,
						.clearValue = clear_rgba(expect[l]),
					};
					VkClearRect cr = { { { 0, 0 }, { w, h } }, l, 1 };

					vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
				}
			} else {
				/* 2. one instance per layer, routed with gl_Layer */
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				vkCmdSetViewport(cb, 0, 1, &vp);
				vkCmdSetScissor(cb, 0, 1, &sc);
				vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
						   sizeof(s), &s);
				vkCmdDraw(cb, 3, L, 0, 0);
				for (uint32_t l = 0; l < L; l++)
					expect[l] = layer_color(l, s);
			}
			vkCmdEndRenderPass(cb);
			for (uint32_t l = 0; l < L; l++)
				cmd_copy_layer_to_buffer(cb, &im, l, &rb, (VkDeviceSize)l * w * h * 4);
			CHECK(cmd_submit_wait(v, cb));
			CHECK(check_layers(v, r, &im, &rb, pass ? "draw" : "clear", expect));
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u x %u layers: layered clears%s, %u frames", w,
		 h, L, draw ? " + gl_Layer draw" : " (no gl_Layer: draw skipped)", frames);
	return R_PASS;
}
