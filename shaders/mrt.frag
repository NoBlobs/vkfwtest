// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Eight colour outputs; output i is a hash of (i, 4x4 block, seed).
#version 450
layout(push_constant) uniform PC { uint seed; } pc;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;

vec4 c(uint i)
{
	uvec2 b = uvec2(gl_FragCoord.xy) / 4u;
	uint h = (b.x * 73856093u) ^ (b.y * 19349663u) ^ (i * 83492791u) ^ pc.seed;
	h ^= h >> 16;
	return vec4(uvec4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, (h >> 24) & 255u)) / 255.0;
}

void main()
{
	o0 = c(0u); o1 = c(1u); o2 = c(2u); o3 = c(3u);
	o4 = c(4u); o5 = c(5u); o6 = c(6u); o7 = c(7u);
}
