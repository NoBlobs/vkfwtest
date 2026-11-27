// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * vertex-ext: vertex fetch features of Vulkan 1.4 (each optional):
 *  - 8-bit indices (index_type_uint8; core 1.4 or VK_KHR/EXT_index_type_uint8);
 *  - instance-rate divisors (vertex_attribute_divisor; core 1.4 or
 *    VK_KHR/EXT_vertex_attribute_divisor): the colour attribute advances every
 *    D instances, and with zero-divisor support, never.
 * A grid of instanced quads; cell i must have colour[i / D] (colour[0] for 0).
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "vbuf.vert.h"
#include "flat.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t CELLS = 16;

struct vx_expect {
	uint32_t w, h, seed, divisor;   /* divisor 1: one colour per cell */
};

static uint32_t cell_color(uint32_t c, uint32_t seed)
{
	return (lcg(c ^ seed, 3) & 0x00FFFFFFu) | 0xFF000000u;
}

static struct rgba vx_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct vx_expect *e = ctx;
	uint32_t c = (y / (e->h / CELLS)) * CELLS + x / (e->w / CELLS);

	return rgba_of(cell_color(e->divisor ? c / e->divisor : 0, e->seed));
}

enum result test_vertex_ext(struct vk *v, const struct opts *o, struct report *r)
{
#if defined(VK_KHR_index_type_uint8) && defined(VK_KHR_vertex_attribute_divisor)
	uint32_t w = 128, h = 128, seed = OPT(o->seed, 1);
	struct img color;
	struct buf rb, vb, cells, cols, ib16, ib8;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	char done[96] = "";

	if (!v->has_index8 && !v->has_divisor) {
		snprintf(r->detail, sizeof(r->detail),
			 "needs index_type_uint8 or vertex_attribute_divisor (Vulkan 1.4 or extensions)");
		return R_SKIP;
	}
	CHECK(img_create(v, &color, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs, spv_vbuf_vert, sizeof(spv_vbuf_vert)));
	CHECK(shader_create(v, &fs, spv_flat_frag, sizeof(spv_flat_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t), nullptr));

	static const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	static const uint8_t i8[8] = { 9, 9, 0, 1, 2, 1, 3, 2 };    /* firstIndex 2 */
	static const uint16_t i16[6] = { 0, 1, 2, 1, 3, 2 };

	CHECK(buf_create(v, &vb, sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
	memcpy(vb.map, corners, sizeof(corners));
	CHECK(buf_create(v, &ib8, sizeof(i8), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true));
	memcpy(ib8.map, i8, sizeof(i8));
	CHECK(buf_create(v, &ib16, sizeof(i16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true));
	memcpy(ib16.map, i16, sizeof(i16));
	CHECK(buf_create(v, &cells, CELLS * CELLS * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
	CHECK(buf_create(v, &cols, CELLS * CELLS * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
	for (uint32_t c = 0; c < CELLS * CELLS; c++) {
		uint32_t xy[2] = { c % CELLS, c / CELLS };
		struct rgba k = rgba_of(cell_color(c, seed));
		uint8_t *p = cols.map + c * 4;

		memcpy(cells.map + c * 8, xy, 8);
		p[0] = k.r, p[1] = k.g, p[2] = k.b, p[3] = k.a;
	}

	/* the cases: (index type, colour divisor) */
	struct {
		bool i8;
		uint32_t divisor;
		bool ok;
		const char *name;
	} cases[3] = {
		{ true, 1, v->has_index8, "8-bit indices" },
		{ false, 4, v->has_divisor, "divisor 4" },
		{ false, 0, v->has_divisor_zero, "divisor 0" },
	};

	for (uint32_t k = 0; k < 3; k++) {
		if (!cases[k].ok)
			continue;
		VkVertexInputBindingDescription bind[3] = {
			{ 0, 8, VK_VERTEX_INPUT_RATE_VERTEX },
			{ 1, 8, VK_VERTEX_INPUT_RATE_INSTANCE },
			{ 2, 4, VK_VERTEX_INPUT_RATE_INSTANCE },
		};
		VkVertexInputAttributeDescription attr[3] = {
			{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
			{ 1, 1, VK_FORMAT_R32G32_UINT, 0 },
			{ 2, 2, VK_FORMAT_R8G8B8A8_UNORM, 0 },
		};
		VkVertexInputBindingDivisorDescriptionKHR div = { 2, cases[k].divisor };
		VkPipelineVertexInputDivisorStateCreateInfoKHR dci = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR,
			.vertexBindingDivisorCount = 1,
			.pVertexBindingDivisors = &div,
		};
		VkPipelineVertexInputStateCreateInfo vi = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.pNext = cases[k].divisor != 1 ? &dci : nullptr,
			.vertexBindingDescriptionCount = 3,
			.pVertexBindingDescriptions = bind,
			.vertexAttributeDescriptionCount = 3,
			.pVertexAttributeDescriptions = attr,
		};
		VkPipeline pipe;
		struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs,
				      .vertex_input = &vi };

		CHECK(gfx_pipeline(v, &pipe, &d));

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
		VkBuffer bufs[3] = { vb.b, cells.b, cols.b };
		VkDeviceSize offs[3] = { 0, 0, 0 };
		uint32_t n = CELLS;
		VkCommandBuffer cb;
		struct vx_expect e = { w, h, seed, cases[k].divisor };

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdBindVertexBuffers(cb, 0, 3, bufs, offs);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &n);
		if (cases[k].i8) {
			vkCmdBindIndexBuffer(cb, ib8.b, 0, VK_INDEX_TYPE_UINT8_KHR);
			vkCmdDrawIndexed(cb, 6, CELLS * CELLS, 2, 0, 0);
		} else {
			vkCmdBindIndexBuffer(cb, ib16.b, 0, VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(cb, 6, CELLS * CELLS, 0, 0, 0);
		}
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &color, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		CHECK(check_rgba(v, r, cases[k].name, rb.map, w, h, vx_fn, &e));
		size_t len = strlen(done);

		snprintf(done + len, sizeof(done) - len, "%s%s", len ? ", " : "", cases[k].name);
	}
	snprintf(r->detail, sizeof(r->detail), "%ux%u instanced quads: %s", w, h, done);
	return R_PASS;
#else
	(void)v;
	(void)o;
	snprintf(r->detail, sizeof(r->detail), "built with Vulkan headers older than 1.3.280");
	return R_SKIP;
#endif
}
