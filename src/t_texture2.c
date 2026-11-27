// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * texture2: advanced texture types and sampling in compute (tex2.comp):
 *  0: cube map (6 faces);
 *  1: 3D texture (4x4x4);
 *  2: 2D array texture (4 layers);
 *  3: depth-compare shadow sampler (D32_SFLOAT, COMPARE_OP_LESS);
 *  4: textureGather (red channel 2x2 footprint);
 *  5: integer texture (R32G32B32A32_UINT, texelFetch);
 *  6: float texture (R32G32B32A32_SFLOAT, exact floatBitsToUint);
 *  7: unnormalized coordinates sampler;
 *  8: ETC2 RGBA8 compressed texture (when textureCompressionETC2 is enabled).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "tex2.comp.h"
#include "tex2_extra.comp.h"

struct tex_obj {
	VkImage i;
	VkImageView view;
	VkImageAspectFlags aspect;
	uint32_t layers;
};

static enum result alloc_image(struct vk *v, const VkImageCreateInfo *ici, VkImageViewType vt,
			       VkImageAspectFlags aspect, struct tex_obj *t)
{
	VkMemoryRequirements req;
	VkDeviceMemory mem;

	memset(t, 0, sizeof(*t));
	t->aspect = aspect;
	t->layers = ici->arrayLayers;
	VK_CHECK(v, vkCreateImage(v->dev, ici, nullptr, &t->i));
	vk_track(v, OBJ_IMAGE, H2U(t->i));
	vkGetImageMemoryRequirements(v->dev, t->i, &req);
	uint32_t mt = 0;

	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (v->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mt = i;
			break;
		}
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = mt,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, &mem));
	vk_track(v, OBJ_MEMORY, H2U(mem));
	VK_CHECK(v, vkBindImageMemory(v->dev, t->i, mem, 0));
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = t->i,
		.viewType = vt,
		.format = ici->format,
		.subresourceRange = { aspect, 0, 1, 0, ici->arrayLayers },
	};
	VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &t->view));
	vk_track(v, OBJ_VIEW, H2U(t->view));
	return R_PASS;
}

static void tex_barrier(VkCommandBuffer cb, const struct tex_obj *t, VkImageLayout from,
			VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
			VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = sa,
		.dstAccessMask = da,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = t->i,
		.subresourceRange = { t->aspect, 0, 1, 0, t->layers },
	};
	vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static enum result check_uvec4(struct report *r, const char *what, const uint8_t *buf, uint32_t i,
			       const uint32_t exp[4])
{
	uint32_t got[4];

	memcpy(got, buf + i * 16, 16);
	r->checked += 4;
	if (got[0] == exp[0] && got[1] == exp[1] && got[2] == exp[2] && got[3] == exp[3])
		return R_PASS;
	r->mismatches++;
	snprintf(r->detail, sizeof(r->detail),
		 "%s[%u]: got (%u,%u,%u,%u) expected (%u,%u,%u,%u)", what, i,
		 got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3]);
	return R_MISMATCH;
}

