// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Per-layer colour; t_rta.c computes the same hash.
#version 450
layout(push_constant) uniform PC { uint seed; } pc;
layout(location = 0) flat in uint v_layer;
layout(location = 0) out vec4 o;

void main()
{
	uint h = (v_layer + 1u) * 0x9E3779B1u ^ pc.seed;
	h ^= h >> 13;
	o = vec4(uvec4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, 255u)) / 255.0;
}
