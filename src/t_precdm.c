// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * pre-cdm: a compute dispatch (local size 64) writes buf[id] = lcg(id ^ seed,
 * iters); every word is checked on the CPU, frame after frame.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fill.comp.h"

struct fill_pc {
	uint32_t seed, iters;
};

static uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

enum result test_precdm(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t groups = OPT(o->groups, 64), frames = OPT(o->frames, 4);
	uint32_t seed = OPT(o->seed, 1), iters = OPT(o->iters, 16);
	uint32_t n = groups * 64;
	struct buf b;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipe;

	CHECK(buf_create(v, &b, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	CHECK(compute_pipeline(v, spv_fill_comp, sizeof(spv_fill_comp), sizeof(struct fill_pc), &b,
			       &layout, &pipe, &set));

	for (uint32_t f = 0; f < frames; f++) {
		struct fill_pc pc = { seed + f * 31u, iters };
		VkBufferMemoryBarrier hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = b.b,
			.size = VK_WHOLE_SIZE,
		};
		VkCommandBuffer cb;
		uint64_t bad = 0;

		memset(b.map, 0, (size_t)n * 4);
		CHECK(cmd_begin(v, &cb));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
					nullptr);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cb, groups, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		/* the device writes little-endian words (SPIR-V/Vulkan uint in memory) */
		for (uint32_t i = 0; i < n; i++) {
			uint32_t got = rd32le(b.map + (size_t)i * 4);
			uint32_t want = lcg(i ^ pc.seed, iters);

			if (got == want)
				continue;
			if (bad < 8 && v->verbose >= 0)
				fprintf(stderr, "vkfwtest: dispatch %u: word %u got %08x expected %08x\n",
					f, i, got, want);
			bad++;
		}
		r->checked += n;
		r->mismatches += bad;
		if (bad) {
			snprintf(r->detail, sizeof(r->detail),
				 "dispatch %u: %" PRIu64 " of %u words wrong", f, bad, n);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail), "%u groups x 64, %u LCG iterations, %u dispatches",
		 groups, iters, frames);
	return R_PASS;
}
