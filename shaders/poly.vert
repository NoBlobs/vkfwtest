// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// One triangle from three NDC points in push constants.
#version 450
layout(push_constant) uniform PC { vec4 p01; vec4 p2; uint color; } pc;

void main()
{
	vec2 p = gl_VertexIndex == 0 ? pc.p01.xy : gl_VertexIndex == 1 ? pc.p01.zw : pc.p2.xy;
	gl_Position = vec4(p, pc.p2.z, 1.0);
}
