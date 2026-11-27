// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * subgroups: Vulkan 1.1 subgroup operations in compute (size, elect, ballot,
 * arithmetic). Skips cleanly when the device does not expose compute subgroups
 * with basic + ballot + arithmetic support.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "sub.comp.h"

constexpr uint32_t WGS = 4;
constexpr uint32_t LOCAL = 64;
constexpr uint32_t N = WGS * LOCAL;

enum result test_subgroups(struct vk *v, const struct opts *o, struct report *r)
{
	(void)o;
	VkSubgroupFeatureFlags req = VK_SUBGROUP_FEATURE_BASIC_BIT |
				     VK_SUBGROUP_FEATURE_BALLOT_BIT |
				     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;

	if (v->props.apiVersion < VK_API_VERSION_1_1 ||
	    !(v->subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
	    (v->subgroup.supportedOperations & req) != req) {
		snprintf(r->detail, sizeof(r->detail),
			 "compute subgroups (basic+ballot+arithmetic) not supported");
		return R_SKIP;
	}

	struct buf out;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSet set;
	VkCommandBuffer cb;
	uint32_t ops = 3u;      /* ballot + arithmetic */

	CHECK(buf_create(v, &out, N * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(out.map, 0, N * 16);
	CHECK(compute_pipeline(v, spv_sub_comp, sizeof(spv_sub_comp), sizeof(ops), &out, &layout,
			       &pipe, &set));
	CHECK(cmd_begin(v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ops), &ops);
	vkCmdDispatch(cb, WGS, 1, 1);
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = out.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	uint32_t sg_size = 0;

	for (uint32_t wg = 0; wg < WGS; wg++) {
		uint32_t elected = 0;

		for (uint32_t l = 0; l < LOCAL; l++) {
			uint32_t i = wg * LOCAL + l;
			uint32_t got[4];

			memcpy(got, out.map + i * 16, 16);
			if (!sg_size)
				sg_size = got[0];
			uint32_t active = sg_size < LOCAL ? sg_size : LOCAL;

			r->checked += 4;
			if (!got[0] || got[0] != sg_size) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "inv %u: gl_SubgroupSize got %u expected %u", i, got[0],
					 sg_size);
				return R_MISMATCH;
			}
			if (got[1] > 1u) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "inv %u: subgroupElect got %u", i, got[1]);
				return R_MISMATCH;
			}
			elected += got[1];
			if (got[2] != active) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "inv %u: ballot count got %u expected %u", i, got[2],
					 active);
				return R_MISMATCH;
			}
			if (got[3] != active) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "inv %u: subgroupAdd(1) got %u expected %u", i, got[3],
					 active);
				return R_MISMATCH;
			}
		}
		uint32_t expect_elected = (LOCAL + sg_size - 1u) / sg_size;

		r->checked++;
		if (elected != expect_elected) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "wg %u: %u elected invocations, expected %u (subgroupSize=%u)",
				 wg, elected, expect_elected, sg_size);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "subgroupSize=%u, elect + ballot + subgroupAdd across %u invocations", sg_size, N);
	return R_PASS;
}
