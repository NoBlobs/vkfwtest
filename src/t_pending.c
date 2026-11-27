// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Tests registered but not implemented yet (docs/steps.md): they report SKIP.
 * Each moves to its own t_*.c file when written; their shaders are already in
 * shaders/.
 */
#include <stdio.h>

#include "vkfwtest.h"

#define PENDING(n)                                                              \
	enum result test_##n(struct vk *v, const struct opts *o, struct report *r) \
	{                                                                       \
		(void)v;                                                        \
		(void)o;                                                        \
		snprintf(r->detail, sizeof(r->detail), "not implemented yet (docs/steps.md)"); \
		return R_SKIP;                                                  \
	}

PENDING(secondary)
PENDING(multiview)
PENDING(raster)
PENDING(resources)
PENDING(compute_dims)
PENDING(subgroups)
PENDING(buffers)
PENDING(ds_copy)
PENDING(blit_mips)
PENDING(image_copy)
PENDING(texture2)
PENDING(sync)
PENDING(memory)
PENDING(robust)
PENDING(idle_resume)
PENDING(hwr_frag)
