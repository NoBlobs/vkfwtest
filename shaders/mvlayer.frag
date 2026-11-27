// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Multiview: the colour of layer gl_ViewIndex, with the hash of layer.frag
// (t_rta.c computes the same).
#version 450
#extension GL_EXT_multiview : require
layout(push_constant) uniform PC { uint seed; } pc;
layout(location = 0) out vec4 o;

void main()
{
	uint h = (uint(gl_ViewIndex) + 1u) * 0x9E3779B1u ^ pc.seed;
	h ^= h >> 13;
	o = vec4(uvec4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, 255u)) / 255.0;
}
