// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Controlled 3D-pipe MMU page fault: writes and reads far past an SSBO in fragment.
#version 450
layout(std430, set = 0, binding = 0) buffer B { uint data[]; } b;
layout(push_constant) uniform PC { uint index; } pc;
layout(location = 0) out vec4 o;

void main()
{
	b.data[pc.index] = 0xDEADBEEFu;
	uint x = b.data[pc.index];
	o = vec4(float(x & 255u) / 255.0, 0.0, 0.0, 1.0);
}
