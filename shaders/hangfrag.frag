// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Controlled fragment lockup: spins while the UBO word is 1 (it always is).
#version 450
layout(set = 0, binding = 0) uniform U { uint spin; } u;
layout(location = 0) out vec4 o;

void main()
{
	uint x = 0u;
	while (u.spin == 1u)
		x += 1u;
	o = vec4(float(x & 255u) / 255.0);
}
