// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// msaa: the position of each sample (gl_SampleID forces per-sample shading).
#version 450
layout(std430, set = 0, binding = 0) buffer B { vec2 pos[]; } b;
layout(location = 0) out vec4 o;

void main()
{
	b.pos[gl_SampleID] = gl_SamplePosition;
	o = vec4(0.0);
}
