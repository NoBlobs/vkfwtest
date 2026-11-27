// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * vkfwtest: command line, test table and report.
 *
 *   vkfwtest [options] <test|group:NAME>... | all | list
 *
 * Every test creates its own Vulkan device (unless --keep-device), runs,
 * checks its output on the CPU and destroys the device. The exit code is the
 * worst result.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "vkfwtest.h"

#ifndef VKFWTEST_VERSION
#define VKFWTEST_VERSION "0.2"
#endif

struct test {
	const char *name;
	const char *alias;
	const char *group;
	test_fn fn;
	bool in_all;            /* run by `all`; disruptive and long tests are not */
	const char *desc;
};

static const struct test tests[] = {
	/* render */
	{ "multiframe", "pre-3d", "render", test_multiframe, true,
	  "TA + 3D over several frames on one device (alternating HWRTData); --iters for heavy fragments" },
	{ "pre-ta", nullptr, "render", test_preta, true,
	  "geometry-heavy frames: layers of tiny tiled triangles (--tris, default 100K)" },
	{ "bigpb", nullptr, "render", test_bigpb, true,
	  "parameter-buffer stress: millions of triangles + D32 depth (--tris, default 4M)" },
	{ "bigpb-layers", nullptr, "render", test_bigpb_layers, true,
	  "bigpb in a 4-layer array: the grid on layer 0, clears on layers 1-3 (partial renders x RTA)" },
	{ "input-attach", nullptr, "render", test_input_attach, true,
	  "two subpasses: subpass 1 reads colour + D32 depth of subpass 0 as input attachments" },
	{ "rta", nullptr, "render", test_rta, true,
	  "6-layer render target: layered clears, and gl_Layer draws when supported" },
	{ "zs-load", nullptr, "render", test_zs_load, true,
	  "depth + stencil loaded and stored across render passes" },
	{ "msaa", nullptr, "render", test_msaa, true,
	  "2x/4x/8x renders resolved in the render pass; 4x diagonal edge checked per sample" },
	{ "mrt", nullptr, "render", test_mrt, true,
	  "up to 8 colour attachments written by one subpass" },
	{ "sizes", nullptr, "render", test_sizes, true,
	  "render targets from 1x1 to 4096x4096 (unique value per pixel) + partial render area" },
	{ "vertex", nullptr, "render", test_vertex, true,
	  "vertex/index buffers (16/32-bit), instancing, points, lines" },
	{ "indirect", nullptr, "render", test_indirect, true,
	  "draw / indexed / multi-draw indirect and dispatch indirect" },
	{ "queries", nullptr, "render", test_queries, true,
	  "occlusion queries (host + GPU copy) and timestamps" },
	{ "formats", nullptr, "render", test_formats, true,
	  "RGB565, BGRA8, sRGB, RGBA16F, R32_UINT targets; additive blend; discard" },
	{ "secondary", nullptr, "render", test_secondary, true,
	  "secondary command buffers; vkCmdClearAttachments with partial rectangles between draws" },
	{ "multiview", nullptr, "render", test_multiview, true,
	  "VK_KHR_multiview: one draw into 2 layers with gl_ViewIndex" },
	{ "raster", nullptr, "render", test_raster, true,
	  "viewport offset / negative height, strips, fans, primitive restart, sample mask, no-attachment pass" },
	{ "load-store-none", nullptr, "render", test_load_store_none, true,
	  "attachments with STORE_OP_NONE / LOAD_OP_NONE stay untouched (1.3 / 1.4, optional)" },
	{ "local-read", nullptr, "render", test_local_read, true,
	  "dynamic rendering local read: draw, by-region barrier, read the attachment (1.4, optional)" },
	{ "vertex-ext", nullptr, "render", test_vertex_ext, true,
	  "8-bit indices and instance-rate divisors, incl. zero (1.4, optional)" },
	/* the second render on a device: one state change at a time */
	{ "second-same", nullptr, "second", test_second_same, true,
	  "two renders on one device, same framebuffer, new command buffer" },
	{ "second-resubmit", nullptr, "second", test_second_resubmit, true,
	  "one render command buffer submitted twice" },
	{ "second-newfb", nullptr, "second", test_second_newfb, true,
	  "second render into a new image + framebuffer (same render pass, pipeline)" },
	{ "second-newall", nullptr, "second", test_second_newall, true,
	  "second render with a new render pass, pipeline and framebuffer" },
	{ "second-compute", nullptr, "second", test_second_compute, true,
	  "render, compute dispatch, render (the indirect pattern)" },
	{ "second-xfer", nullptr, "second", test_second_xfer, true,
	  "render, transfer (buffer fill + image clear), render" },
	{ "second-split", nullptr, "second", test_second_split, true,
	  "second render, then its read-back copy in a later submission (no in-buffer ordering)" },
	{ "second-barrier", nullptr, "second", test_second_barrier, true,
	  "two renders; the copy also ordered by an explicit vkCmdPipelineBarrier" },
	/* compute */
	{ "pre-cdm", nullptr, "compute", test_precdm, true,
	  "compute dispatches writing an LCG pattern (--groups, --iters)" },
	{ "resources", nullptr, "compute", test_resources, true,
	  "UBO, dynamic SSBO offset, storage image, texel buffers, atomics, shared memory" },
	{ "compute-dims", nullptr, "compute", test_compute_dims, true,
	  "3D dispatch with vkCmdDispatchBase; the largest workgroup with a shared-memory reduction" },
	{ "subgroups", nullptr, "compute", test_subgroups, true,
	  "subgroup size, elect, ballot and arithmetic (when supported)" },
	/* transfer */
	{ "xfer", nullptr, "transfer", test_xfer, true,
	  "3D transfers: upload, offset copy, mirrored blit, MSAA resolve on NPOT and POT images" },
	{ "buffers", nullptr, "transfer", test_buffers, true,
	  "vkCmdCopyBuffer (several regions), vkCmdFillBuffer, vkCmdUpdateBuffer" },
	{ "ds-copy", nullptr, "transfer", test_ds_copy, true,
	  "vkCmdClearDepthStencilImage and depth image -> buffer copies" },
	{ "blit-mips", nullptr, "transfer", test_blit_mips, true,
	  "scaled LINEAR blits and a mip chain generated with blits" },
	{ "image-copy", nullptr, "transfer", test_image_copy, true,
	  "3D image, multi-layer and format-compatible (R32_UINT <-> RGBA8) copies" },
	/* texture */
	{ "texture", nullptr, "texture", test_texture, true,
	  "texture unit: NPOT, 4 address modes, 2 border colours, linear filter, 6 mip levels" },
	{ "texture2", nullptr, "texture", test_texture2, true,
	  "cube, 3D, array, shadow compare, gather, texelFetch, unnormalized, int/float, ETC2" },
	/* sync, memory, robustness, power */
	{ "sync", nullptr, "sync", test_sync, true,
	  "semaphores between submissions and queues, timeline semaphores, events, 500 small submits" },
	{ "sync2", nullptr, "sync", test_sync2, true,
	  "vkQueueSubmit2 with semaphores and vkCmdPipelineBarrier2 (1.3, optional)" },
	{ "memory", nullptr, "memory", test_memory, true,
	  "a large allocation, many small ones, map/unmap in a loop" },
	{ "sparse", nullptr, "memory", test_sparse, true,
	  "sparse buffer binding and residency (when supported)" },
	{ "robust", nullptr, "robust", test_robust, true,
	  "robustBufferAccess: out-of-bounds reads stay in bounds or read zero, no fault" },
	{ "idle-resume", nullptr, "power", test_idle_resume, true,
	  "render, wait past the runtime-PM autosuspend, render again on the same device" },
	{ "suspend-after", nullptr, "power", test_suspend_after, true,
	  "compute + sync work, then the GPU must runtime-suspend, then render (skip without runtime PM)" },
	/* stress */
	{ "stress", nullptr, "stress", test_stress, true,
	  "4 threads / devices rendering, computing and copying at once" },
	{ "soak", nullptr, "stress", test_soak, true,
	  "one long-lived device: >= 100 frames of render, compute, geometry, transfer" },
	{ "churn", nullptr, "stress", test_churn, true,
	  "create and destroy 20 devices, each rendering and computing" },
	{ "ctx-switch", nullptr, "stress", test_ctx_switch, true,
	  "2 devices (2 firmware contexts): per round A renders, B renders or computes, queued together" },
	/* disruptive / long */
	{ "hwr-lockup", nullptr, "hwr", test_hwr_lockup, false,
	  "[not in all] compute lockup -> device lost, then clean renders on new devices" },
	{ "hwr-fault", nullptr, "hwr", test_hwr_fault, false,
	  "[not in all] MMU page fault -> device lost, then clean renders on new devices" },
	{ "hwr-frag", nullptr, "hwr", test_hwr_frag, false,
	  "[not in all] fragment-shader lockup -> device lost, then clean renders on new devices" },
	{ "hwr-3d-fault", nullptr, "hwr", test_hwr_3d_fault, false,
	  "[not in all] 3D-pipe MMU page fault -> device lost, then clean renders on new devices" },
	{ "prio-3d", nullptr, "prio", test_prio_3d, false,
	  "[not in all] long LOW-priority fragment job vs HIGH-priority probes (preemption)" },
	{ "prio-ta", nullptr, "prio", test_prio_ta, false,
	  "[not in all] long LOW-priority geometry job vs HIGH-priority probes (preemption)" },
	{ "prio-cdm", nullptr, "prio", test_prio_cdm, false,
	  "[not in all] long LOW-priority compute job vs HIGH-priority probes (preemption)" },
};
constexpr size_t NTESTS = sizeof(tests) / sizeof(tests[0]);
constexpr size_t MAX_RUN = 256;

