// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * The tiled-triangle workload of pre-ta and bigpb: `layers` full-screen grids
 * of cells x cells quads (two triangles each), drawn with no vertex buffer.
 * Cell edges fall on pixel edges, so every pixel's colour is known exactly.
 */
#include "vkfwtest.h"

#include "grid.vert.h"
#include "grid.frag.h"

struct grid_pc {
	uint32_t cells, layers, seed, depth;
};
static_assert(sizeof(struct grid_pc) == 16);

static uint32_t gcd(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

void grid_plan(struct grid *g, uint32_t w, uint32_t h, uint64_t tris, uint32_t min_px)
{
	uint32_t common = gcd(w, h);
	uint32_t cells = 1;

	/*
	 * The largest cell count that divides both sides, keeps cells at least
	 * min_px wide and does not exceed the triangles asked for in one layer.
	 */
	for (uint32_t c = 1; c <= common; c++) {
		if (common % c || w / c < min_px || h / c < min_px)
			continue;
		if ((uint64_t)c * c * 2 > tris && c > 1)
			break;
		cells = c;
	}
	g->w = w;
	g->h = h;
	g->cells = cells;
	uint64_t per_layer = (uint64_t)cells * cells * 2;
	uint64_t layers = (tris + per_layer - 1) / per_layer;

	g->layers = layers ? (uint32_t)layers : 1;
}

uint64_t grid_tris(const struct grid *g)
{
	return (uint64_t)g->cells * g->cells * 2 * g->layers;
}

static struct rgba grid_color(uint32_t cell, uint32_t layer, uint32_t seed)
{
	uint32_t hsh = (cell * 2654435761u) ^ (layer * 40503u + 0x9E3779B9u) ^ seed;

	hsh ^= hsh >> 15;
	return (struct rgba){ hsh & 0xFF, (hsh >> 8) & 0xFF, (hsh >> 16) & 0xFF, 0xFF };
}

struct rgba grid_expect(uint32_t x, uint32_t y, const void *ctx)
{
	const struct grid *g = ctx;
	/* framebuffer y grows downwards, like NDC y in Vulkan */
	uint32_t cx = x / (g->w / g->cells), cy = y / (g->h / g->cells);

	return grid_color(cy * g->cells + cx, g->final_layer, g->seed);
}

enum result grid_pipeline(struct vk *v, VkPipeline *p, VkPipelineLayout *l, VkRenderPass rp,
			  bool depth)
{
	VkShaderModule vs, fs;

	CHECK(shader_create(v, &vs, spv_grid_vert, sizeof(spv_grid_vert)));
	CHECK(shader_create(v, &fs, spv_grid_frag, sizeof(spv_grid_frag)));
	CHECK(layout_create(v, l, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct grid_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = *l, .vs = vs, .fs = fs, .depth = depth };

	return gfx_pipeline(v, p, &d);
}

void grid_draw(VkCommandBuffer cb, VkPipelineLayout l, const struct grid *g, bool depth)
{
	struct grid_pc pc = { g->cells, g->layers, g->seed, depth };
	uint64_t verts = grid_tris(g) * 3;

	vkCmdPushConstants(cb, l, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			   sizeof(pc), &pc);
	/* split very large draws to stay inside 32-bit vertex counts */
	for (uint64_t first = 0; first < verts;) {
		uint64_t n = verts - first;

		if (n > 0x30000000ull)
			n = 0x30000000ull;
		vkCmdDraw(cb, (uint32_t)n, 1, (uint32_t)first, 0);
		first += n;
	}
}
