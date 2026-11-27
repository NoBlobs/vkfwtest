// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * compute-dims:
 *  1. 3D dispatch via vkCmdDispatchBase with non-zero base group (1, 1, 1) and
 *     count (2, 3, 2) on local_size (4, 2, 2); verifies both written and
 *     untouched elements;
 *  2. largest supported 1D workgroup size (specialisation constant) running a
 *     shared-memory tree reduction of 1..N across 4 workgroups.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "dims.comp.h"
#include "reduce.comp.h"

enum result test_compute_dims(struct vk *v, const struct opts *o, struct report *r)
{
	(void)o;
	/* 1. 3D dispatch with vkCmdDispatchBase */
	constexpr uint32_t GX = 12;
	constexpr uint32_t GY = 8;
	constexpr uint32_t GZ = 6;
	constexpr uint32_t NTOT = GX * GY * GZ;
	struct buf b3d;
	VkShaderModule cs3d;
	VkDescriptorSetLayout dsl3d;
	VkPipelineLayout l3d;
	VkPipeline p3d;
	VkDescriptorSet s3d;
	VkCommandBuffer cb;
	uint32_t pc[4] = { GX, GY, GZ, 0 };

	CHECK(buf_create(v, &b3d, NTOT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(b3d.map, 0, NTOT * 4);
	VkDescriptorSetLayoutBinding bind3d = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	CHECK(desc_create(v, &bind3d, 1, &dsl3d, &s3d));
	VkDescriptorBufferInfo dbi3d = { b3d.b, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr3d = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = s3d,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi3d,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr3d, 0, nullptr);
	CHECK(shader_create(v, &cs3d, spv_dims_comp, sizeof(spv_dims_comp)));
	CHECK(layout_create(v, &l3d, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &dsl3d));
	VkComputePipelineCreateInfo cci3d = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs3d, .pName = "main" },
		.layout = l3d,
	};
	VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cci3d, nullptr, &p3d));
	vk_track(v, OBJ_PIPELINE, H2U(p3d));
	CHECK(cmd_begin(v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p3d);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, l3d, 0, 1, &s3d, 0, nullptr);
	vkCmdPushConstants(cb, l3d, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vkCmdDispatchBase(cb, 1, 1, 1, 2, 3, 2);
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = b3d.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	for (uint32_t z = 0; z < GZ; z++)
		for (uint32_t y = 0; y < GY; y++)
			for (uint32_t x = 0; x < GX; x++) {
				uint32_t idx = (z * GY + y) * GX + x;
				bool in = x >= 4 && y >= 2 && z >= 2;
				uint32_t exp = in ? 0x10000u + x + y * 256u + z * 65536u : 0u;
				uint32_t got;

				memcpy(&got, b3d.map + idx * 4, 4);
				r->checked++;
				if (got != exp) {
					r->mismatches++;
					snprintf(r->detail, sizeof(r->detail),
						 "3D dispatchBase (%u,%u,%u): got 0x%x expected 0x%x",
						 x, y, z, got, exp);
					return R_MISMATCH;
				}
			}

	/* 2. largest 1D workgroup with shared-memory reduction */
	uint32_t max_n = v->props.limits.maxComputeWorkGroupSize[0];

	if (v->props.limits.maxComputeWorkGroupInvocations < max_n)
		max_n = v->props.limits.maxComputeWorkGroupInvocations;
	if (v->props.limits.maxComputeSharedMemorySize / 4 < max_n)
		max_n = v->props.limits.maxComputeSharedMemorySize / 4;
	if (max_n > 2048u)
		max_n = 2048u;  /* keep shared array sane even on huge limits */

	constexpr uint32_t WGS = 4;
	struct buf bred;
	VkShaderModule cs;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet sred;
	VkPipelineLayout lred;
	VkPipeline pred;

	CHECK(buf_create(v, &bred, WGS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(bred.map, 0, WGS * 4);
	VkDescriptorSetLayoutBinding bind = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	CHECK(desc_create(v, &bind, 1, &dsl, &sred));
	VkDescriptorBufferInfo dbi = { bred.b, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = sred,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
	CHECK(shader_create(v, &cs, spv_reduce_comp, sizeof(spv_reduce_comp)));
	CHECK(layout_create(v, &lred, 0, 0, &dsl));

	VkSpecializationMapEntry sme = { .constantID = 0, .offset = 0, .size = sizeof(max_n) };
	VkSpecializationInfo si = {
		.mapEntryCount = 1,
		.pMapEntries = &sme,
		.dataSize = sizeof(max_n),
		.pData = &max_n,
	};
	VkComputePipelineCreateInfo cci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main",
			   .pSpecializationInfo = &si },
		.layout = lred,
	};
	VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cci, nullptr, &pred));
	vk_track(v, OBJ_PIPELINE, H2U(pred));

	CHECK(cmd_begin(v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pred);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lred, 0, 1, &sred, 0, nullptr);
	vkCmdDispatch(cb, WGS, 1, 1);
	hb.buffer = bred.b;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	uint32_t expect_sum = max_n * (max_n + 1u) / 2u;

	for (uint32_t i = 0; i < WGS; i++) {
		uint32_t got;

		memcpy(&got, bred.map + i * 4, 4);
		r->checked++;
		if (got != expect_sum) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "reduce(%u) wg %u: got %u expected %u", max_n, i, got, expect_sum);
			return R_MISMATCH;
		}
	}
	snprintf(r->detail, sizeof(r->detail),
		 "3D dispatchBase (1,1,1)+(2,3,2), max workgroup %u reduction x %u", max_n, WGS);
	return R_PASS;
}
