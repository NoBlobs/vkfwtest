// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * stress, soak, churn: concurrency, duration and context life cycle.
 *  stress: four threads, each with a device of its own, run multiframe,
 *          pre-ta, pre-cdm and xfer at the same time (the firmware sees four
 *          contexts submitting to three data masters at once);
 *  soak:   one device runs multiframe (>= 100 frames), pre-cdm, pre-ta and
 *          xfer in sequence: a long-lived context;
 *  churn:  creates and destroys devices in a loop (20 by default), each
 *          running a short render and a dispatch: context set-up and cleanup.
 * --frames scales the amount of work.
 */
#include <stdio.h>
#include <threads.h>

#include "vkfwtest.h"

struct job {
	const char *name;
	test_fn fn;
	struct opts o;
	struct report r;
	enum result res;
};

static int job_thread(void *arg)
{
	struct job *j = arg;

	j->res = run_fresh(&j->o, j->fn, &j->r);
	return 0;
}

static int sev(enum result r)
{
	return r == R_LOST ? 5 : r == R_TIMEOUT ? 4 : r == R_MISMATCH ? 3 : r == R_SETUP ? 2 : 0;
}

static void merge(struct report *r, const struct report *x)
{
	r->frames += x->frames;
	r->checked += x->checked;
	r->mismatches += x->mismatches;
}

static struct opts sub(const struct opts *o, uint32_t frames)
{
	struct opts s = *o;

	s.frames = frames;
	s.width = s.height = 0;
	s.iters = 0;
	s.tris = 0;
	s.groups = 0;
	return s;
}

enum result test_stress(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t n = OPT(o->frames, 20);
	struct job jobs[4] = {
		{ "multiframe", test_multiframe, sub(o, n), { 0 }, R_PASS },
		{ "pre-ta", test_preta, sub(o, n / 4 ? n / 4 : 1), { 0 }, R_PASS },
		{ "pre-cdm", test_precdm, sub(o, n), { 0 }, R_PASS },
		{ "xfer", test_xfer, sub(o, n / 10 ? n / 10 : 1), { 0 }, R_PASS },
	};
	thrd_t t[4];
	enum result worst = R_PASS;
	int bad = -1;

	(void)v;                /* every thread has a device of its own */
	for (uint32_t i = 0; i < 4; i++)
		if (thrd_create(&t[i], job_thread, &jobs[i]) != thrd_success) {
			snprintf(r->detail, sizeof(r->detail), "cannot start thread %u", i);
			for (uint32_t k = 0; k < i; k++)
				thrd_join(t[k], nullptr);
			return R_SETUP;
		}
	for (uint32_t i = 0; i < 4; i++) {
		thrd_join(t[i], nullptr);
		merge(r, &jobs[i].r);
		if (sev(jobs[i].res) > sev(worst)) {
			worst = jobs[i].res;
			bad = (int)i;
		}
	}
	if (bad >= 0) {
		snprintf(r->detail, sizeof(r->detail), "concurrent %s: %s %.150s", jobs[bad].name,
			 result_name(worst), jobs[bad].r.detail);
		return worst;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "4 devices at once: multiframe x%u, pre-ta x%u, pre-cdm x%u, xfer x%u", n,
		 jobs[1].o.frames, n, jobs[3].o.frames);
	return R_PASS;
}

enum result test_soak(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t n = OPT(o->frames, 120);
	struct {
		const char *name;
		test_fn fn;
		uint32_t frames;
	} steps[4] = {
		{ "multiframe", test_multiframe, n },
		{ "pre-cdm", test_precdm, n },
		{ "pre-ta", test_preta, n / 6 ? n / 6 : 1 },
		{ "xfer", test_xfer, n / 12 ? n / 12 : 1 },
	};

	for (uint32_t i = 0; i < 4; i++) {
		struct opts s = sub(o, steps[i].frames);
		struct report x = { 0 };
		enum result res = steps[i].fn(v, &s, &x);       /* the same device throughout */

		merge(r, &x);
		if (res != R_PASS) {
			snprintf(r->detail, sizeof(r->detail), "%s (after %u frames): %.150s",
				 steps[i].name, r->frames, x.detail);
			return res;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "one device: multiframe x%u, pre-cdm x%u, pre-ta x%u, xfer x%u", n, n,
		 steps[2].frames, steps[3].frames);
	return R_PASS;
}

enum result test_churn(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t n = OPT(o->frames, 20);
	struct opts s = sub(o, 2);

	(void)v;
	s.width = s.height = 64;
	for (uint32_t i = 0; i < n; i++) {
		enum result res = run_fresh(&s, test_multiframe, r);

		if (res == R_PASS) {
			struct opts c = sub(o, 1);

			res = run_fresh(&c, test_precdm, r);
		}
		if (res != R_PASS) {
			char d[160];

			snprintf(d, sizeof(d), "%.150s", r->detail);
			snprintf(r->detail, sizeof(r->detail), "device %u of %u: %.150s", i + 1, n, d);
			return res;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%u devices created and destroyed, each with 2 frames + 1 dispatch", n);
	return R_PASS;
}
