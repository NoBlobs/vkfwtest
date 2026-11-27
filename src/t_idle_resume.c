// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * idle-resume: render on the device, sleep longer than the runtime-PM
 * autosuspend delay (read from sysfs when present, or --idle-ms, else 500 ms),
 * and render again on the same VkDevice to exercise the warm-boot path.
 */
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "vkfwtest.h"

static uint32_t detect_autosuspend_ms(void)
{
	static const char *const patterns[] = {
		"/sys/bus/platform/devices/*.gpu/power/autosuspend_delay_ms",
		"/sys/class/drm/renderD*/device/power/autosuspend_delay_ms",
	};

	for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
		glob_t g = { 0 };

		if (glob(patterns[p], 0, nullptr, &g) != 0)
			continue;
		for (size_t i = 0; i < g.gl_pathc; i++) {
			FILE *f = fopen(g.gl_pathv[i], "r");
			long val = -1;

			if (!f)
				continue;
			if (fscanf(f, "%ld", &val) == 1 && val > 0 && val <= 60000) {
				fclose(f);
				globfree(&g);
				return (uint32_t)val;
			}
			fclose(f);
		}
		globfree(&g);
	}
	return 0;
}

enum result test_idle_resume(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t detected = detect_autosuspend_ms();
	uint32_t idle_ms = o->idle_ms ? o->idle_ms : (detected ? detected + 500u : 500u);
	struct opts o1 = *o, o2 = *o;

	o1.frames = OPT(o->frames, 2);
	o1.width = OPT(o->width, 64);
	o1.height = OPT(o->height, 64);
	o1.seed = OPT(o->seed, 1);

	CHECK(test_multiframe(v, &o1, r));

	struct timespec ts = {
		.tv_sec = idle_ms / 1000u,
		.tv_nsec = (long)(idle_ms % 1000u) * 1000000L,
	};
	nanosleep(&ts, nullptr);

	o2 = o1;
	o2.seed = o1.seed + 1000u;
	CHECK(test_multiframe(v, &o2, r));

	snprintf(r->detail, sizeof(r->detail),
		 "render -> idle %u ms%s -> render on same device", idle_ms,
		 o->idle_ms ? " (--idle-ms)" : detected ? " (sysfs + 500 ms)" : " (default)");
	return R_PASS;
}
