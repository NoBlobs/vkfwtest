// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * vertex: the vertex fetch paths that gl_VertexIndex-only tests skip.
 *  1. indexed, instanced quads from vertex buffers: a per-vertex corner
 *     (R32G32_SFLOAT), a per-instance cell (R32G32_UINT) and colour
 *     (R8G8B8A8_UNORM); half the cells with 16-bit indices, half with 32-bit
 *     ones, using firstIndex, vertexOffset and firstInstance;
 *  2. a point list: 1-pixel points on every 3rd column and 2nd row;
 *  3. a line list: horizontal lines through every 3rd row.
 * Every pixel is checked.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "vbuf.vert.h"
#include "flat.frag.h"
#include "point.vert.h"
#include "line.vert.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t CELLS = 16;

struct prim_pc {
	uint32_t w, h, stepx, stepy, color;
};

struct vexp {
	uint32_t w, h, seed, kind, stepx, stepy, color;
};

static uint32_t cell_color(uint32_t cell, uint32_t seed)
{
	return (lcg(cell ^ seed, 3) & 0x00FFFFFFu) | 0xFF000000u;
}

static struct rgba vexp_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct vexp *e = ctx;

	switch (e->kind) {
	case 0: {
		uint32_t cx = x / (e->w / CELLS), cy = y / (e->h / CELLS);

		return rgba_of(cell_color(cy * CELLS + cx, e->seed));
	}
	case 1:
		return rgba_of(x % e->stepx == 0 && y % e->stepy == 0 ? e->color : 0);
	default:
		return rgba_of(y % e->stepy == 0 ? e->color : 0);
	}
}

static void put_u32(uint8_t *p, uint32_t x)
{
	memcpy(p, &x, 4);       /* vertex data is in the host's byte order */
}

static void put_f32(uint8_t *p, float x)
{
	memcpy(p, &x, 4);
}

