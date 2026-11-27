// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * prio-3d / prio-ta / prio-cdm (oracle stage F): a long job on a LOW-priority
 * device while short probes run on a HIGH-priority device (two devices, so
 * two firmware contexts of different priority). The long job is calibrated to
 * about 1.5 s. Every output is checked; the probe latencies show whether the
 * firmware preempted (context switch) the long job. --require-preempt makes a
 * missing overtake a failure.
 */
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"
#include "fill.comp.h"

enum kind { K_3D, K_TA, K_CDM };

constexpr double TARGET_MS = 1500.0;
constexpr uint32_t PROBES = 10;

struct job {
	struct vk *v;
	enum kind k;
	uint32_t w, h, seed;
	struct img color;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;
	struct grid g;
	struct buf ssbo;
	VkDescriptorSet set;
	uint32_t groups;
};

struct color_pc {
	uint32_t color, iters, seed;
};

struct fill_pc {
	uint32_t seed, iters;
};

struct lcg_expect {
	uint32_t iters, seed;
};

static struct rgba lcg_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct lcg_expect *e = ctx;

	return rgba_of(lcg(e->seed + x + y * 65536u, e->iters) | 0xFF000000u);
}

static void sleep_ms(uint32_t ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&t, nullptr);
}

static enum result job_setup(struct job *j, struct vk *v, enum kind k, uint32_t seed)
{
	*j = (struct job){ .v = v, .k = k, .seed = seed, .w = 256, .h = 256, .groups = 64 };
	if (k == K_CDM) {
		CHECK(buf_create(v, &j->ssbo, (VkDeviceSize)j->groups * 64 * 4,
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
		return compute_pipeline(v, spv_fill_comp, sizeof(spv_fill_comp),
					sizeof(struct fill_pc), &j->ssbo, &j->layout, &j->pipe, &j->set);
	}
	CHECK(img_create(v, &j->color, j->w, j->h, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &j->rb, (VkDeviceSize)j->w * j->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			 true));
	CHECK(rp_create(v, &j->rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &j->fb, j->rp, &j->color, nullptr));
	if (k == K_TA)
		return grid_pipeline(v, &j->pipe, &j->layout, j->rp, false);

	VkShaderModule vs, fs;

	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &j->layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc d = { .rp = j->rp, .layout = j->layout, .vs = vs, .fs = fs };

	return gfx_pipeline(v, &j->pipe, &d);
}

/* Record the job with work amount n: LCG iterations (3D, CDM) or triangles (TA). */
static enum result job_record(struct job *j, uint32_t n, VkCommandBuffer *cb)
{
	CHECK(cmd_begin(j->v, cb));
	if (j->k == K_CDM) {
		struct fill_pc pc = { j->seed, n };
		VkBufferMemoryBarrier hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = j->ssbo.b,
			.size = VK_WHOLE_SIZE,
		};

		vkCmdBindPipeline(*cb, VK_PIPELINE_BIND_POINT_COMPUTE, j->pipe);
		vkCmdBindDescriptorSets(*cb, VK_PIPELINE_BIND_POINT_COMPUTE, j->layout, 0, 1, &j->set,
					0, nullptr);
		vkCmdPushConstants(*cb, j->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(*cb, j->groups, 1, 1);
		vkCmdPipelineBarrier(*cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		return R_PASS;
	}

	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = j->rp,
		.framebuffer = j->fb,
		.renderArea = { { 0, 0 }, { j->w, j->h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)j->w, (float)j->h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { j->w, j->h } };

	vkCmdBeginRenderPass(*cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(*cb, VK_PIPELINE_BIND_POINT_GRAPHICS, j->pipe);
	vkCmdSetViewport(*cb, 0, 1, &vp);
	vkCmdSetScissor(*cb, 0, 1, &sc);
	if (j->k == K_TA) {
		grid_plan(&j->g, j->w, j->h, n, 2);
		j->g.seed = j->seed;
		j->g.final_layer = j->g.layers - 1;
		grid_draw(*cb, j->layout, &j->g, false);
	} else {
		struct color_pc pc = { 0, n, j->seed };

		vkCmdPushConstants(*cb, j->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDraw(*cb, 3, 1, 0, 0);
	}
	vkCmdEndRenderPass(*cb);
	cmd_copy_to_buffer(*cb, &j->color, &j->rb);
	return R_PASS;
}

static enum result job_check(struct job *j, uint32_t n, struct report *r, const char *what)
{
	if (j->k == K_TA)
		return check_rgba(j->v, r, what, j->rb.map, j->w, j->h, grid_expect, &j->g);
	if (j->k == K_3D) {
		struct lcg_expect e = { n, j->seed };

		return check_rgba(j->v, r, what, j->rb.map, j->w, j->h, lcg_fn, &e);
	}
	uint32_t words = j->groups * 64;
	uint64_t bad = 0;

	for (uint32_t i = 0; i < words; i++) {
		const uint8_t *p = j->ssbo.map + (size_t)i * 4;
		uint32_t got = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
			       (uint32_t)p[3] << 24;

		bad += got != lcg(i ^ j->seed, n);
	}
	r->checked += words;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail), "%s: %" PRIu64 " of %u words wrong", what,
			 bad, words);
		return R_MISMATCH;
	}
	return R_PASS;
}

/* Run the job once, synchronously; returns its duration in *ms. */
static enum result job_run(struct job *j, uint32_t n, struct report *r, const char *what,
			   double *ms)
{
	VkCommandBuffer cb;
	double t0 = now_ms();

	CHECK(job_record(j, n, &cb));
	CHECK(cmd_submit_wait(j->v, cb));
	*ms = now_ms() - t0;
	return job_check(j, n, r, what);
}

static enum result prio(struct vk *v, const struct opts *o, struct report *r, enum kind k)
{
	static const char *const kn[] = { "3D", "TA", "CDM" };
	struct opts ol = *o, oh = *o;
	struct vk lo, hi;
	struct job jl, jh;
	enum result res;
	uint32_t seed = OPT(o->seed, 1);
	/* start amount and growth limit of the calibration */
	uint32_t n = k == K_TA ? 2000 : k == K_3D ? 64 : 256;
	uint32_t n_max = k == K_TA ? 20000000u : 100000000u;
	uint32_t probe_n = k == K_TA ? 2 : 1;
	double t = 0, alone = 0, sum = 0, worst = 0;

	(void)v;                /* two devices of their own */
	ol.priority = PRIO_LOW;
	oh.priority = PRIO_HIGH;
	CHECK(vk_init(&lo, &ol));
	res = vk_init(&hi, &oh);
	if (res != R_PASS) {
		vk_fini(&lo);
		return res;
	}
#define TRY(x)                                  \
	do {                                    \
		res = (x);                      \
		if (res != R_PASS)              \
			goto out;               \
	} while (0)
	TRY(job_setup(&jl, &lo, k, seed));
	TRY(job_setup(&jh, &hi, k, seed + 1));

	/* calibrate the low-priority job to about TARGET_MS */
	for (;;) {
		TRY(job_run(&jl, n, r, "calibration", &t));
		LOG(v, 1, "vkfwtest: %s calibrate: n=%" PRIu32 " -> %.1f ms\n", kn[k], n, t);
		if (t > 150.0 || n >= n_max / 4)
			break;
		n *= 4;
	}
	{
		double scaled = (double)n * TARGET_MS / (t > 1.0 ? t : 1.0);

		n = scaled > n_max ? n_max : scaled < 1 ? 1 : (uint32_t)scaled;
	}
	for (uint32_t i = 0; i < 3; i++) {
		double ms;

		TRY(job_run(&jh, probe_n, r, "probe alone", &ms));
		alone += ms / 3;
	}

	/* the long job, and the probes while it runs */
	VkCommandBuffer cb;
	VkFence fl;
	double t0;

	TRY(job_record(&jl, n, &cb));
	TRY(cmd_submit_async(&lo, cb, &fl));
	t0 = now_ms();
	sleep_ms(50);
	for (uint32_t i = 0; i < PROBES; i++) {
		double ms;

		TRY(job_run(&jh, probe_n, r, "probe", &ms));
		sum += ms;
		if (ms > worst)
			worst = ms;
		r->frames++;
		sleep_ms(100);
	}
	bool running = vkGetFenceStatus(lo.dev, fl) == VK_NOT_READY;

	TRY(fence_wait(&lo, fl, lo.timeout_ns));
	double long_ms = now_ms() - t0;

	TRY(job_check(&jl, n, r, "long job"));
	r->frames++;
	/*
	 * The probes overtook the long job if they were not held up by it: the
	 * worst latency stays close to the latency on an idle GPU.
	 */
	bool overtook = worst <= alone * 10.0 + 20.0;
	snprintf(r->detail, sizeof(r->detail),
		 "%s: low job %.2f s (n=%" PRIu32 "); probe alone %.1f ms, during: avg %.1f worst %.1f"
		 " ms; probes %s%s",
		 kn[k], long_ms / 1e3, n, alone, sum / PROBES, worst,
		 overtook ? "not delayed (preempted or concurrent)" : "delayed (no preemption)",
		 running ? "" : ", low job ended first");
	if (o->require_preempt && !overtook)
		res = R_MISMATCH;
out:
	vk_fini(&hi);
	vk_fini(&lo);
	return res;
#undef TRY
}

enum result test_prio_3d(struct vk *v, const struct opts *o, struct report *r)
{
	return prio(v, o, r, K_3D);
}

enum result test_prio_ta(struct vk *v, const struct opts *o, struct report *r)
{
	return prio(v, o, r, K_TA);
}

enum result test_prio_cdm(struct vk *v, const struct opts *o, struct report *r)
{
	return prio(v, o, r, K_CDM);
}