static void usage(FILE *f)
{
	fprintf(f,
		"usage: vkfwtest [options] <test|group:NAME>... | all | list\n"
		"  all runs every test except the hwr and prio groups (see 'vkfwtest list')\n"
		"  groups: render second compute transfer texture sync memory robust power stress hwr prio\n"
		"\n"
		"  -f, --frames N         frames / rounds per test (default per test)\n"
		"  -s, --size WxH         render target size (or N for NxN)\n"
		"  -t, --tris N[K|M]      triangles (pre-ta, bigpb)\n"
		"  -i, --iters N          LCG iterations per pixel / invocation\n"
		"  -g, --groups N         compute workgroups (pre-cdm, indirect)\n"
		"  -S, --seed N           pattern seed (default 1)\n"
		"  -T, --timeout SEC      per-fence timeout (default 60)\n"
		"  -L, --test-timeout SEC abort the run if one test takes longer (exit 2)\n"
		"  -r, --repeat N         run each test N times\n"
		"  -b, --bench N          run each test N times and print min/avg/max times\n"
		"  -p, --priority P       low|medium|high (VK_KHR/EXT_global_priority)\n"
		"  -d, --device N         physical device index (default: first PowerVR, else 0)\n"
		"  -K, --keep-device      run all the tests on one device (in order)\n"
		"  -A, --api X.Y          use at most Vulkan X.Y (instance and device)\n"
		"  -I, --idle-ms MS       idle-resume: idle time (default: autosuspend delay + 500)\n"
		"  -D, --dump DIR         write got/expected/diff PPM images of mismatches to DIR\n"
		"  -B, --baseline FILE    compare each result with FILE (\"<test> <RESULT>\" lines);\n"
		"                         the exit code then fails only when a test is worse\n"
		"  -P, --require-preempt  prio-*: fail unless the probes overtake the long job\n"
		"  -k, --keep-going       run the remaining tests after a failure\n"
		"  -j, --json             one JSON line per test, and a summary line, on stdout\n"
		"  -v, --verbose          more output (repeat for more)\n"
		"  -q, --quiet            no diagnostics, only result lines\n"
		"  -V, --version          print the version\n"
		"  -h, --help             this text\n"
		"\n"
		"Exit codes: 0 pass, 1 output mismatch, 2 fence or test timeout, 3 device lost,\n"
		"            4 setup error / bad option, 77 all tests skipped.\n"
		"            With --baseline: 0 unless a test is worse than its baseline.\n");
}

