// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// One triangle that covers the whole viewport (draw 3 vertices).
#version 450
void main()
{
	vec2 p = vec2((gl_VertexIndex & 1) * 4 - 1, (gl_VertexIndex >> 1) * 4 - 1);
	gl_Position = vec4(p, 0.0, 1.0);
}