enum result test_vertex(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 128), h = OPT(o->height, 128);
	uint32_t frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	struct img color;
	struct buf rb, vb, ib, inst;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs_buf, vs_pt, vs_ln, fs;
	VkPipelineLayout l_buf, l_prim;
	VkPipeline p_tri, p_pt, p_ln;

	if (w % CELLS || h % CELLS) {
		snprintf(r->detail, sizeof(r->detail), "--size must be a multiple of %u", CELLS);
		return R_SETUP;
	}
	CHECK(img_create(v, &color, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));

	/* vertex buffer: 2 padding vertices, then the 4 corners (vertexOffset = 2) */
	static const float corners[6][2] = { { 9, 9 }, { 9, 9 }, { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

	CHECK(buf_create(v, &vb, sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));
	for (uint32_t i = 0; i < 6; i++) {
		put_f32(vb.map + i * 8, corners[i][0]);
		put_f32(vb.map + i * 8 + 4, corners[i][1]);
	}
	/* index buffer: 16-bit [pad pad pad | 0 1 2 1 3 2], then 32-bit at offset 64 */
	static const uint16_t i16[9] = { 7, 7, 7, 0, 1, 2, 1, 3, 2 };
	static const uint32_t i32[6] = { 0, 1, 2, 1, 3, 2 };

	CHECK(buf_create(v, &ib, 128, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true));
	memcpy(ib.map, i16, sizeof(i16));
	memcpy(ib.map + 64, i32, sizeof(i32));
	/* instance buffer: cell (2 x u32) + colour (4 x u8) per instance */
	CHECK(buf_create(v, &inst, CELLS * CELLS * 12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true));

	VkVertexInputBindingDescription bind[2] = {
		{ 0, 8, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, 12, VK_VERTEX_INPUT_RATE_INSTANCE },
	};
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 1, VK_FORMAT_R32G32_UINT, 0 },
		{ 2, 1, VK_FORMAT_R8G8B8A8_UNORM, 8 },
	};
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 2,
		.pVertexBindingDescriptions = bind,
		.vertexAttributeDescriptionCount = 3,
		.pVertexAttributeDescriptions = attr,
	};
	const VkPrimitiveTopology points = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	const VkPrimitiveTopology lines = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

	CHECK(shader_create(v, &vs_buf, spv_vbuf_vert, sizeof(spv_vbuf_vert)));
	CHECK(shader_create(v, &vs_pt, spv_point_vert, sizeof(spv_point_vert)));
	CHECK(shader_create(v, &vs_ln, spv_line_vert, sizeof(spv_line_vert)));
	CHECK(shader_create(v, &fs, spv_flat_frag, sizeof(spv_flat_frag)));
	CHECK(layout_create(v, &l_buf, VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t), nullptr));
	CHECK(layout_create(v, &l_prim, VK_SHADER_STAGE_VERTEX_BIT, sizeof(struct prim_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = l_buf, .vs = vs_buf, .fs = fs, .vertex_input = &vi };
	CHECK(gfx_pipeline(v, &p_tri, &d));
	d = (struct gfx_desc){ .rp = rp, .layout = l_prim, .vs = vs_pt, .fs = fs, .topology = &points };
	CHECK(gfx_pipeline(v, &p_pt, &d));
	d = (struct gfx_desc){ .rp = rp, .layout = l_prim, .vs = vs_ln, .fs = fs, .topology = &lines };
	CHECK(gfx_pipeline(v, &p_ln, &d));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 131u;

		for (uint32_t c = 0; c < CELLS * CELLS; c++) {
			struct rgba col = rgba_of(cell_color(c, s));
			uint8_t *p = inst.map + c * 12;

			put_u32(p, c % CELLS);
			put_u32(p + 4, c / CELLS);
			p[8] = col.r;
			p[9] = col.g;
			p[10] = col.b;
			p[11] = col.a;
		}
		for (uint32_t kind = 0; kind < 3; kind++) {
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
			struct vexp e = { w, h, s, kind, 3, kind == 1 ? 2 : 3,
					  (lcg(s, 9) & 0x00FFFFFFu) | 0xFF000000u };
			struct prim_pc pc = { w, h, e.stepx, e.stepy, e.color };
			VkCommandBuffer cb;
			char what[48];

			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &sc);
			if (kind == 0) {
				VkBuffer bufs[2] = { vb.b, inst.b };
				VkDeviceSize offs[2] = { 0, 0 };
				uint32_t cells = CELLS, half = CELLS * CELLS / 2;

				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_tri);
				vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
				vkCmdPushConstants(cb, l_buf, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &cells);
				/* first half: 16-bit indices from firstIndex 3, vertexOffset 2 */
				vkCmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT16);
				vkCmdDrawIndexed(cb, 6, half, 3, 2, 0);
				/* second half: 32-bit indices at byte 64, firstInstance = half */
				vkCmdBindIndexBuffer(cb, ib.b, 64, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexed(cb, 6, half, 0, 2, half);
			} else {
				uint32_t n = kind == 1 ? ((w + 2) / 3) * ((h + 1) / 2) : (h + 2) / 3 * 2;

				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
						  kind == 1 ? p_pt : p_ln);
				vkCmdPushConstants(cb, l_prim, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc),
						   &pc);
				vkCmdDraw(cb, n, 1, 0, 0);
			}
			vkCmdEndRenderPass(cb);
			cmd_copy_to_buffer(cb, &color, &rb);
			CHECK(cmd_submit_wait(v, cb));
			snprintf(what, sizeof(what), "frame %u %s", f,
				 kind == 0 ? "indexed instanced quads" : kind == 1 ? "points" : "lines");
			CHECK(check_rgba(v, r, what, rb.map, w, h, vexp_fn, &e));
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u: indexed (16/32-bit) instanced quads, points, lines; %u frames", w, h, frames);
	return R_PASS;
}
