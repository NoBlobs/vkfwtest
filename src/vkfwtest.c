// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * vkfwtest: command line, test table and report.
 *
 *   vkfwtest [options] <test>... | all | list
 *
 * Every test creates its own Vulkan device, runs, checks its output on the
 * CPU and destroys the device. The exit code is the worst result.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vkfwtest.h"

#ifndef VKFWTEST_VERSION
#define VKFWTEST_VERSION "0.1"
#endif

struct test {
	const char *name;
	const char *alias;
	test_fn fn;
	const char *desc;
};

static const struct test tests[] = {
	{ "multiframe", "pre-3d", test_multiframe,
	  "TA + 3D over several frames on one device (alternating HWRTData); --iters for heavy fragments" },
	{ "pre-ta", nullptr, test_preta,
	  "geometry-heavy frames: layers of tiny tiled triangles (--tris, default 100K)" },
	{ "pre-cdm", nullptr, test_precdm,
	  "compute dispatches writing an LCG pattern (--groups, --iters)" },
	{ "xfer", nullptr, test_xfer,
	  "3D transfers: upload, offset copy, mirrored blit, MSAA resolve on NPOT and POT images" },
	{ "bigpb", nullptr, test_bigpb,
	  "parameter-buffer stress: millions of triangles + D32 depth (--tris, default 4M)" },
};
constexpr size_t NTESTS = sizeof(tests) / sizeof(tests[0]);

static void usage(FILE *f)
{
	fprintf(f,
		"usage: vkfwtest [options] <test>... | all | list\n"
		"\n"
		"  -f, --frames N      frames / rounds per test (default per test)\n"
		"  -s, --size WxH      render target size (or N for NxN)\n"
		"  -t, --tris N[K|M]   triangles (pre-ta, bigpb)\n"
		"  -i, --iters N       LCG iterations per pixel / invocation\n"
		"  -g, --groups N      compute workgroups (pre-cdm)\n"
		"  -S, --seed N        pattern seed (default 1)\n"
		"  -T, --timeout SEC   per-fence timeout (default 60)\n"
		"  -r, --repeat N      run each test N times\n"
		"  -p, --priority P    low|medium|high (VK_KHR/EXT_global_priority)\n"
		"  -d, --device N      physical device index (default: first PowerVR, else 0)\n"
		"  -k, --keep-going    run the remaining tests after a failure\n"
		"  -j, --json          one JSON line per test on stdout\n"
		"  -v, --verbose       more output (repeat for more)\n"
		"  -q, --quiet         no diagnostics, only result lines\n"
		"  -V, --version       print the version\n"
		"  -h, --help          this text\n"
		"\n"
		"Exit codes: 0 pass, 1 output mismatch, 2 fence timeout, 3 device lost,\n"
		"            4 setup error / bad option, 77 all tests skipped.\n");
}

static void list(void)
{
	for (size_t i = 0; i < NTESTS; i++)
		printf("%-11s %-8s %s\n", tests[i].name, tests[i].alias ? tests[i].alias : "",
		       tests[i].desc);
}

static const struct test *find(const char *name)
{
	for (size_t i = 0; i < NTESTS; i++)
		if (!strcmp(name, tests[i].name) ||
		    (tests[i].alias && !strcmp(name, tests[i].alias)))
			return &tests[i];
	return nullptr;
}

/* ------------------------------------------------------------ option parsing */
static bool parse_u64(const char *s, uint64_t *out, bool suffix)
{
	char *end;
	uint64_t mul = 1;

	errno = 0;
	unsigned long long x = strtoull(s, &end, 0);

	if (errno || end == s || *s == '-')
		return false;
	if (suffix && (*end == 'k' || *end == 'K'))
		mul = 1000, end++;
	else if (suffix && (*end == 'm' || *end == 'M'))
		mul = 1000000, end++;
	if (*end)
		return false;
	*out = (uint64_t)x * mul;
	return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
	uint64_t x;

	if (!parse_u64(s, &x, false) || x > UINT32_MAX)
		return false;
	*out = (uint32_t)x;
	return true;
}

static bool parse_size(const char *s, uint32_t *w, uint32_t *h)
{
	char buf[32], *x;

	if (strlen(s) >= sizeof(buf))
		return false;
	strcpy(buf, s);
	x = strpbrk(buf, "xX");
	if (!x)
		return parse_u32(buf, w) && (*h = *w, *w > 0);
	*x = '\0';
	return parse_u32(buf, w) && parse_u32(x + 1, h) && *w > 0 && *h > 0;
}

/* ------------------------------------------------------------ run */
static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Worst result first: lost > timeout > mismatch > setup > skip > pass. */
static int severity(enum result r)
{
	switch (r) {
	case R_LOST: return 5;
	case R_TIMEOUT: return 4;
	case R_MISMATCH: return 3;
	case R_SETUP: return 2;
	case R_SKIP: return 1;
	case R_PASS: return 0;
	}
	return 0;
}

static void json_str(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			putchar('\\');
		if ((unsigned char)*s >= 0x20)
			putchar(*s);
	}
	putchar('"');
}

