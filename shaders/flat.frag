// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
#version 450
layout(location = 0) flat in vec4 v_color;
layout(location = 0) out vec4 o;

void main()
{
	o = v_color;
}
