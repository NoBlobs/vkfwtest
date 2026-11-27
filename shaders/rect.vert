// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// An axis-aligned rectangle (6 vertices) at depth z, from push constants.
#version 450
layout(push_constant) uniform PC { vec4 rect; uint color; float z; } pc;

void main()
{
	const uvec2 c[6] = uvec2[](uvec2(0, 0), uvec2(1, 0), uvec2(0, 1),
				   uvec2(1, 0), uvec2(1, 1), uvec2(0, 1));
	uvec2 k = c[gl_VertexIndex];
	gl_Position = vec4(k.x == 0u ? pc.rect.x : pc.rect.z, k.y == 0u ? pc.rect.y : pc.rect.w,
			   pc.z, 1.0);
}
