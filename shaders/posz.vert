// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Depth/large-point vertex shader: NDC pos + depth + point size via push constant.
#version 450
layout(location = 0) in vec2 pos;
layout(push_constant) uniform PC { uint color; float z; float ptsz; } pc;

void main()
{
	gl_Position = vec4(pos, pc.z, 1.0);
	gl_PointSize = pc.ptsz;
}
