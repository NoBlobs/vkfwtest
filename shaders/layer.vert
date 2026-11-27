// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Layered rendering: one full-screen triangle per instance, sent to layer
// gl_InstanceIndex (VK_EXT_shader_viewport_index_layer).
#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(location = 0) flat out uint v_layer;

void main()
{
	vec2 p = vec2((gl_VertexIndex & 1) * 4 - 1, (gl_VertexIndex >> 1) * 4 - 1);
	gl_Position = vec4(p, 0.0, 1.0);
	gl_Layer = gl_InstanceIndex;
	v_layer = uint(gl_InstanceIndex);
}
