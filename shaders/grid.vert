// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Tiled tiny triangles with no vertex buffer: layers of cells x cells quads,
// two triangles per cell, cell edges on pixel edges. With depth, every later
// layer is farther away, so the first layer must survive (depth test LESS).
#version 450
layout(push_constant) uniform PC { uint cells; uint layers; uint seed; uint depth; } pc;
layout(location = 0) flat out uint v_cell;
layout(location = 1) flat out uint v_layer;

void main()
{
	uint tri = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) % 3u;
	uint quad = tri >> 1, per_layer = pc.cells * pc.cells;
	uint layer = quad / per_layer, cell = quad % per_layer;
	uvec2 c = uvec2(cell % pc.cells, cell / pc.cells);
	// triangle 0: (0,0) (1,0) (0,1); triangle 1: (1,0) (1,1) (0,1)
	uvec2 d = (tri & 1u) == 0u ? uvec2(corner == 1u ? 1u : 0u, corner == 2u ? 1u : 0u)
				   : uvec2(corner == 2u ? 0u : 1u, corner == 0u ? 0u : 1u);
	vec2 p = vec2(c + d) / float(pc.cells) * 2.0 - 1.0;
	float z = pc.depth != 0u ? float(layer + 1u) / float(pc.layers + 1u) : 0.0;
	gl_Position = vec4(p, z, 1.0);
	v_cell = cell;
	v_layer = layer;
}
