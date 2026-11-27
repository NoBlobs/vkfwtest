// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * buffers: vkCmdFillBuffer (whole + partial), vkCmdUpdateBuffer (inline data)
 * and vkCmdCopyBuffer with multiple regions of odd sizes and unaligned offsets.
 * Every byte is verified against a CPU model.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

constexpr uint32_t SZ = 8192;

enum result test_buffers(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	struct buf dev_a, dev_b, rb;
	VkCommandBuffer cb;
	uint8_t exp_a[SZ], exp_b[SZ], upd[256];
	uint32_t fill0 = lcg(seed, 2);
	uint32_t fill1 = lcg(seed, 5);
	uint32_t fill_b = 0xA5A5A5A5u;

	for (uint32_t i = 0; i < SZ; i += 4) {
		memcpy(exp_a + i, &fill0, 4);
		memcpy(exp_b + i, &fill_b, 4);
	}
	for (uint32_t i = 1024; i < 1536; i += 4)
		memcpy(exp_a + i, &fill1, 4);
	for (uint32_t i = 0; i < sizeof(upd); i++)
		upd[i] = (uint8_t)((i * 37u + seed) & 0xFFu);
	memcpy(exp_a + 2048, upd, sizeof(upd));

	static const VkBufferCopy regions[] = {
		{ 0, 0, 1024 },
		{ 1025, 1027, 509 },    /* unaligned src/dst offsets and odd size */
		{ 2048, 2049, 255 },    /* odd dst offset and odd size */
		{ 2048, 4096, 1 },      /* single byte */
		{ 2051, 4100, 17 },     /* 17 bytes */
		{ 0, 5000, 1023 },      /* 1023 bytes */
	};
	constexpr uint32_t NREG = sizeof(regions) / sizeof(regions[0]);

	for (uint32_t i = 0; i < NREG; i++)
		memcpy(exp_b + regions[i].dstOffset, exp_a + regions[i].srcOffset,
		       (size_t)regions[i].size);

	CHECK(buf_create(v, &dev_a, SZ,
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
	CHECK(buf_create(v, &dev_b, SZ,
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
	CHECK(buf_create(v, &rb, SZ, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	VkMemoryBarrier t2t = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
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
	VkBufferCopy full = { 0, 0, SZ };

	CHECK(cmd_begin(v, &cb));
	vkCmdFillBuffer(cb, dev_a.b, 0, VK_WHOLE_SIZE, fill0);
	vkCmdFillBuffer(cb, dev_b.b, 0, VK_WHOLE_SIZE, fill_b);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 1, &t2t, 0, nullptr, 0, nullptr);
	vkCmdFillBuffer(cb, dev_a.b, 1024, 512, fill1);
	vkCmdUpdateBuffer(cb, dev_a.b, 2048, sizeof(upd), upd);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 1, &t2t, 0, nullptr, 0, nullptr);
	vkCmdCopyBuffer(cb, dev_a.b, dev_b.b, NREG, regions);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0, 1, &t2t, 0, nullptr, 0, nullptr);
	vkCmdCopyBuffer(cb, dev_b.b, rb.b, 1, &full);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	uint64_t bad = 0;

	for (uint32_t i = 0; i < SZ; i++) {
		if (rb.map[i] == exp_b[i])
			continue;
		if (bad < 8 && v->verbose >= 0)
			fprintf(stderr, "vkfwtest: buffers: byte %u got 0x%02x expected 0x%02x\n",
				i, rb.map[i], exp_b[i]);
		bad++;
	}
	r->checked += SZ;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail), "%" PRIu64 " of %u bytes wrong", bad, SZ);
		return R_MISMATCH;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "fillBuffer, updateBuffer, copyBuffer (%u regions incl. odd/1B), %u bytes",
		 NREG, SZ);
	return R_PASS;
}
