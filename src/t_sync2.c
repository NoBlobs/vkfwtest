// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * sync2: VK_KHR_synchronization2 (core 1.3).
 *  submission 1 (vkQueueSubmit2): fill A with X; vkCmdPipelineBarrier2;
 *                fill the first half of A with Y; signal a semaphore;
 *  submission 2 (vkQueueSubmit2): wait for it (VkSemaphoreSubmitInfo, at the
 *                transfer stage); copy A to B; barrier2 to the host.
 * B must be Y in its first half and X in the second: both orderings hold.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

constexpr uint32_t WORDS = 4096;

enum result test_sync2(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t frames = OPT(o->frames, 4), seed = OPT(o->seed, 1);

	if (!v->has_sync2) {
		snprintf(r->detail, sizeof(r->detail),
			 "needs synchronization2 (Vulkan 1.3 or VK_KHR_synchronization2)");
		return R_SKIP;
	}
	bool core = v->api >= VK_API_VERSION_1_3;
	PFN_vkQueueSubmit2 submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(
		v->dev, core ? "vkQueueSubmit2" : "vkQueueSubmit2KHR");
	PFN_vkCmdPipelineBarrier2 barrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(
		v->dev, core ? "vkCmdPipelineBarrier2" : "vkCmdPipelineBarrier2KHR");

	if (!submit2 || !barrier2) {
		snprintf(r->detail, sizeof(r->detail), "synchronization2 entry points missing");
		return R_SKIP;
	}

	struct buf a, b;
	VkSemaphore sem;
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

	CHECK(buf_create(v, &a, WORDS * 4,
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false));
	CHECK(buf_create(v, &b, WORDS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	VK_CHECK(v, vkCreateSemaphore(v->dev, &sci, nullptr, &sem));
	vk_track(v, OBJ_SEMAPHORE, H2U(sem));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t x = lcg(seed + f, 3), y = lcg(seed + f, 7);
		VkCommandBuffer cb1, cb2;
		VkCommandBufferAllocateInfo cai = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = v->pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBufferBeginInfo bi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		VkBufferMemoryBarrier2 bb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = a.b,
			.size = VK_WHOLE_SIZE,
		};
		VkDependencyInfo di = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.bufferMemoryBarrierCount = 1,
			.pBufferMemoryBarriers = &bb,
		};

		VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &cai, &cb1));
		VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &cai, &cb2));
		VK_CHECK(v, vkBeginCommandBuffer(cb1, &bi));
		vkCmdFillBuffer(cb1, a.b, 0, VK_WHOLE_SIZE, x);
		barrier2(cb1, &di);
		vkCmdFillBuffer(cb1, a.b, 0, WORDS * 2, y);
		VK_CHECK(v, vkEndCommandBuffer(cb1));

		VkBufferCopy cp = { 0, 0, WORDS * 4 };
		VkBufferMemoryBarrier2 hb = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
			.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = b.b,
			.size = VK_WHOLE_SIZE,
		};
		VkDependencyInfo hdi = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.bufferMemoryBarrierCount = 1,
			.pBufferMemoryBarriers = &hb,
		};

		VK_CHECK(v, vkBeginCommandBuffer(cb2, &bi));
		vkCmdCopyBuffer(cb2, a.b, b.b, 1, &cp);
		barrier2(cb2, &hdi);
		VK_CHECK(v, vkEndCommandBuffer(cb2));

		VkCommandBufferSubmitInfo c1 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
						 .commandBuffer = cb1 };
		VkCommandBufferSubmitInfo c2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
						 .commandBuffer = cb2 };
		VkSemaphoreSubmitInfo sig = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					      .semaphore = sem,
					      .stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT };
		VkSemaphoreSubmitInfo wt = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					     .semaphore = sem,
					     .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT };
		VkSubmitInfo2 s[2] = {
			{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1,
			  .pCommandBufferInfos = &c1, .signalSemaphoreInfoCount = 1,
			  .pSignalSemaphoreInfos = &sig },
			{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1,
			  .pWaitSemaphoreInfos = &wt, .commandBufferInfoCount = 1,
			  .pCommandBufferInfos = &c2 },
		};

		memset(b.map, 0, WORDS * 4);
		VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
		VK_CHECK(v, submit2(v->queue, 2, s, v->fence));
		CHECK(VK_TRY(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, v->timeout_ns)));
		VkCommandBuffer both[2] = { cb1, cb2 };

		vkFreeCommandBuffers(v->dev, v->pool, 2, both);
		r->frames++;

		uint64_t bad = 0;

		for (uint32_t i = 0; i < WORDS; i++) {
			uint32_t got, want = i < WORDS / 2 ? y : x;

			memcpy(&got, b.map + (size_t)i * 4, 4);
			if (got != want) {
				if (!bad && v->verbose >= 0)
					fprintf(stderr, "vkfwtest: sync2 round %u word %u: got %08x"
						" expected %08x\n", f, i, got, want);
				bad++;
			}
		}
		r->checked += WORDS;
		r->mismatches += bad;
		if (bad) {
			snprintf(r->detail, sizeof(r->detail),
				 "round %u: %" PRIu64 " of %u words wrong", f, bad, WORDS);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "vkQueueSubmit2 + semaphore, vkCmdPipelineBarrier2 (%s), %u rounds",
		 core ? "core 1.3" : "KHR", frames);
	return R_PASS;
}
