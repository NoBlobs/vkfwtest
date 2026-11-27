// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * sparse: sparse buffer binding and residency (vkQueueBindSparse).
 * Creates a 2-page sparse buffer, binds device-local memory only to the second
 * page, fills that page, copies it to a host buffer and checks every word.
 * Skips cleanly when sparseBinding / sparseResidencyBuffer are not supported.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

enum result test_sparse(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);

	if (!v->feat.sparseBinding || !v->feat.sparseResidencyBuffer || !v->has_sparse_queue) {
		snprintf(r->detail, sizeof(r->detail), "sparse buffer residency not supported");
		return R_SKIP;
	}

	/* Probe alignment with a small sparse buffer, then create a 2-page buffer */
	VkBuffer sb;
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
			 VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
		.size = 131072,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryRequirements req;

	VK_CHECK(v, vkCreateBuffer(v->dev, &bci, nullptr, &sb));
	vk_track(v, OBJ_BUFFER, H2U(sb));
	vkGetBufferMemoryRequirements(v->dev, sb, &req);

	VkDeviceSize page = req.alignment;

	if (page * 2 > bci.size) {
		/* Recreate large enough for 2 sparse pages */
		bci.size = page * 2;
		VK_CHECK(v, vkCreateBuffer(v->dev, &bci, nullptr, &sb));
		vk_track(v, OBJ_BUFFER, H2U(sb));
		vkGetBufferMemoryRequirements(v->dev, sb, &req);
	}

	uint32_t mt = 0;

	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (v->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mt = i;
			break;
		}
	VkDeviceMemory mem;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = page,
		.memoryTypeIndex = mt,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, &mem));
	vk_track(v, OBJ_MEMORY, H2U(mem));

	/* Bind only the second page (offset = page) */
	VkSparseMemoryBind bind = {
		.resourceOffset = page,
		.size = page,
		.memory = mem,
		.memoryOffset = 0,
	};
	VkSparseBufferMemoryBindInfo bbi = {
		.buffer = sb,
		.bindCount = 1,
		.pBinds = &bind,
	};
	VkBindSparseInfo bsi = {
		.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
		.bufferBindCount = 1,
		.pBufferBinds = &bbi,
	};
	VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
	VK_CHECK(v, vkQueueBindSparse(v->queue, 1, &bsi, v->fence));
	CHECK(VK_TRY(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, v->timeout_ns)));

	struct buf rb;
	VkCommandBuffer cb;
	uint32_t pat = lcg(seed, 7);

	CHECK(buf_create(v, &rb, page, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	memset(rb.map, 0, (size_t)page);

	VkMemoryBarrier t2t = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	VkBufferCopy cpy = { .srcOffset = page, .dstOffset = 0, .size = page };
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = rb.b,
		.size = VK_WHOLE_SIZE,
	};
	CHECK(cmd_begin(v, &cb));
	vkCmdFillBuffer(cb, sb, page, page, pat);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 1, &t2t, 0, nullptr, 0, nullptr);
	vkCmdCopyBuffer(cb, sb, rb.b, 1, &cpy);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	uint32_t nwords = (uint32_t)(page / 4);

	for (uint32_t i = 0; i < nwords; i++) {
		uint32_t got;

		memcpy(&got, rb.map + i * 4, 4);
		r->checked++;
		if (got != pat) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "sparse page word %u: got 0x%08x expected 0x%08x", i, got, pat);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "sparse buffer: bound page 1 (%" PRIu64 " KiB), %u words checked",
		 (uint64_t)(page / 1024), nwords);
	return R_PASS;
}
