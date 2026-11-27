// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Dynamic rendering local read: read colour attachment 0 of the same
// rendering as an input attachment, write it swizzled to attachment 1.
#version 450
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput c0;
layout(location = 1) out vec4 o1;

void main()
{
	vec4 c = subpassLoad(c0);
	o1 = vec4(c.b, c.g, c.r, 1.0);
}
