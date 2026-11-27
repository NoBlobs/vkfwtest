// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// iters == 0: a solid colour. Otherwise an integer LCG run iters times from a
// per-pixel seed; the CPU computes the same value to check every pixel.
#version 450
layout(push_constant) uniform PC { uint color; uint iters; uint seed; } pc;
layout(location = 0) out vec4 o;

vec4 unpack(uint x)
{
	return vec4(uvec4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, (x >> 24) & 255u)) / 255.0;
}

void main()
{
	if (pc.iters == 0u) {
		o = unpack(pc.color);
		return;
	}
	uint x = pc.seed + uint(gl_FragCoord.x) + uint(gl_FragCoord.y) * 65536u;
	for (uint i = 0u; i < pc.iters; i++)
		x = x * 1664525u + 1013904223u;
	o = unpack(x | 0xFF000000u);
}