static void list(bool json)
{
	if (json)
		printf("[");
	for (size_t i = 0; i < NTESTS; i++) {
		if (json)
			printf("%s{\"name\":\"%s\",\"alias\":\"%s\",\"group\":\"%s\",\"in_all\":%s,"
			       "\"desc\":\"%s\"}", i ? "," : "", tests[i].name,
			       tests[i].alias ? tests[i].alias : "", tests[i].group,
			       tests[i].in_all ? "true" : "false", tests[i].desc);
		else
			printf("%-13s %-7s %-9s %s\n", tests[i].name,
			       tests[i].alias ? tests[i].alias : "", tests[i].group, tests[i].desc);
	}
	if (json)
		printf("]\n");
}

static const struct test *find(const char *name)
{
	for (size_t i = 0; i < NTESTS; i++)
		if (!strcmp(name, tests[i].name) ||
		    (tests[i].alias && !strcmp(name, tests[i].alias)))
			return &tests[i];
	return nullptr;
}

/* ------------------------------------------------------------ baseline */
/*
 * --baseline FILE: the expected result of each test on a reference stack, one
 * "<test> <PASS|FAIL|SKIP|TIMEOUT|LOST|ERROR>" per line; '#' starts a comment
 * and anything after the result is ignored. Each result is compared with it.
 */
