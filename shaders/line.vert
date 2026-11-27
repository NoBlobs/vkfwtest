// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Horizontal lines through the pixel centres of every stepy-th row, from
// beyond the left edge to beyond the right edge.
#version 450
layout(push_constant) uniform PC { uint w, h, stepx, stepy, color; } pc;
layout(location = 0) flat out vec4 v_color;

void main()
{
	uint line = uint(gl_VertexIndex) / 2u, end = uint(gl_VertexIndex) % 2u;
	vec2 px = vec2(end == 0u ? -2.0 : float(pc.w) + 2.0, float(line * pc.stepy) + 0.5);
	gl_Position = vec4(px / vec2(pc.w, pc.h) * 2.0 - 1.0, 0.0, 1.0);
	uint x = pc.color;
	v_color = vec4(uvec4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, (x >> 24) & 255u)) / 255.0;
}
