// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// A constant integer colour (integer render targets).
#version 450
layout(push_constant) uniform PC { uvec4 color; } pc;
layout(location = 0) out uvec4 o;

void main()
{
	o = pc.color;
}
