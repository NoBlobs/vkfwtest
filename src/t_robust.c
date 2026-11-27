// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * robust: robustBufferAccess. Creates a device with robustBufferAccess enabled
 * and dispatches robust.comp on a 16-word buffer with an out-of-bounds index
 * (1,000,000). Verifies:
 *  - no fault / device lost;
 *  - the in-bounds read succeeded (data[0] == data[3]);
 *  - the out-of-bounds read returned 0 or an in-bounds value from the buffer;
 *  - the out-of-bounds write did not corrupt any in-bounds word.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "robust.comp.h"

enum result test_robust(struct vk *v, const struct opts *o, struct report *r)
{
	VkPhysicalDeviceFeatures have;

	vkGetPhysicalDeviceFeatures(v->phys, &have);
	if (!have.robustBufferAccess) {
		snprintf(r->detail, sizeof(r->detail), "robustBufferAccess not supported");
		return R_SKIP;
	}

	struct opts o2 = *o;
	struct vk v2;

	o2.robust = true;
	CHECK(vk_init(&v2, &o2));

	struct buf b;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSet set;
	VkCommandBuffer cb;
	uint32_t far_idx = 1000000u;
	enum result res = buf_create(&v2, &b, 16 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

	if (res == R_PASS) {
		for (uint32_t i = 0; i < 16; i++) {
			uint32_t val = 0x1000u + i;

			memcpy(b.map + i * 4, &val, 4);
		}
		res = compute_pipeline(&v2, spv_robust_comp, sizeof(spv_robust_comp),
				       sizeof(far_idx), &b, &layout, &pipe, &set);
	}
	if (res == R_PASS)
		res = cmd_begin(&v2, &cb);
	if (res == R_PASS) {
		VkBufferMemoryBarrier hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = b.b,
			.size = VK_WHOLE_SIZE,
		};
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
					nullptr);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(far_idx),
				   &far_idx);
		vkCmdDispatch(cb, 1, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		res = cmd_submit_wait(&v2, cb);
	}
	if (res == R_PASS) {
		uint32_t d[16];

		memcpy(d, b.map, sizeof(d));
		r->frames++;
		r->checked += 16;
		if (d[0] != 0x1003u) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "in-bounds read: got 0x%x expected 0x1003", d[0]);
			res = R_MISMATCH;
		} else if (d[1] != 0u && (d[1] < 0x1000u || d[1] > 0x100Fu)) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "OOB read returned unexpected value 0x%x", d[1]);
			res = R_MISMATCH;
		} else {
			for (uint32_t i = 2; i < 16; i++)
				if (d[i] != 0x1000u + i) {
					r->mismatches++;
					snprintf(r->detail, sizeof(r->detail),
						 "word[%u] corrupted: got 0x%x exp 0x%x",
						 i, d[i], 0x1000u + i);
					res = R_MISMATCH;
					break;
				}
		}
		if (res == R_PASS)
			snprintf(r->detail, sizeof(r->detail),
				 "OOB read[%u] -> 0x%x, OOB write discarded, no fault",
				 far_idx, d[1]);
	}
	vk_fini(&v2);
	return res;
}
