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
#include <string.h>
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

/*
 * The power directory of the GPU with runtime PM on (control = auto) and its
 * autosuspend delay; false if there is none (desktop GPUs often keep "on").
 */
static bool pm_dir(char *dir, size_t n, uint32_t *delay_ms)
{
	static const char *const patterns[] = {
		"/sys/bus/platform/devices/*.gpu/power",
		"/sys/class/drm/renderD*/device/power",
	};

	for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
		glob_t g = { 0 };

		if (glob(patterns[p], 0, nullptr, &g) != 0)
			continue;
		for (size_t i = 0; i < g.gl_pathc; i++) {
			char path[512], ctl[16] = "";
			long d = -1;
			FILE *f;

			snprintf(path, sizeof(path), "%s/control", g.gl_pathv[i]);
			if ((f = fopen(path, "r"))) {
				if (!fgets(ctl, sizeof(ctl), f))
					ctl[0] = '\0';
				fclose(f);
			}
			snprintf(path, sizeof(path), "%s/autosuspend_delay_ms", g.gl_pathv[i]);
			if ((f = fopen(path, "r"))) {
				if (fscanf(f, "%ld", &d) != 1)
					d = -1;
				fclose(f);
			}
			if (!strncmp(ctl, "auto", 4) && d >= 0 && d <= 60000) {
				snprintf(dir, n, "%s", g.gl_pathv[i]);
				*delay_ms = (uint32_t)d;
				globfree(&g);
				return true;
			}
		}
		globfree(&g);
	}
	return false;
}

static bool runtime_suspended(const char *dir)
{
	char path[512], st[32] = "";
	FILE *f;

	snprintf(path, sizeof(path), "%s/runtime_status", dir);
	if (!(f = fopen(path, "r")))
		return false;
	if (!fgets(st, sizeof(st), f))
		st[0] = '\0';
	fclose(f);
	return !strncmp(st, "suspended", 9);
}

/*
 * suspend-after: the GPU must runtime-suspend after compute and sync work
 * (E043 saw it stay active after those suites), then render again. Waits up
 * to the autosuspend delay + 3 s for "suspended". R_SKIP without runtime PM.
 */
enum result test_suspend_after(struct vk *v, const struct opts *o, struct report *r)
{
	char dir[512];
	uint32_t delay, waited = 0;
	struct opts oc = *o, om = *o;

	if (!pm_dir(dir, sizeof(dir), &delay)) {
		snprintf(r->detail, sizeof(r->detail), "no GPU with runtime PM (control = auto)");
		return R_SKIP;
	}
	oc.frames = 0;
	oc.width = oc.height = 0;
	CHECK(test_precdm(v, &oc, r));
	CHECK(test_sync(v, &oc, r));
	while (!runtime_suspended(dir) && waited < delay + 3000u) {
		struct timespec ts = { 0, 50 * 1000000L };

		nanosleep(&ts, nullptr);
		waited += 50;
	}
	if (!runtime_suspended(dir)) {
		snprintf(r->detail, sizeof(r->detail),
			 "still active %u ms after compute + sync (autosuspend %u ms)", waited, delay);
		LOG(v, 0, "vkfwtest: suspend-after: %s/runtime_status is not suspended\n", dir);
		return R_MISMATCH;
	}
	/* and it must come back: one frame */
	om.frames = 1;
	om.width = om.height = 64;
	om.seed = OPT(o->seed, 1) + 2000u;
	CHECK(test_multiframe(v, &om, r));
	snprintf(r->detail, sizeof(r->detail),
		 "compute + sync, suspended after %u ms (autosuspend %u ms), then a render", waited,
		 delay);
	return R_PASS;
}
