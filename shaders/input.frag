// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
// Second subpass: read subpass 0's colour and depth as input attachments and
// write a derived colour: colour swizzled to BGR, alpha 1 where depth < 0.75.
#version 450
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput c0;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput d0;
layout(location = 0) out vec4 o;

void main()
{
	vec4 c = subpassLoad(c0);
	float d = subpassLoad(d0).r;
	o = vec4(c.b, c.g, c.r, d < 0.75 ? 1.0 : 0.0);
}
