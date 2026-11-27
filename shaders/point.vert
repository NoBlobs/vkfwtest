// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// One 1-pixel point at the centre of every (stepx, stepy)-th pixel.
#version 450
layout(push_constant) uniform PC { uint w, h, stepx, stepy, color; } pc;
layout(location = 0) flat out vec4 v_color;

void main()
{
	uint per_row = (pc.w + pc.stepx - 1u) / pc.stepx;
	uint i = uint(gl_VertexIndex);
	vec2 px = vec2((i % per_row) * pc.stepx, (i / per_row) * pc.stepy) + 0.5;
	gl_Position = vec4(px / vec2(pc.w, pc.h) * 2.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 1.0;
	uint x = pc.color;
	v_color = vec4(uvec4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, (x >> 24) & 255u)) / 255.0;
}