constexpr size_t MAX_BASE = 256;
static struct { char name[32]; char result[8]; } base[MAX_BASE];
static size_t nbase;

static bool baseline_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];

	if (!f) {
		fprintf(stderr, "vkfwtest: cannot open baseline %s\n", path);
		return false;
	}
	while (fgets(line, sizeof(line), f)) {
		char name[32], res[16];
		char *hash = strchr(line, '#');

		if (hash)
			*hash = '\0';
		if (sscanf(line, "%31s %15s", name, res) != 2)
			continue;
		if (nbase == MAX_BASE)
			break;
		if (strcmp(res, "PASS") && strcmp(res, "FAIL") && strcmp(res, "SKIP") &&
		    strcmp(res, "TIMEOUT") && strcmp(res, "LOST") && strcmp(res, "ERROR")) {
			fprintf(stderr, "vkfwtest: baseline %s: bad result '%s' for %s\n", path, res,
				name);
			fclose(f);
			return false;
		}
		strcpy(base[nbase].name, name);
		strcpy(base[nbase].result, res);
		nbase++;
	}
	fclose(f);
	return true;
}

static const char *baseline_of(const char *test)
{
	for (size_t i = 0; i < nbase; i++)
		if (!strcmp(base[i].name, test))
			return base[i].result;
	return nullptr;
}

