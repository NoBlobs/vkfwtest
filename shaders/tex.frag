// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Sample a texture at (fragment - off) / size with an explicit LOD.
#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform PC { vec2 off; vec2 size; float lod; } pc;
layout(location = 0) out vec4 o;

void main()
{
	o = textureLod(t, (gl_FragCoord.xy - pc.off) / pc.size, pc.lod);
}
