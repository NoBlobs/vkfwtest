// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * memory:
 *  1. a large device-local allocation (up to 64 MiB, capped at 1/8 of the
 *     largest device-local heap): fill and verify first, middle and last pages;
 *  2. many small allocations (up to 256 x 4 KiB): write, verify, free;
 *  3. map / write / unmap / map / verify in a loop (200 iterations).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkfwtest.h"

static int find_mem(const struct vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (v->mem.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if (bits & (1u << i))
			return (int)i;
	return -1;
}

enum result test_memory(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);

	/* 1. Large device-local allocation */
	VkDeviceSize max_heap = 0;

	for (uint32_t i = 0; i < v->mem.memoryHeapCount; i++)
		if ((v->mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
		    v->mem.memoryHeaps[i].size > max_heap)
			max_heap = v->mem.memoryHeaps[i].size;
	VkDeviceSize big_sz = max_heap / 8;

	if (big_sz > 64ull * 1024 * 1024)
		big_sz = 64ull * 1024 * 1024;
	big_sz &= ~(VkDeviceSize)4095;
	if (big_sz < 16384)
		big_sz = 16384;

	{
		VkBuffer big;
		VkDeviceMemory big_mem;
		VkBufferCreateInfo bci = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = big_sz,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VkMemoryRequirements req;

		VK_CHECK(v, vkCreateBuffer(v->dev, &bci, nullptr, &big));
		vkGetBufferMemoryRequirements(v->dev, big, &req);
		int mt = find_mem(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		if (mt < 0) {
			vkDestroyBuffer(v->dev, big, nullptr);
			return R_SETUP;
		}
		VkMemoryAllocateInfo mai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = (uint32_t)mt,
		};
		enum result ar = VK_TRY(v, vkAllocateMemory(v->dev, &mai, nullptr, &big_mem));

		if (ar != R_PASS) {
			vkDestroyBuffer(v->dev, big, nullptr);
			return ar;
		}
		ar = VK_TRY(v, vkBindBufferMemory(v->dev, big, big_mem, 0));
		if (ar != R_PASS) {
			vkDestroyBuffer(v->dev, big, nullptr);
			vkFreeMemory(v->dev, big_mem, nullptr);
			return ar;
		}

		struct buf rb;
		VkCommandBuffer cb;
		uint32_t p0 = lcg(seed, 2), p1 = lcg(seed, 3), p2 = lcg(seed, 4);
		VkDeviceSize mid_off = (big_sz / 2) & ~(VkDeviceSize)4095;
		VkDeviceSize end_off = big_sz - 4096;

		ar = buf_create(v, &rb, 3 * 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
		if (ar == R_PASS)
			ar = cmd_begin(v, &cb);
		if (ar == R_PASS) {
			VkMemoryBarrier t2t = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			};
			VkBufferCopy cpy[3] = {
				{ 0, 0, 4096 },
				{ mid_off, 4096, 4096 },
				{ end_off, 8192, 4096 },
			};
			VkBufferMemoryBarrier hb = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = rb.b,
				.size = VK_WHOLE_SIZE,
			};
			vkCmdFillBuffer(cb, big, 0, 4096, p0);
			vkCmdFillBuffer(cb, big, mid_off, 4096, p1);
			vkCmdFillBuffer(cb, big, end_off, 4096, p2);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &t2t, 0, nullptr,
					     0, nullptr);
			vkCmdCopyBuffer(cb, big, rb.b, 3, cpy);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
					     nullptr);
			ar = cmd_submit_wait(v, cb);
		}
		vkDestroyBuffer(v->dev, big, nullptr);
		vkFreeMemory(v->dev, big_mem, nullptr);
		CHECK(ar);
		r->frames++;

		uint32_t pats[3] = { p0, p1, p2 };

		for (uint32_t pg = 0; pg < 3; pg++)
			for (uint32_t w = 0; w < 1024; w++) {
				uint32_t got;

				memcpy(&got, rb.map + pg * 4096 + w * 4, 4);
				r->checked++;
				if (got != pats[pg]) {
					r->mismatches++;
					snprintf(r->detail, sizeof(r->detail),
						 "large alloc page %u word %u: got 0x%x exp 0x%x",
						 pg, w, got, pats[pg]);
					return R_MISMATCH;
				}
			}
	}

	/* 2. Many small host-visible allocations (explicitly freed) */
	uint32_t nsmall = 256;

	if (v->props.limits.maxMemoryAllocationCount / 4 < nsmall)
		nsmall = v->props.limits.maxMemoryAllocationCount / 4;
	if (nsmall < 16)
		nsmall = 16;

	{
		VkBuffer *bufs = calloc(nsmall, sizeof(*bufs));
		VkDeviceMemory *mems = calloc(nsmall, sizeof(*mems));
		uint8_t **maps = calloc(nsmall, sizeof(*maps));
		enum result res = R_PASS;
		uint32_t made = 0;

		if (!bufs || !mems || !maps) {
			free(bufs); free(mems); free(maps);
			return R_SETUP;
		}
		VkBufferCreateInfo bci = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = 4096,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		for (uint32_t i = 0; i < nsmall; i++) {
			VkMemoryRequirements req;
			void *p;

			res = VK_TRY(v, vkCreateBuffer(v->dev, &bci, nullptr, &bufs[i]));
			if (res != R_PASS)
				break;
			vkGetBufferMemoryRequirements(v->dev, bufs[i], &req);
			int mt = find_mem(v, req.memoryTypeBits,
					  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (mt < 0) {
				vkDestroyBuffer(v->dev, bufs[i], nullptr);
				res = R_SETUP;
				break;
			}
			VkMemoryAllocateInfo mai = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = req.size,
				.memoryTypeIndex = (uint32_t)mt,
			};
			res = VK_TRY(v, vkAllocateMemory(v->dev, &mai, nullptr, &mems[i]));
			if (res != R_PASS) {
				vkDestroyBuffer(v->dev, bufs[i], nullptr);
				break;
			}
			made = i + 1;
			res = VK_TRY(v, vkBindBufferMemory(v->dev, bufs[i], mems[i], 0));
			if (res != R_PASS)
				break;
			res = VK_TRY(v, vkMapMemory(v->dev, mems[i], 0, 4096, 0, &p));
			if (res != R_PASS)
				break;
			maps[i] = p;
			uint32_t tag = 0xBEEF0000u + i + seed;

			memcpy(maps[i], &tag, 4);
			memcpy(maps[i] + 4092, &tag, 4);
		}
		if (res == R_PASS) {
			for (uint32_t i = 0; i < made; i++) {
				uint32_t tag = 0xBEEF0000u + i + seed, g0, g1;

				memcpy(&g0, maps[i], 4);
				memcpy(&g1, maps[i] + 4092, 4);
				r->checked += 2;
				if (g0 != tag || g1 != tag) {
					r->mismatches++;
					snprintf(r->detail, sizeof(r->detail),
						 "small alloc %u: got 0x%x/0x%x exp 0x%x",
						 i, g0, g1, tag);
					res = R_MISMATCH;
					break;
				}
			}
		}
		for (uint32_t i = 0; i < made; i++) {
			if (maps[i])
				vkUnmapMemory(v->dev, mems[i]);
			vkDestroyBuffer(v->dev, bufs[i], nullptr);
			vkFreeMemory(v->dev, mems[i], nullptr);
		}
		free(bufs); free(mems); free(maps);
		CHECK(res);
	}

	/* 3. Map / write / unmap / map / verify in a loop (200 iterations) */
	{
		VkBuffer b;
		VkDeviceMemory m;
		VkBufferCreateInfo bci = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = 4096,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VkMemoryRequirements req;

		VK_CHECK(v, vkCreateBuffer(v->dev, &bci, nullptr, &b));
		vkGetBufferMemoryRequirements(v->dev, b, &req);
		int mt = find_mem(v, req.memoryTypeBits,
				  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mt < 0) {
			vkDestroyBuffer(v->dev, b, nullptr);
			return R_SETUP;
		}
		VkMemoryAllocateInfo mai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = (uint32_t)mt,
		};
		enum result res = VK_TRY(v, vkAllocateMemory(v->dev, &mai, nullptr, &m));

		if (res != R_PASS) {
			vkDestroyBuffer(v->dev, b, nullptr);
			return res;
		}
		res = VK_TRY(v, vkBindBufferMemory(v->dev, b, m, 0));
		for (uint32_t it = 0; res == R_PASS && it < 200; it++) {
			void *p;
			uint32_t val = 0x12340000u + it + seed, got;

			res = VK_TRY(v, vkMapMemory(v->dev, m, 0, 4096, 0, &p));
			if (res != R_PASS)
				break;
			memcpy(p, &val, 4);
			vkUnmapMemory(v->dev, m);
			res = VK_TRY(v, vkMapMemory(v->dev, m, 0, 4096, 0, &p));
			if (res != R_PASS)
				break;
			memcpy(&got, p, 4);
			vkUnmapMemory(v->dev, m);
			r->checked++;
			if (got != val) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "map/unmap iter %u: got 0x%x exp 0x%x", it, got, val);
				res = R_MISMATCH;
			}
		}
		vkDestroyBuffer(v->dev, b, nullptr);
		vkFreeMemory(v->dev, m, nullptr);
		CHECK(res);
	}

	snprintf(r->detail, sizeof(r->detail),
		 "%.1f MiB alloc, %u small allocs, 200 map/unmap cycles",
		 (double)big_sz / (1024.0 * 1024.0), nsmall);
	return R_PASS;
}