enum result test_texture2(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	bool do_etc = v->feat.textureCompressionETC2;
	struct tex_obj t_cube, t_vol, t_arr, t_shd, t_gat, t_int, t_flt, t_unn, t_etc;
	struct buf s_cube, s_vol, s_arr, s_gat, s_int, s_flt, s_unn, s_etc, out;
	VkSampler samp_near, samp_shd, samp_unn;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkShaderModule cs;
	VkCommandBuffer cb;

	VkImageCreateInfo base = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { 4, 4, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	/* 0: Cube 4x4 x 6 */
	VkImageCreateInfo ci = base;
	ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ci.arrayLayers = 6;
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, &t_cube));
	CHECK(buf_create(v, &s_cube, 4 * 4 * 6 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	uint32_t face_col[6];

	for (uint32_t f = 0; f < 6; f++) {
		face_col[f] = (lcg(seed + f * 17u, 2) & 0x00FFFFFFu) | 0xFF000000u;
		struct rgba c = rgba_of(face_col[f]);

		for (uint32_t p = 0; p < 16; p++) {
			uint8_t *q = s_cube.map + (f * 16 + p) * 4;

			q[0] = c.r; q[1] = c.g; q[2] = c.b; q[3] = c.a;
		}
	}

	/* 1: 3D 4x4x4 */
	ci = base;
	ci.imageType = VK_IMAGE_TYPE_3D;
	ci.extent = (VkExtent3D){ 4, 4, 4 };
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT, &t_vol));
	CHECK(buf_create(v, &s_vol, 4 * 4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	for (uint32_t i = 0; i < 64; i++) {
		uint32_t x = i % 4, y = (i / 4) % 4, z = i / 16;
		uint8_t *q = s_vol.map + i * 4;

		q[0] = (uint8_t)(x * 60 + 10);
		q[1] = (uint8_t)(y * 60 + 20);
		q[2] = (uint8_t)(z * 60 + 30);
		q[3] = 255;
	}

	/* 2: 2D array 4x4 x 4 */
	ci = base;
	ci.arrayLayers = 4;
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, &t_arr));
	CHECK(buf_create(v, &s_arr, 4 * 4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	uint32_t arr_col[4];

	for (uint32_t l = 0; l < 4; l++) {
		arr_col[l] = (lcg(seed + 100u + l * 31u, 2) & 0x00FFFFFFu) | 0xFF000000u;
		struct rgba c = rgba_of(arr_col[l]);

		for (uint32_t p = 0; p < 16; p++) {
			uint8_t *q = s_arr.map + (l * 16 + p) * 4;

			q[0] = c.r; q[1] = c.g; q[2] = c.b; q[3] = c.a;
		}
	}

	/* 3: Shadow D32_SFLOAT 4x4 cleared to 0.5f */
	ci = base;
	ci.format = VK_FORMAT_D32_SFLOAT;
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, &t_shd));

	/* 4: Gather 4x4 */
	CHECK(alloc_image(v, &base, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t_gat));
	CHECK(buf_create(v, &s_gat, 4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	for (uint32_t i = 0; i < 16; i++) {
		uint8_t *q = s_gat.map + i * 4;

		q[0] = (uint8_t)(i * 16 + 7);
		q[1] = 0; q[2] = 0; q[3] = 255;
	}

	/* 5: Integer 4x4 R32G32B32A32_UINT */
	ci = base;
	ci.format = VK_FORMAT_R32G32B32A32_UINT;
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t_int));
	CHECK(buf_create(v, &s_int, 4 * 4 * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	for (uint32_t i = 0; i < 16; i++) {
		uint32_t u[4] = { 1000u + i, 2000u + i * 3u, 3000u ^ i, 0xCAFE0000u + i };

		memcpy(s_int.map + i * 16, u, 16);
	}

	/* 6: Float 4x4 R32G32B32A32_SFLOAT */
	ci = base;
	ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t_flt));
	CHECK(buf_create(v, &s_flt, 4 * 4 * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	for (uint32_t i = 0; i < 16; i++) {
		float fv[4] = { 1.5f + (float)(i % 4), -2.25f - (float)(i / 4),
				0.125f * (float)i, 42.0f };

		memcpy(s_flt.map + i * 16, fv, 16);
	}

	/* 7: Unnormalized 4x4 RGBA8 */
	CHECK(alloc_image(v, &base, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t_unn));
	CHECK(buf_create(v, &s_unn, 4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
	for (uint32_t i = 0; i < 16; i++) {
		uint8_t *q = s_unn.map + i * 4;

		q[0] = (uint8_t)(i * 15 + 5);
		q[1] = (uint8_t)(250 - i * 12);
		q[2] = (uint8_t)(i * 7 + 33);
		q[3] = 255;
	}

	/* 8: ETC2 RGBA8 4x4 (one 16-byte block -> solid (134, 68, 200, 255)) */
	if (do_etc) {
		ci = base;
		ci.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
		CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t_etc));
		CHECK(buf_create(v, &s_etc, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		static const uint8_t blk[16] = {
			0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* EAC alpha = 255 */
			0x80, 0x40, 0xC0, 0x02, 0x00, 0x00, 0x00, 0x00, /* diff RGB=(132,66,198)+2 */
		};
		memcpy(s_etc.map, blk, 16);
	} else {
		t_etc = t_gat;
	}

	/* Samplers */
	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	CHECK(sampler_create(v, &sci, &samp_near));
	VkSamplerCreateInfo sci_shd = sci;

	sci_shd.compareEnable = VK_TRUE;
	sci_shd.compareOp = VK_COMPARE_OP_LESS;
	CHECK(sampler_create(v, &sci_shd, &samp_shd));
	VkSamplerCreateInfo sci_unn = sci;

	sci_unn.unnormalizedCoordinates = VK_TRUE;
	CHECK(sampler_create(v, &sci_unn, &samp_unn));

	/* Output buffer and descriptors */
	CHECK(buf_create(v, &out, 64 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	VkDescriptorSetLayoutBinding binds[10];

	for (uint32_t i = 0; i < 9; i++)
		binds[i] = (VkDescriptorSetLayoutBinding){
			i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
		};
	binds[9] = (VkDescriptorSetLayoutBinding){
		9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
	};
	CHECK(desc_create(v, binds, 10, &dsl, &set));

	VkDescriptorImageInfo imgs[9] = {
		{ samp_near, t_cube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_vol.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_arr.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_shd,  t_shd.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_gat.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_int.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_flt.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_unn,  t_unn.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ samp_near, t_etc.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkDescriptorBufferInfo obi = { out.b, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr[10];

	for (uint32_t i = 0; i < 9; i++)
		wr[i] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set, .dstBinding = i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &imgs[i],
		};
	wr[9] = (VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set, .dstBinding = 9, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &obi,
	};
	vkUpdateDescriptorSets(v->dev, 10, wr, 0, nullptr);

	CHECK(shader_create(v, &cs, spv_tex2_comp, sizeof(spv_tex2_comp)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_COMPUTE_BIT, 8, &dsl));
	VkComputePipelineCreateInfo cci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" },
		.layout = layout,
	};
	VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cci, nullptr, &pipe));
	vk_track(v, OBJ_PIPELINE, H2U(pipe));

	/* Upload all textures in one command buffer */
	const struct tex_obj *all_t[9] = {
		&t_cube, &t_vol, &t_arr, &t_shd, &t_gat, &t_int, &t_flt, &t_unn, &t_etc,
	};
	uint32_t nt = do_etc ? 9 : 8;

	CHECK(cmd_begin(v, &cb));
	for (uint32_t i = 0; i < nt; i++)
		tex_barrier(cb, all_t[i], VK_IMAGE_LAYOUT_UNDEFINED,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy c2d = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 4, 4, 1 },
	};
	VkBufferImageCopy ccube = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 },
		.imageExtent = { 4, 4, 1 },
	};
	VkBufferImageCopy c3d = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 4, 4, 4 },
	};
	VkBufferImageCopy carr = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 4 },
		.imageExtent = { 4, 4, 1 },
	};

	vkCmdCopyBufferToImage(cb, s_cube.b, t_cube.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ccube);
	vkCmdCopyBufferToImage(cb, s_vol.b,  t_vol.i,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c3d);
	vkCmdCopyBufferToImage(cb, s_arr.b,  t_arr.i,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &carr);
	VkClearDepthStencilValue dclr = { .depth = 0.5f };
	VkImageSubresourceRange drng = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

	vkCmdClearDepthStencilImage(cb, t_shd.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dclr, 1, &drng);
	vkCmdCopyBufferToImage(cb, s_gat.b, t_gat.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c2d);
	vkCmdCopyBufferToImage(cb, s_int.b, t_int.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c2d);
	vkCmdCopyBufferToImage(cb, s_flt.b, t_flt.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c2d);
	vkCmdCopyBufferToImage(cb, s_unn.b, t_unn.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c2d);
	if (do_etc)
		vkCmdCopyBufferToImage(cb, s_etc.b, t_etc.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				       1, &c2d);
	for (uint32_t i = 0; i < nt; i++)
		tex_barrier(cb, all_t[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	CHECK(cmd_submit_wait(v, cb));

	static const uint32_t counts[9] = { 6, 64, 4, 2, 1, 16, 16, 16, 16 };
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = out.b,
		.size = VK_WHOLE_SIZE,
	};

	for (uint32_t mode = 0; mode < nt; mode++) {
		uint32_t pc[2] = { mode, counts[mode] };

		memset(out.map, 0, counts[mode] * 16);
		CHECK(cmd_begin(v, &cb));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
					nullptr);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
		vkCmdDispatch(cb, 1, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		for (uint32_t i = 0; i < counts[mode]; i++) {
			uint32_t exp[4] = { 0 };

			switch (mode) {
			case 0: {
				struct rgba c = rgba_of(face_col[i]);
				exp[0] = c.r; exp[1] = c.g; exp[2] = c.b; exp[3] = c.a;
				CHECK(check_uvec4(r, "cube", out.map, i, exp));
				break;
			}
			case 1: {
				uint32_t x = i % 4, y = (i / 4) % 4, z = i / 16;
				exp[0] = x * 60 + 10; exp[1] = y * 60 + 20;
				exp[2] = z * 60 + 30; exp[3] = 255;
				CHECK(check_uvec4(r, "3D", out.map, i, exp));
				break;
			}
			case 2: {
				struct rgba c = rgba_of(arr_col[i]);
				exp[0] = c.r; exp[1] = c.g; exp[2] = c.b; exp[3] = c.a;
				CHECK(check_uvec4(r, "2DArray", out.map, i, exp));
				break;
			}
			case 3: {
				uint32_t val = (i & 1u) == 0u ? 255u : 0u;
				exp[0] = exp[1] = exp[2] = exp[3] = val;
				CHECK(check_uvec4(r, "shadow", out.map, i, exp));
				break;
			}
			case 4: {
				exp[0] = 71; exp[1] = 87; exp[2] = 23; exp[3] = 7;
				CHECK(check_uvec4(r, "gather", out.map, i, exp));
				break;
			}
			case 5: {
				memcpy(exp, s_int.map + i * 16, 16);
				CHECK(check_uvec4(r, "int texelFetch", out.map, i, exp));
				break;
			}
			case 6: {
				memcpy(exp, s_flt.map + i * 16, 16);
				CHECK(check_uvec4(r, "float texelFetch", out.map, i, exp));
				break;
			}
			case 7: {
				const uint8_t *q = s_unn.map + i * 4;
				exp[0] = q[0]; exp[1] = q[1]; exp[2] = q[2]; exp[3] = q[3];
				CHECK(check_uvec4(r, "unnormalized", out.map, i, exp));
				break;
			}
			case 8: {
				exp[0] = 134; exp[1] = 68; exp[2] = 200; exp[3] = 255;
				CHECK(check_uvec4(r, "ETC2", out.map, i, exp));
				break;
			}
			}
		}
	}
	/* Extra probes: cube array, ASTC 4x4 LDR, anisotropic sampler */
	bool do_carr = v->feat.imageCubeArray;
	bool do_astc = v->feat.textureCompressionASTC_LDR;
	bool do_aniso = v->feat.samplerAnisotropy;

	if (do_carr || do_astc || do_aniso) {
		struct tex_obj t_carr = t_cube, t_astc = t_gat;
		struct buf s_carr, s_astc;
		uint32_t carr_col[12] = { 0 };
		VkSampler samp_aniso = samp_near;

		if (do_carr) {
			ci = base;
			ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			ci.arrayLayers = 12;
			CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
					  VK_IMAGE_ASPECT_COLOR_BIT, &t_carr));
			CHECK(buf_create(v, &s_carr, 4 * 4 * 12 * 4,
					 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
			for (uint32_t f = 0; f < 12; f++) {
				carr_col[f] = (lcg(seed + 500u + f * 29u, 2) & 0x00FFFFFFu) |
					      0xFF000000u;
				struct rgba c = rgba_of(carr_col[f]);

				for (uint32_t p = 0; p < 16; p++) {
					uint8_t *q = s_carr.map + (f * 16 + p) * 4;

					q[0] = c.r; q[1] = c.g; q[2] = c.b; q[3] = c.a;
				}
			}
		}
		if (do_astc) {
			ci = base;
			ci.format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
			CHECK(alloc_image(v, &ci, VK_IMAGE_VIEW_TYPE_2D,
					  VK_IMAGE_ASPECT_COLOR_BIT, &t_astc));
			CHECK(buf_create(v, &s_astc, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
			/* Void-extent ASTC 4x4 block: constant RGBA = (128, 64, 192, 255) */
			static const uint8_t astc_blk[16] = {
				0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
				0x00, 0x80, 0x00, 0x40, 0x00, 0xC0, 0xFF, 0xFF,
			};
			memcpy(s_astc.map, astc_blk, 16);
		}
		if (do_aniso) {
			VkSamplerCreateInfo asci = sci;

			asci.magFilter = VK_FILTER_LINEAR;
			asci.minFilter = VK_FILTER_LINEAR;
			asci.anisotropyEnable = VK_TRUE;
			asci.maxAnisotropy = v->props.limits.maxSamplerAnisotropy < 4.0f ?
					     v->props.limits.maxSamplerAnisotropy : 4.0f;
			CHECK(sampler_create(v, &asci, &samp_aniso));
		}

		/* Upload cube array and/or ASTC */
		if (do_carr || do_astc) {
			CHECK(cmd_begin(v, &cb));
			if (do_carr) {
				VkBufferImageCopy cca = {
					.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 12 },
					.imageExtent = { 4, 4, 1 },
				};
				tex_barrier(cb, &t_carr, VK_IMAGE_LAYOUT_UNDEFINED,
					    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
					    VK_ACCESS_TRANSFER_WRITE_BIT,
					    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT);
				vkCmdCopyBufferToImage(cb, s_carr.b, t_carr.i,
						       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
						       &cca);
				tex_barrier(cb, &t_carr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					    VK_ACCESS_TRANSFER_WRITE_BIT,
					    VK_ACCESS_SHADER_READ_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			}
			if (do_astc) {
				tex_barrier(cb, &t_astc, VK_IMAGE_LAYOUT_UNDEFINED,
					    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
					    VK_ACCESS_TRANSFER_WRITE_BIT,
					    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT);
				vkCmdCopyBufferToImage(cb, s_astc.b, t_astc.i,
						       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
						       &c2d);
				tex_barrier(cb, &t_astc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					    VK_ACCESS_TRANSFER_WRITE_BIT,
					    VK_ACCESS_SHADER_READ_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			}
			CHECK(cmd_submit_wait(v, cb));
		}

		/* Only create cubeArray pipeline if cubeArray is supported (SPIR-V capability) */
		if (do_carr) {
			VkDescriptorSetLayout edsl;
			VkDescriptorSet eset;
			VkPipelineLayout elay;
			VkPipeline epipe;
			VkShaderModule ecs;
			VkDescriptorSetLayoutBinding eb[3] = {
				{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
				  VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
				  VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
				  VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			};
			CHECK(desc_create(v, eb, 3, &edsl, &eset));
			VkDescriptorImageInfo eii[2] = {
				{ samp_near, t_carr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
				{ do_astc ? samp_near : samp_aniso, t_astc.view,
				  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			};
			VkWriteDescriptorSet ewr[3] = {
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = eset,
				  .dstBinding = 0, .descriptorCount = 1,
				  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				  .pImageInfo = &eii[0] },
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = eset,
				  .dstBinding = 1, .descriptorCount = 1,
				  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				  .pImageInfo = &eii[1] },
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = eset,
				  .dstBinding = 2, .descriptorCount = 1,
				  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				  .pBufferInfo = &obi },
			};
			vkUpdateDescriptorSets(v->dev, 3, ewr, 0, nullptr);
			CHECK(shader_create(v, &ecs, spv_tex2_extra_comp,
					    sizeof(spv_tex2_extra_comp)));
			CHECK(layout_create(v, &elay, VK_SHADER_STAGE_COMPUTE_BIT, 8, &edsl));
			VkComputePipelineCreateInfo ecci = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = ecs,
					   .pName = "main" },
				.layout = elay,
			};
			VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &ecci,
							     nullptr, &epipe));
			vk_track(v, OBJ_PIPELINE, H2U(epipe));

			/* Mode 0: cube array (12 faces) */
			uint32_t pc[2] = { 0, 12 };

			memset(out.map, 0, 12 * 16);
			CHECK(cmd_begin(v, &cb));
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, epipe);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, elay, 0, 1,
						&eset, 0, nullptr);
			vkCmdPushConstants(cb, elay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
			vkCmdDispatch(cb, 1, 1, 1);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
					     nullptr);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			for (uint32_t i = 0; i < 12; i++) {
				struct rgba c = rgba_of(carr_col[i]);
				uint32_t exp[4] = { c.r, c.g, c.b, c.a };

				CHECK(check_uvec4(r, "cubeArray", out.map, i, exp));
			}

			/* Mode 1: ASTC 4x4 or Aniso presence */
			if (do_astc || do_aniso) {
				pc[0] = 1;
				pc[1] = 16;
				memset(out.map, 0, 16 * 16);
				CHECK(cmd_begin(v, &cb));
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, epipe);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, elay, 0,
							1, &eset, 0, nullptr);
				vkCmdPushConstants(cb, elay, VK_SHADER_STAGE_COMPUTE_BIT, 0,
						   sizeof(pc), pc);
				vkCmdDispatch(cb, 1, 1, 1);
				vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
						     &hb, 0, nullptr);
				CHECK(cmd_submit_wait(v, cb));
				r->frames++;
				if (do_astc) {
					for (uint32_t i = 0; i < 16; i++) {
						uint32_t got[4];

						memcpy(got, out.map + i * 16, 16);
						r->checked += 4;
						uint32_t exp[4] = { 128, 64, 192, 255 };

						for (int k = 0; k < 4; k++) {
							uint32_t d = got[k] > exp[k] ?
								     got[k] - exp[k] :
								     exp[k] - got[k];
							if (d > 1) {
								r->mismatches++;
								snprintf(r->detail,
									 sizeof(r->detail),
									 "ASTC[%u]: got (%u,%u,%u,%u)",
									 i, got[0], got[1], got[2],
									 got[3]);
								return R_MISMATCH;
							}
						}
					}
				}
			}
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "cube%s, 3D, array, shadow, gather, int/float, unnorm%s%s%s",
		 do_carr ? "+arr" : "", do_etc ? ", ETC2" : "", do_astc ? ", ASTC" : "",
		 do_aniso ? ", aniso" : "");
	return R_PASS;
}
