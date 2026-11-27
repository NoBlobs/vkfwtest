// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * sync:
 *  1. binary semaphores chaining 3 submissions (across 2 queues when available);
 *  2. timeline semaphores (Vulkan 1.2) signalled/waited across GPU and host;
 *  3. events (host-set vkSetEvent + GPU vkCmdSetEvent / vkCmdWaitEvents);
 *  4. 500 small fence-synchronised submissions in a tight loop.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vkfwtest.h"

constexpr uint32_t WORDS = 64;
constexpr uint32_t SZ = WORDS * 4;

static enum result check_words(struct report *r, const char *what, const uint8_t *buf, uint32_t n,
			       uint32_t exp)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t got;

		memcpy(&got, buf + i * 4, 4);
		r->checked++;
		if (got != exp) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "%s[%u]: got 0x%08x expected 0x%08x",
				 what, i, got, exp);
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

enum result test_sync(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	struct buf ba, bb, bc;

	CHECK(buf_create(v, &ba, SZ,
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
	CHECK(buf_create(v, &bb, SZ,
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
	CHECK(buf_create(v, &bc, SZ,
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	VkBufferCopy full = { 0, 0, SZ };
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = bc.b,
		.size = VK_WHOLE_SIZE,
	};

	/* 1. Binary semaphores chaining 3 submissions (2 queues if available) */
	{
		uint32_t pat = lcg(seed, 2);
		VkSemaphore s1, s2;
		VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkCommandBuffer cb[3];

		VK_CHECK(v, vkCreateSemaphore(v->dev, &sci, nullptr, &s1));
		vk_track(v, OBJ_SEMAPHORE, H2U(s1));
		VK_CHECK(v, vkCreateSemaphore(v->dev, &sci, nullptr, &s2));
		vk_track(v, OBJ_SEMAPHORE, H2U(s2));

		CHECK(cmd_begin(v, &cb[0]));
		vkCmdFillBuffer(cb[0], ba.b, 0, SZ, pat);
		VK_CHECK(v, vkEndCommandBuffer(cb[0]));

		CHECK(cmd_begin(v, &cb[1]));
		vkCmdCopyBuffer(cb[1], ba.b, bb.b, 1, &full);
		VK_CHECK(v, vkEndCommandBuffer(cb[1]));

		CHECK(cmd_begin(v, &cb[2]));
		vkCmdCopyBuffer(cb[2], bb.b, bc.b, 1, &full);
		vkCmdPipelineBarrier(cb[2], VK_PIPELINE_STAGE_TRANSFER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		VK_CHECK(v, vkEndCommandBuffer(cb[2]));

		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si0 = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1, .pCommandBuffers = &cb[0],
			.signalSemaphoreCount = 1, .pSignalSemaphores = &s1,
		};
		VkSubmitInfo si1 = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &s1, .pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb[1],
			.signalSemaphoreCount = 1, .pSignalSemaphores = &s2,
		};
		VkSubmitInfo si2 = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &s2, .pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb[2],
		};
		VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
		VK_CHECK(v, vkQueueSubmit(v->queues[0], 1, &si0, VK_NULL_HANDLE));
		VK_CHECK(v, vkQueueSubmit(v->nqueues >= 2 ? v->queues[1] : v->queues[0], 1, &si1,
					  VK_NULL_HANDLE));
		VK_CHECK(v, vkQueueSubmit(v->queues[0], 1, &si2, v->fence));
		CHECK(VK_TRY(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, v->timeout_ns)));
		vkFreeCommandBuffers(v->dev, v->pool, 3, cb);
		r->frames++;
		CHECK(check_words(r, "binary semaphores", bc.map, WORDS, pat));
	}

	/* 2. Timeline semaphores (host -> GPU -> GPU -> host) */
	if (v->has_timeline) {
		uint32_t pat = lcg(seed, 5);
		VkSemaphoreTypeCreateInfo tci = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue = 0,
		};
		VkSemaphoreCreateInfo sci = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &tci,
		};
		VkSemaphore tsem;
		VkCommandBuffer cb[2];

		VK_CHECK(v, vkCreateSemaphore(v->dev, &sci, nullptr, &tsem));
		vk_track(v, OBJ_SEMAPHORE, H2U(tsem));

		CHECK(cmd_begin(v, &cb[0]));
		vkCmdFillBuffer(cb[0], ba.b, 0, SZ, pat);
		VK_CHECK(v, vkEndCommandBuffer(cb[0]));

		CHECK(cmd_begin(v, &cb[1]));
		vkCmdCopyBuffer(cb[1], ba.b, bc.b, 1, &full);
		vkCmdPipelineBarrier(cb[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		VK_CHECK(v, vkEndCommandBuffer(cb[1]));

		uint64_t v1 = 1, v2 = 2, v3 = 3;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkTimelineSemaphoreSubmitInfo ts0 = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &v1,
			.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v2,
		};
		VkSubmitInfo si0 = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts0,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &tsem, .pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb[0],
			.signalSemaphoreCount = 1, .pSignalSemaphores = &tsem,
		};
		VkTimelineSemaphoreSubmitInfo ts1 = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &v2,
			.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v3,
		};
		VkSubmitInfo si1 = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts1,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &tsem, .pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb[1],
			.signalSemaphoreCount = 1, .pSignalSemaphores = &tsem,
		};
		VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si0, VK_NULL_HANDLE));
		VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si1, VK_NULL_HANDLE));

		VkSemaphoreSignalInfo sig = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
			.semaphore = tsem,
			.value = 1,
		};
		VK_CHECK(v, vkSignalSemaphore(v->dev, &sig));
		VkSemaphoreWaitInfo wi = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &tsem,
			.pValues = &v3,
		};
		CHECK(VK_TRY(v, vkWaitSemaphores(v->dev, &wi, v->timeout_ns)));
		vkFreeCommandBuffers(v->dev, v->pool, 2, cb);
		r->frames++;
		CHECK(check_words(r, "timeline semaphore", bc.map, WORDS, pat));
	}

	/* 3. Events: host vkSetEvent + GPU vkCmdSetEvent / vkCmdWaitEvents */
	{
		uint32_t pat = lcg(seed, 9);
		VkEvent ev_host, ev_gpu;
		VkEventCreateInfo eci = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
		VkCommandBuffer cb;

		VK_CHECK(v, vkCreateEvent(v->dev, &eci, nullptr, &ev_host));
		vk_track(v, OBJ_EVENT, H2U(ev_host));
		VK_CHECK(v, vkCreateEvent(v->dev, &eci, nullptr, &ev_gpu));
		vk_track(v, OBJ_EVENT, H2U(ev_gpu));
		VK_CHECK(v, vkSetEvent(v->dev, ev_host));

		VkBufferMemoryBarrier ba_bar = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = ba.b,
			.size = VK_WHOLE_SIZE,
		};
		CHECK(cmd_begin(v, &cb));
		vkCmdWaitEvents(cb, 1, &ev_host, VK_PIPELINE_STAGE_HOST_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
		vkCmdFillBuffer(cb, ba.b, 0, SZ, pat);
		vkCmdSetEvent(cb, ev_gpu, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdWaitEvents(cb, 1, &ev_gpu, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 1, &ba_bar, 0, nullptr);
		vkCmdCopyBuffer(cb, ba.b, bc.b, 1, &full);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		CHECK(check_words(r, "events", bc.map, WORDS, pat));
	}

	/* 4. 500 small submissions in a tight loop (reusing one command buffer) */
	{
		uint32_t nsub = OPT(o->frames, 500);
		VkCommandBuffer cb;
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
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cb,
		};

		VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &cai, &cb));
		for (uint32_t i = 0; i < nsub; i++) {
			uint32_t val = 0x70000u + i;

			VK_CHECK(v, vkResetCommandBuffer(cb, 0));
			VK_CHECK(v, vkBeginCommandBuffer(cb, &bi));
			vkCmdFillBuffer(cb, bc.b, 0, 4, val);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
					     nullptr);
			VK_CHECK(v, vkEndCommandBuffer(cb));
			VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
			VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si, v->fence));
			enum result wr = VK_TRY(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE,
								   v->timeout_ns));
			if (wr != R_PASS) {
				if (wr != R_TIMEOUT)
					vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
				return wr;
			}
			uint32_t got;

			memcpy(&got, bc.map, 4);
			r->checked++;
			if (got != val) {
				r->mismatches++;
				vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
				snprintf(r->detail, sizeof(r->detail),
					 "small submit %u: got 0x%x expected 0x%x", i, got, val);
				return R_MISMATCH;
			}
		}
		vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
		r->frames += nsub;
	}

	/* 5. External fence (sync_file export + import) */
	bool did_ext = false;

	if (v->has_ext_fence_fd) {
		PFN_vkGetFenceFdKHR get_fd =
			(PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(v->dev, "vkGetFenceFdKHR");
		PFN_vkImportFenceFdKHR imp_fd =
			(PFN_vkImportFenceFdKHR)vkGetDeviceProcAddr(v->dev, "vkImportFenceFdKHR");
		if (get_fd && imp_fd) {
			uint32_t pat = lcg(seed, 13);
			VkExportFenceCreateInfo efci = {
				.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
				.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
			};
			VkFenceCreateInfo fci_exp = {
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				.pNext = &efci,
			};
			VkFenceCreateInfo fci_imp = {
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			};
			VkFence f_exp, f_imp;
			VkCommandBuffer cb;
			int fd = -1;

			VK_CHECK(v, vkCreateFence(v->dev, &fci_exp, nullptr, &f_exp));
			VK_CHECK(v, vkCreateFence(v->dev, &fci_imp, nullptr, &f_imp));
			CHECK(cmd_begin(v, &cb));
			vkCmdFillBuffer(cb, bc.b, 0, SZ, pat);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
					     nullptr);
			VK_CHECK(v, vkEndCommandBuffer(cb));
			VkSubmitInfo si = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &cb,
			};
			enum result er = VK_TRY(v, vkQueueSubmit(v->queue, 1, &si, f_exp));

			if (er == R_PASS) {
				VkFenceGetFdInfoKHR gfi = {
					.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
					.fence = f_exp,
					.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
				};
				er = VK_TRY(v, get_fd(v->dev, &gfi, &fd));
			}
			if (er == R_PASS) {
				VkImportFenceFdInfoKHR ifi = {
					.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
					.fence = f_imp,
					.flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
					.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
					.fd = fd,
				};
				er = VK_TRY(v, imp_fd(v->dev, &ifi));
				if (er == R_PASS)
					fd = -1; /* ownership transferred on success */
			}
			if (fd >= 0)
				close(fd);
			if (er == R_PASS)
				er = VK_TRY(v, vkWaitForFences(v->dev, 1, &f_imp, VK_TRUE,
							       v->timeout_ns));
			vkDestroyFence(v->dev, f_imp, nullptr);
			vkDestroyFence(v->dev, f_exp, nullptr);
			if (er == R_PASS)
				vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
			CHECK(er);
			r->frames++;
			CHECK(check_words(r, "external fence sync_file", bc.map, WORDS, pat));
			did_ext = true;
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "binary sem (%u queue%s)%s, events, %u small submits%s",
		 v->nqueues, v->nqueues > 1 ? "s" : "",
		 v->has_timeline ? ", timeline sem" : "", OPT(o->frames, 500),
		 did_ext ? ", sync_file fence" : "");
	return R_PASS;
}
