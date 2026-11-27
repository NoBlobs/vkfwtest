// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Multiview: the colour of view gl_ViewIndex.
#version 450
#extension GL_EXT_multiview : require
layout(push_constant) uniform PC { uint c0, c1; } pc;
layout(location = 0) out vec4 o;

void main()
{
	uint x = gl_ViewIndex == 0 ? pc.c0 : pc.c1;
	o = vec4(uvec4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, (x >> 24) & 255u)) / 255.0;
}
