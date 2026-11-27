// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// A constant colour; mode 1 discards every other pixel (checkerboard).
#version 450
layout(push_constant) uniform PC { vec4 color; uint mode; } pc;
layout(location = 0) out vec4 o;

void main()
{
	if (pc.mode == 1u && ((uint(gl_FragCoord.x) + uint(gl_FragCoord.y)) & 1u) != 0u)
		discard;
	o = pc.color;
}
