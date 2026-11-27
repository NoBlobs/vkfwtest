// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Indexed, instanced quads from vertex buffers: a unit-square corner per
// vertex, a cell and a colour per instance.
#version 450
layout(location = 0) in vec2 corner;
layout(location = 1) in uvec2 cell;
layout(location = 2) in vec4 color;
layout(push_constant) uniform PC { uint cells; } pc;
layout(location = 0) flat out vec4 v_color;

void main()
{
	gl_Position = vec4((vec2(cell) + corner) / float(pc.cells) * 2.0 - 1.0, 0.0, 1.0);
	v_color = color;
}
