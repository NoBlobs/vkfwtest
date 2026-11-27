// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Positions from a vertex buffer (NDC).
#version 450
layout(location = 0) in vec2 pos;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
}
