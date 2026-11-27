// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// The push-constant colour (packed RGBA8).
#version 450
layout(push_constant) uniform PC { uint color; } pc;
layout(location = 0) out vec4 o;

void main()
{
	uint x = pc.color;
	o = vec4(uvec4(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u, (x >> 24) & 255u)) / 255.0;
}
