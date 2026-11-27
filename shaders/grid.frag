// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Flat colour from (cell, layer, seed); grid.c computes the same hash.
#version 450
layout(push_constant) uniform PC { uint cells; uint layers; uint seed; uint depth; } pc;
layout(location = 0) flat in uint v_cell;
layout(location = 1) flat in uint v_layer;
layout(location = 0) out vec4 o;

void main()
{
	uint h = (v_cell * 2654435761u) ^ (v_layer * 40503u + 0x9E3779B9u) ^ pc.seed;
	h ^= h >> 15;
	o = vec4(uvec4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, 255u)) / 255.0;
}
