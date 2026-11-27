// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Fragment shader imageLoad + imageStore.
#version 450
layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D src_img;
layout(set = 0, binding = 1, r32ui) uniform writeonly uimage2D dst_img;
layout(location = 0) out vec4 o;

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	uint v = imageLoad(src_img, p).x;
	imageStore(dst_img, p, uvec4(v + 10u, 0u, 0u, 0u));
	o = vec4(1.0);
}
