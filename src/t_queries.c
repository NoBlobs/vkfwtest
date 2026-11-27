// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * queries: occlusion queries and timestamps.
 *  1. occlusion: a 40x24 rectangle (960 samples), a rectangle entirely
 *     outside the viewport (0) and one hidden behind it by the depth test (0);
 *     with occlusionQueryPrecise the count must be exact, otherwise non-zero /
 *     zero. Results are read on the host and copied by the GPU into a buffer
 *     (vkCmdCopyQueryPoolResults); both must agree.
 *  2. timestamps around a fragment-heavy draw: valid, increasing, and less
 *     than 10 s apart.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "rect.vert.h"
#include "rect.frag.h"
#include "fullscreen.vert.h"
#include "color.frag.h"

constexpr VkFormat CFMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat DFMT = VK_FORMAT_D32_SFLOAT;

struct rect_pc {
	float rect[4];
	uint32_t color;
	float z;
};

static float ndc(uint32_t px, uint32_t n)
{
	return (float)px / (float)n * 2.0f - 1.0f;
}

enum result test_queries(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = 128, h = 64, frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	bool precise = v->has_precise_occlusion;
	struct img color, depth;
	struct buf gpu_res;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs, fvs, ffs;
	VkPipelineLayout layout, l_heavy;
	VkPipeline pipe, heavy;
	VkQueryPool occ, ts = VK_NULL_HANDLE;
	VkFormatProperties fp;

	vkGetPhysicalDeviceFormatProperties(v->phys, DFMT, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "D32_SFLOAT depth not supported");
		return R_SKIP;
	}
	CHECK(img_create(v, &color, w, h, CFMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(img_create(v, &depth, w, h, DFMT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(rp_create(v, &rp, CFMT, DFMT));
	CHECK(fb_create(v, &fb, rp, &color, &depth));
	CHECK(shader_create(v, &vs, spv_rect_vert, sizeof(spv_rect_vert)));
	CHECK(shader_create(v, &fs, spv_rect_frag, sizeof(spv_rect_frag)));
	CHECK(shader_create(v, &fvs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &ffs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct rect_pc), nullptr));
	CHECK(layout_create(v, &l_heavy, VK_SHADER_STAGE_FRAGMENT_BIT, 12, nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs, .depth = true };
	CHECK(gfx_pipeline(v, &pipe, &d));
	d = (struct gfx_desc){ .rp = rp, .layout = l_heavy, .vs = fvs, .fs = ffs };
	CHECK(gfx_pipeline(v, &heavy, &d));
	CHECK(buf_create(v, &gpu_res, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	VkQueryPoolCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_OCCLUSION,
		.queryCount = 3,
	};
	VK_CHECK(v, vkCreateQueryPool(v->dev, &qci, nullptr, &occ));
	vk_track(v, OBJ_QUERY_POOL, H2U(occ));

	/* timestamps need valid bits on the queue family */
	VkQueueFamilyProperties qf[16];
	uint32_t nq = 16;

	vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
	uint32_t ts_bits = qf[v->qfam].timestampValidBits;

	/* without valid bits, vkCmdWriteTimestamp is not allowed (Vulkan spec) */
	if (ts_bits) {
		qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qci.queryCount = 2;
		VK_CHECK(v, vkCreateQueryPool(v->dev, &qci, nullptr, &ts));
		vk_track(v, OBJ_QUERY_POOL, H2U(ts));
	}

	const uint64_t want[3] = { 40 * 24, 0, 0 };

	for (uint32_t f = 0; f < frames; f++) {
		VkClearValue clear[2] = { { .color.float32 = { 0, 0, 0, 0 } },
					  { .depthStencil = { 1.0f, 0 } } };
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 2,
			.pClearValues = clear,
		};
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		/* q0: 40x24 at (10,8); a front occluder; q2: the same rectangle behind it */
		struct rect_pc visible = { { ndc(10, w), ndc(8, h), ndc(50, w), ndc(32, h) },
					   0xFF0000FFu, 0.5f };
		struct rect_pc outside = { { 1.5f, 1.5f, 2.0f, 2.0f }, 0xFF00FF00u, 0.5f };
		struct rect_pc occluder = { { ndc(60, w), ndc(8, h), ndc(100, w), ndc(32, h) },
					    0xFFFF0000u, 0.2f };
		struct rect_pc hidden = occluder;
		struct { uint32_t color, iters, seed; } hpc = { 0, 200, seed + f };
		VkQueryControlFlags qf_flags = precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
		VkCommandBuffer cb;

		hidden.z = 0.7f;
		CHECK(cmd_begin(v, &cb));
		vkCmdResetQueryPool(cb, occ, 0, 3);
		if (ts_bits)
			vkCmdResetQueryPool(cb, ts, 0, 2);
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		/* a heavy full-screen draw between two timestamps (depth test off) */
		if (ts_bits)
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts, 0);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, heavy);
		vkCmdPushConstants(cb, l_heavy, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(hpc), &hpc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		const struct rect_pc *draws[4] = { &visible, &outside, &occluder, &hidden };
		const int query[4] = { 0, 1, -1, 2 };

		for (uint32_t i = 0; i < 4; i++) {
			if (query[i] >= 0)
				vkCmdBeginQuery(cb, occ, (uint32_t)query[i], qf_flags);
			vkCmdPushConstants(cb, layout,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
					   sizeof(*draws[i]), draws[i]);
			vkCmdDraw(cb, 6, 1, 0, 0);
			if (query[i] >= 0)
				vkCmdEndQuery(cb, occ, (uint32_t)query[i]);
		}
		vkCmdEndRenderPass(cb);
		if (ts_bits)
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 1);
		vkCmdCopyQueryPoolResults(cb, occ, 0, 3, gpu_res.b, 0, 8,
					  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		VkBufferMemoryBarrier hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = gpu_res.b,
			.size = VK_WHOLE_SIZE,
		};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
				     nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		uint64_t host[3], gpu[3], t[2];

		VK_CHECK(v, vkGetQueryPoolResults(v->dev, occ, 0, 3, sizeof(host), host, 8,
						  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		memcpy(gpu, gpu_res.map, sizeof(gpu));
		for (uint32_t i = 0; i < 3; i++) {
			bool ok = precise ? host[i] == want[i] : (host[i] != 0) == (want[i] != 0);

			r->checked += 2;
			if (!ok || gpu[i] != host[i]) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "frame %u occlusion query %u: host %" PRIu64 ", GPU copy %" PRIu64
					 ", expected %s%" PRIu64, f, i, host[i], gpu[i],
					 precise ? "" : "zero/non-zero like ", want[i]);
				return R_MISMATCH;
			}
		}
		if (ts_bits) {
			VK_CHECK(v, vkGetQueryPoolResults(v->dev, ts, 0, 2, sizeof(t), t, 8,
							  VK_QUERY_RESULT_64_BIT |
								  VK_QUERY_RESULT_WAIT_BIT));
			uint64_t mask = ts_bits >= 64 ? ~0ull : (1ull << ts_bits) - 1;
			double ns = (double)((t[1] - t[0]) & mask) * v->props.limits.timestampPeriod;

			r->checked++;
			if (t[1] < t[0] || ns > 10e9) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "frame %u timestamps: %" PRIu64 " -> %" PRIu64 " (%.0f ns)", f,
					 t[0], t[1], ns);
				return R_MISMATCH;
			}
			LOG(v, 1, "vkfwtest: queries: draw took %.3f ms by timestamps\n", ns / 1e6);
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "occlusion %s (960 / 0 / 0, host + GPU copy), timestamps %s; %u frames",
		 precise ? "precise" : "non-precise", ts_bits ? "checked" : "not supported", frames);
	return R_PASS;
}