static enum result run_one(const struct test *t, const struct opts *o, uint32_t iter)
{
	struct vk v;
	struct report rep = { 0 };
	double t0 = now_ms();
	enum result r = vk_init(&v, o);

	if (r == R_PASS)
		r = t->fn(&v, o, &rep);
	vk_fini(&v);

	double ms = now_ms() - t0;

	if (o->json) {
		printf("{\"test\":");
		json_str(t->name);
		printf(",\"iteration\":%" PRIu32 ",\"result\":", iter);
		json_str(result_name(r));
		printf(",\"code\":%d,\"ms\":%.1f,\"frames\":%" PRIu32 ",\"checked\":%" PRIu64
		       ",\"mismatches\":%" PRIu64 ",\"device\":",
		       (int)r, ms, rep.frames, rep.checked, rep.mismatches);
		json_str(v.props.deviceName);
		printf(",\"detail\":");
		json_str(rep.detail);
		printf("}\n");
	} else {
		printf("%-7s %-10s %9.1f ms  %s\n", result_name(r), t->name, ms, rep.detail);
	}
	fflush(stdout);
	return r;
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "frames", required_argument, nullptr, 'f' },
		{ "size", required_argument, nullptr, 's' },
		{ "tris", required_argument, nullptr, 't' },
		{ "iters", required_argument, nullptr, 'i' },
		{ "groups", required_argument, nullptr, 'g' },
		{ "seed", required_argument, nullptr, 'S' },
		{ "timeout", required_argument, nullptr, 'T' },
		{ "repeat", required_argument, nullptr, 'r' },
		{ "priority", required_argument, nullptr, 'p' },
		{ "device", required_argument, nullptr, 'd' },
		{ "keep-going", no_argument, nullptr, 'k' },
		{ "json", no_argument, nullptr, 'j' },
		{ "verbose", no_argument, nullptr, 'v' },
		{ "quiet", no_argument, nullptr, 'q' },
		{ "version", no_argument, nullptr, 'V' },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};
	struct opts o = { .device = -1 };
	int c;

	while ((c = getopt_long(argc, argv, "f:s:t:i:g:S:T:r:p:d:kjvqVh", lo, nullptr)) != -1) {
		bool ok = true;

		switch (c) {
		case 'f': ok = parse_u32(optarg, &o.frames); break;
		case 's': ok = parse_size(optarg, &o.width, &o.height); break;
		case 't': ok = parse_u64(optarg, &o.tris, true); break;
		case 'i': ok = parse_u32(optarg, &o.iters); break;
		case 'g': ok = parse_u32(optarg, &o.groups); break;
		case 'S': ok = parse_u32(optarg, &o.seed); break;
		case 'T': ok = parse_u32(optarg, &o.timeout_s); break;
		case 'r': ok = parse_u32(optarg, &o.repeat); break;
		case 'p':
			if (!strcmp(optarg, "low"))
				o.priority = PRIO_LOW;
			else if (!strcmp(optarg, "medium"))
				o.priority = PRIO_MEDIUM;
			else if (!strcmp(optarg, "high"))
				o.priority = PRIO_HIGH;
			else
				ok = false;
			break;
		case 'd': {
			uint32_t d;

			ok = parse_u32(optarg, &d) && d <= INT32_MAX;
			o.device = ok ? (int)d : -1;
			break;
		}
		case 'k': o.keep_going = true; break;
		case 'j': o.json = true; break;
		case 'v': o.verbose++; break;
		case 'q': o.verbose = -1; break;
		case 'V': printf("vkfwtest %s\n", VKFWTEST_VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return R_SETUP;
		}
		if (!ok) {
			fprintf(stderr, "vkfwtest: bad value for -%c: %s\n", c, optarg);
			return R_SETUP;
		}
	}
	if (optind >= argc) {
		usage(stderr);
		return R_SETUP;
	}

	/* the tests to run, in command-line order */
	const struct test *run[64];
	size_t n = 0;

	for (int a = optind; a < argc; a++) {
		if (!strcmp(argv[a], "list")) {
			list();
			return 0;
		}
		if (!strcmp(argv[a], "all")) {
			for (size_t i = 0; i < NTESTS && n < 64; i++)
				run[n++] = &tests[i];
			continue;
		}
		const struct test *t = find(argv[a]);

		if (!t) {
			fprintf(stderr, "vkfwtest: unknown test '%s' (try 'vkfwtest list')\n", argv[a]);
			return R_SETUP;
		}
		if (n < 64)
			run[n++] = t;
	}

	enum result worst = R_PASS;
	bool all_skipped = true;

	for (size_t i = 0; i < n; i++) {
		for (uint32_t it = 0; it < OPT(o.repeat, 1); it++) {
			enum result r = run_one(run[i], &o, it);

			if (r != R_SKIP)
				all_skipped = false;
			if (severity(r) > severity(worst) && r != R_SKIP)
				worst = r;
			if (r != R_PASS && r != R_SKIP && !o.keep_going)
				return r;
		}
	}
	if (all_skipped && n)
		return R_SKIP;
	return worst;
}