/* PASS best, then SKIP, FAIL/ERROR, TIMEOUT, LOST */
static int base_rank(const char *res)
{
	if (!strcmp(res, "PASS"))
		return 0;
	if (!strcmp(res, "SKIP"))
		return 1;
	if (!strcmp(res, "TIMEOUT"))
		return 3;
	if (!strcmp(res, "LOST"))
		return 4;
	return 2;
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

/* ------------------------------------------------------------ test watchdog */
static atomic_uint_fast64_t wd_deadline_ms;     /* 0: no test running */
static const char *_Atomic wd_test;

static int watchdog(void *arg)
{
	bool json = *(const bool *)arg;

	for (;;) {
		struct timespec t = { 0, 200000000L };
		uint64_t d = atomic_load(&wd_deadline_ms);

		nanosleep(&t, nullptr);
		if (d && (uint64_t)now_ms() > d) {
			const char *name = atomic_load(&wd_test);

			if (json)
				printf("{\"test\":\"%s\",\"result\":\"TIMEOUT\",\"code\":2,"
				       "\"detail\":\"test time limit exceeded\"}\n", name);
			else
				printf("TIMEOUT %-12s  test time limit exceeded\n", name);
			fflush(stdout);
			_Exit(R_TIMEOUT);
		}
	}
	return 0;
}

/* ------------------------------------------------------------ run */
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

/* `shared` is the device kept between tests with --keep-device, else null. */
static enum result run_one(const struct test *t, const struct opts *o, uint32_t iter,
			   struct vk *shared, double *ms_out)
{
	struct vk own, *v = shared ? shared : &own;
	struct report rep = { 0 };
	double t0 = now_ms();
	enum result r = R_PASS;

	if (o->test_timeout_s) {
		atomic_store(&wd_test, t->name);
		atomic_store(&wd_deadline_ms, (uint64_t)t0 + (uint64_t)o->test_timeout_s * 1000u);
	}
	if (!shared || !v->dev)
		r = vk_init(v, o);
	if (r == R_PASS) {
		v->test = t->name;
		r = t->fn(v, o, &rep);
	}
	if (!shared || v->lost)
		vk_fini(v);
	atomic_store(&wd_deadline_ms, 0);

	double ms = now_ms() - t0;

	if (ms_out)
		*ms_out = ms;
	if (o->json) {
		printf("{\"test\":");
		json_str(t->name);
		printf(",\"group\":");
		json_str(t->group);
		printf(",\"iteration\":%" PRIu32 ",\"result\":", iter);
		json_str(result_name(r));
		printf(",\"code\":%d,\"ms\":%.1f,\"frames\":%" PRIu32 ",\"checked\":%" PRIu64
		       ",\"mismatches\":%" PRIu64 ",\"device\":",
		       (int)r, ms, rep.frames, rep.checked, rep.mismatches);
		json_str(v->props.deviceName);
		printf(",\"detail\":");
		json_str(rep.detail);
		printf("}\n");
	} else {
		printf("%-7s %-12s %9.1f ms  %s\n", result_name(r), t->name, ms, rep.detail);
	}
	fflush(stdout);
	return r;
}

static bool add(const struct test **run, size_t *n, const struct test *t)
{
	if (*n >= MAX_RUN)
		return false;
	run[(*n)++] = t;
	return true;
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
		{ "test-timeout", required_argument, nullptr, 'L' },
		{ "repeat", required_argument, nullptr, 'r' },
		{ "bench", required_argument, nullptr, 'b' },
		{ "priority", required_argument, nullptr, 'p' },
		{ "device", required_argument, nullptr, 'd' },
		{ "keep-device", no_argument, nullptr, 'K' },
		{ "api", required_argument, nullptr, 'A' },
		{ "idle-ms", required_argument, nullptr, 'I' },
		{ "dump", required_argument, nullptr, 'D' },
		{ "baseline", required_argument, nullptr, 'B' },
		{ "keep-going", no_argument, nullptr, 'k' },
		{ "require-preempt", no_argument, nullptr, 'P' },
		{ "json", no_argument, nullptr, 'j' },
		{ "verbose", no_argument, nullptr, 'v' },
		{ "quiet", no_argument, nullptr, 'q' },
		{ "version", no_argument, nullptr, 'V' },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};
	struct opts o = { .device = -1 };
	const char *baseline = nullptr;
	int c;

	while ((c = getopt_long(argc, argv, "f:s:t:i:g:S:T:L:r:b:p:d:KA:I:D:B:PkjvqVh", lo, nullptr)) !=
	       -1) {
		bool ok = true;

		switch (c) {
		case 'f': ok = parse_u32(optarg, &o.frames); break;
		case 's': ok = parse_size(optarg, &o.width, &o.height); break;
		case 't': ok = parse_u64(optarg, &o.tris, true); break;
		case 'i': ok = parse_u32(optarg, &o.iters); break;
		case 'g': ok = parse_u32(optarg, &o.groups); break;
		case 'S': ok = parse_u32(optarg, &o.seed); break;
		case 'T': ok = parse_u32(optarg, &o.timeout_s); break;
		case 'L': ok = parse_u32(optarg, &o.test_timeout_s); break;
		case 'r': ok = parse_u32(optarg, &o.repeat); break;
		case 'b': ok = parse_u32(optarg, &o.bench) && o.bench > 0; break;
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
		case 'K': o.keep_device = true; break;
		case 'A': {
			unsigned maj, min;

			ok = sscanf(optarg, "%u.%u", &maj, &min) == 2 && maj == 1 && min <= 9;
			if (ok)
				o.max_api = VK_MAKE_API_VERSION(0, maj, min, 0);
			break;
		}
		case 'I': ok = parse_u32(optarg, &o.idle_ms); break;
		case 'D': o.dump = optarg; break;
		case 'B': baseline = optarg; break;
		case 'P': o.require_preempt = true; break;
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
	if (baseline && !baseline_load(baseline))
		return R_SETUP;

	/* the tests to run, in command-line order */
	static const struct test *run[MAX_RUN];
	size_t n = 0;

	for (int a = optind; a < argc; a++) {
		if (!strcmp(argv[a], "list")) {
			list(o.json);
			return 0;
		}
		if (!strcmp(argv[a], "all")) {
			for (size_t i = 0; i < NTESTS; i++)
				if (tests[i].in_all)
					add(run, &n, &tests[i]);
			continue;
		}
		if (!strncmp(argv[a], "group:", 6)) {
			bool any = false;

			for (size_t i = 0; i < NTESTS; i++)
				if (!strcmp(tests[i].group, argv[a] + 6)) {
					add(run, &n, &tests[i]);
					any = true;
				}
			if (!any) {
				fprintf(stderr, "vkfwtest: unknown group '%s'\n", argv[a] + 6);
				return R_SETUP;
			}
			continue;
		}
		const struct test *t = find(argv[a]);

		if (!t) {
			fprintf(stderr, "vkfwtest: unknown test '%s' (try 'vkfwtest list')\n", argv[a]);
			return R_SETUP;
		}
		add(run, &n, t);
	}

	thrd_t wd;

	if (o.test_timeout_s && thrd_create(&wd, watchdog, &o.json) == thrd_success)
		thrd_detach(wd);

	enum result worst = R_PASS;
	uint32_t count[6] = { 0 };      /* pass, mismatch, timeout, lost, setup, skip */
	struct { const char *name; uint32_t pass, total; } grps[16];
	size_t ngrps = 0;
	static struct vk shared;
	uint32_t reps = o.bench ? o.bench : OPT(o.repeat, 1);
	bool stop = false;
	uint32_t bsame = 0, bbetter = 0, bworse = 0, bnone = 0;
	enum result bworst = R_PASS;

	for (size_t i = 0; i < n && !stop; i++) {
		double tmin = 1e30, tmax = 0, tsum = 0;
		size_t gi = 0;

		while (gi < ngrps && strcmp(grps[gi].name, run[i]->group))
			gi++;
		if (gi == ngrps && ngrps < 16) {
			grps[ngrps].name = run[i]->group;
			grps[ngrps].pass = 0;
			grps[ngrps].total = 0;
			ngrps++;
		}

		for (uint32_t it = 0; it < reps; it++) {
			double ms;
			enum result r = run_one(run[i], &o, it, o.keep_device ? &shared : nullptr, &ms);

			count[r == R_SKIP ? 5 : (int)r]++;
			if (baseline) {
				const char *b = baseline_of(run[i]->name), *now = result_name(r);
				const char *change = "same";

				if (!b) {
					bnone++;
					change = "new";
				} else if (!strcmp(b, now)) {
					bsame++;
				} else if (base_rank(now) < base_rank(b)) {
					bbetter++;
					change = "better";
				} else {
					bworse++;
					change = "worse";
					if (severity(r) > severity(bworst))
						bworst = r == R_SKIP ? R_MISMATCH : r;
				}
				if (strcmp(change, "same")) {
					if (o.json)
						printf("{\"baseline_diff\":{\"test\":\"%s\",\"baseline\":\"%s\","
						       "\"result\":\"%s\",\"change\":\"%s\"}}\n",
						       run[i]->name, b ? b : "", now, change);
					else
						printf("BASE    %-12s %s (baseline %s, now %s)\n",
						       run[i]->name, change, b ? b : "none", now);
					fflush(stdout);
				}
			}
			if (gi < ngrps) {
				grps[gi].total++;
				if (r == R_PASS)
					grps[gi].pass++;
			}
			if (severity(r) > severity(worst) && r != R_SKIP)
				worst = r;
			tmin = ms < tmin ? ms : tmin;
			tmax = ms > tmax ? ms : tmax;
			tsum += ms;
			if (r != R_PASS && r != R_SKIP && !o.keep_going) {
				stop = true;
				break;
			}
		}
		if (o.bench && !stop) {
			if (o.json)
				printf("{\"bench\":\"%s\",\"runs\":%u,\"min_ms\":%.1f,\"avg_ms\":%.1f,"
				       "\"max_ms\":%.1f}\n", run[i]->name, reps, tmin, tsum / reps, tmax);
			else
				printf("BENCH   %-12s %u runs: min %.1f ms, avg %.1f ms, max %.1f ms\n",
				       run[i]->name, reps, tmin, tsum / reps, tmax);
		}
	}
	if (o.keep_device)
		vk_fini(&shared);

	int code = worst;

	if (worst == R_PASS && count[5] && !count[0])
		code = R_SKIP;
	if (baseline)
		code = bworst;
	if (o.json) {
		printf("{\"summary\":{\"pass\":%u,\"fail\":%u,\"timeout\":%u,\"lost\":%u,\"error\":%u,"
		       "\"skip\":%u,\"code\":%d,\"groups\":{", count[0], count[1], count[2], count[3],
		       count[4], count[5], code);
		for (size_t gi = 0; gi < ngrps; gi++)
			printf("%s\"%s\":{\"pass\":%u,\"total\":%u}", gi ? "," : "",
			       grps[gi].name, grps[gi].pass, grps[gi].total);
		printf("}");
		if (baseline)
			printf(",\"baseline\":{\"file\":\"%s\",\"same\":%u,\"better\":%u,"
			       "\"worse\":%u,\"new\":%u}", baseline, bsame, bbetter, bworse, bnone);
		printf("}}\n");
	} else if (n > 1) {
		printf("== %u pass, %u fail, %u timeout, %u lost, %u error, %u skip\n", count[0],
		       count[1], count[2], count[3], count[4], count[5]);
		if (ngrps > 1) {
			printf("== groups:");
			for (size_t gi = 0; gi < ngrps; gi++)
				printf(" %s %u/%u", grps[gi].name, grps[gi].pass, grps[gi].total);
			printf("\n");
		}
	}
	if (baseline && !o.json)
		printf("== baseline %s: %u same, %u better, %u worse, %u not in it\n", baseline,
		       bsame, bbetter, bworse, bnone);
	return code;
}
