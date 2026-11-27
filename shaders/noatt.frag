// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// A render pass without attachments: count fragments with an atomic.
#version 450
layout(std430, set = 0, binding = 0) buffer C { uint n; } c;

void main()
{
	atomicAdd(c.n, 1u);
}
