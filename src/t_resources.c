// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * resources: compute shader resources in one dispatch (256 invocations = 4 x 64):
 *  - uniform buffer (std140);
 *  - storage buffer bound at a non-zero dynamic offset;
 *  - storage image (r32ui 64x4) written with imageStore;
 *  - uniform and storage texel buffers (R32_UINT);
 *  - atomicAdd counter;
 *  - shared memory with barrier() reversing each workgroup.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "res.comp.h"
#include "res_img.comp.h"
#include "dims.comp.h"
#include "fullscreen.vert.h"
#include "fragimg.frag.h"

constexpr uint32_t W = 64;
constexpr uint32_t H = 4;
constexpr uint32_t N = W * H;

enum result test_resources(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1);
	uint32_t align = (uint32_t)v->props.limits.minStorageBufferOffsetAlignment;
	uint32_t dyn_off = align > 256u ? align : 256u;
	struct buf ubo, outb, utex, stex, cnt, rb_img;
	struct img im;
	VkImageView im_view;
	VkBufferView uview, sview;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkShaderModule cs;
	VkCommandBuffer cb;

	uint32_t k[4] = { 7u + (seed & 7u), 13u + seed, 0x5A5A0000u ^ (seed * 2654435761u), 0u };

	CHECK(buf_create(v, &ubo, sizeof(k), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true));
	memcpy(ubo.map, k, sizeof(k));

	CHECK(buf_create(v, &outb, dyn_off + N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(outb.map, 0xA5, dyn_off);
	memset(outb.map + dyn_off, 0, N * 4);

	CHECK(img_create(v, &im, W, H, VK_FORMAT_R32_UINT,
			 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	VkImageViewCreateInfo ivci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = im.i,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R32_UINT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VK_CHECK(v, vkCreateImageView(v->dev, &ivci, nullptr, &im_view));
	vk_track(v, OBJ_VIEW, H2U(im_view));
	CHECK(buf_create(v, &rb_img, N * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));

	CHECK(buf_create(v, &utex, N * 4, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, true));
	CHECK(buf_create(v, &stex, N * 4, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, true));
	for (uint32_t i = 0; i < N; i++) {
		uint32_t val = 100u + i * 5u + seed;

		memcpy(utex.map + i * 4, &val, 4);
	}
	memset(stex.map, 0, N * 4);

	VkBufferViewCreateInfo bvci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
		.buffer = utex.b,
		.format = VK_FORMAT_R32_UINT,
		.range = VK_WHOLE_SIZE,
	};
	VK_CHECK(v, vkCreateBufferView(v->dev, &bvci, nullptr, &uview));
	vk_track(v, OBJ_BUFFER_VIEW, H2U(uview));
	bvci.buffer = stex.b;
	VK_CHECK(v, vkCreateBufferView(v->dev, &bvci, nullptr, &sview));
	vk_track(v, OBJ_BUFFER_VIEW, H2U(sview));

	CHECK(buf_create(v, &cnt, (1 + N) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(cnt.map, 0, (1 + N) * 4);

	VkDescriptorSetLayoutBinding binds[6] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
	};
	CHECK(desc_create(v, binds, 6, &dsl, &set));

	VkDescriptorBufferInfo b_ubo = { ubo.b, 0, sizeof(k) };
	VkDescriptorBufferInfo b_out = { outb.b, 0, N * 4 };
	VkDescriptorImageInfo i_img = { VK_NULL_HANDLE, im_view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorBufferInfo b_cnt = { cnt.b, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr[6] = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		  .pBufferInfo = &b_ubo },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
		  .pBufferInfo = &b_out },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		  .pImageInfo = &i_img },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
		  .pTexelBufferView = &uview },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
		  .pTexelBufferView = &sview },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 5,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &b_cnt },
	};
	vkUpdateDescriptorSets(v->dev, 6, wr, 0, nullptr);

	CHECK(shader_create(v, &cs, spv_res_comp, sizeof(spv_res_comp)));
	CHECK(layout_create(v, &layout, 0, 0, &dsl));
	VkComputePipelineCreateInfo cci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" },
		.layout = layout,
	};
	VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cci, nullptr, &pipe));
	vk_track(v, OBJ_PIPELINE, H2U(pipe));

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, &im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		    0, VK_ACCESS_SHADER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1, &dyn_off);
	vkCmdDispatch(cb, H, 1, 1);

	VkMemoryBarrier mb = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 1, &mb, 0, nullptr, 0, nullptr);
	img_barrier(cb, &im, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	cmd_copy_to_buffer(cb, &im, &rb_img);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	/* Check sentinel before dynamic offset */
	for (uint32_t i = 0; i < dyn_off; i++) {
		r->checked++;
		if (outb.map[i] != 0xA5) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "dynamic SSBO clobbered byte %u before offset %u (got 0x%02x)",
				 i, dyn_off, outb.map[i]);
			return R_MISMATCH;
		}
	}

	uint32_t counter;

	memcpy(&counter, cnt.map, 4);
	r->checked++;
	if (counter != N) {
		r->mismatches++;
		snprintf(r->detail, sizeof(r->detail), "atomic counter: got %u expected %u",
			 counter, N);
		return R_MISMATCH;
	}

	for (uint32_t i = 0; i < N; i++) {
		uint32_t g_out, g_img, g_tex, g_rev;
		uint32_t e_out = k[0] * i + k[1];
		uint32_t e_img = i ^ k[2];
		uint32_t e_tex = (100u + i * 5u + seed) + 7u;
		uint32_t e_rev = ((i & ~63u) + (63u - (i & 63u))) * 3u;

		memcpy(&g_out, outb.map + dyn_off + i * 4, 4);
		memcpy(&g_img, rb_img.map + i * 4, 4);
		memcpy(&g_tex, stex.map + i * 4, 4);
		memcpy(&g_rev, cnt.map + (1 + i) * 4, 4);
		r->checked += 4;
		if (g_out != e_out) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "UBO/dyn SSBO[%u]: got 0x%x expected 0x%x",
				 i, g_out, e_out);
			return R_MISMATCH;
		}
		if (g_img != e_img) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "storage image[%u]: got 0x%x expected 0x%x",
				 i, g_img, e_img);
			return R_MISMATCH;
		}
		if (g_tex != e_tex) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "texel buffer[%u]: got 0x%x expected 0x%x",
				 i, g_tex, e_tex);
			return R_MISMATCH;
		}
		if (g_rev != e_rev) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail), "shared mem rev[%u]: got 0x%x expected 0x%x",
				 i, g_rev, e_rev);
			return R_MISMATCH;
		}
	}
	/* Extra 1: image atomics + imageLoad */
	{
		struct img aim;
		struct buf aout;
		VkImageView aview;
		VkDescriptorSetLayout adsl;
		VkDescriptorSet aset;
		VkPipelineLayout alay;
		VkPipeline apipe;
		VkShaderModule acs;

		CHECK(img_create(v, &aim, 4, 4, VK_FORMAT_R32_UINT,
				 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		VkImageViewCreateInfo avci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = aim.i,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_R32_UINT,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		VK_CHECK(v, vkCreateImageView(v->dev, &avci, nullptr, &aview));
		vk_track(v, OBJ_VIEW, H2U(aview));
		CHECK(buf_create(v, &aout, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
		memset(aout.map, 0, 16);

		VkDescriptorSetLayoutBinding ab[2] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		};
		CHECK(desc_create(v, ab, 2, &adsl, &aset));
		VkDescriptorImageInfo aii = { VK_NULL_HANDLE, aview, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorBufferInfo abi = { aout.b, 0, 16 };
		VkWriteDescriptorSet awr[2] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = aset,
			  .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &aii },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = aset,
			  .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &abi },
		};
		vkUpdateDescriptorSets(v->dev, 2, awr, 0, nullptr);
		CHECK(shader_create(v, &acs, spv_res_img_comp, sizeof(spv_res_img_comp)));
		CHECK(layout_create(v, &alay, 0, 0, &adsl));
		VkComputePipelineCreateInfo acci = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = acs,
				   .pName = "main" },
			.layout = alay,
		};
		VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &acci, nullptr, &apipe));
		vk_track(v, OBJ_PIPELINE, H2U(apipe));

		VkClearColorValue zero_c = { .uint32 = { 0, 0, 0, 0 } };
		VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &aim, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdClearColorImage(cb, aim.i, VK_IMAGE_LAYOUT_GENERAL, &zero_c, 1, &rng);
		img_barrier(cb, &aim, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, apipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, alay, 0, 1, &aset, 0,
					nullptr);
		vkCmdDispatch(cb, 1, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		uint32_t got;

		memcpy(&got, aout.map, 4);
		r->checked++;
		if (got != 64u) {
			r->mismatches++;
			snprintf(r->detail, sizeof(r->detail),
				 "imageAtomicAdd: got %u expected 64", got);
			return R_MISMATCH;
		}
	}

	/* Extra 2: fragment shader imageLoad / imageStore */
	bool did_frag_img = false;

	if (v->feat.fragmentStoresAndAtomics) {
		constexpr uint32_t FW = 16;
		constexpr uint32_t FH = 16;
		struct img sim, dim, col;
		struct buf stg, rb;
		VkImageView sview_f, dview_f;
		VkRenderPass rp;
		VkFramebuffer fb;
		VkDescriptorSetLayout fdsl;
		VkDescriptorSet fset;
		VkPipelineLayout flay;
		VkPipeline fpipe;
		VkShaderModule vs, fs;

		CHECK(img_create(v, &sim, FW, FH, VK_FORMAT_R32_UINT,
				 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(img_create(v, &dim, FW, FH, VK_FORMAT_R32_UINT,
				 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				 VK_SAMPLE_COUNT_1_BIT));
		CHECK(img_create(v, &col, FW, FH, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT));
		VkImageViewCreateInfo iv = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = sim.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_R32_UINT,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		VK_CHECK(v, vkCreateImageView(v->dev, &iv, nullptr, &sview_f));
		vk_track(v, OBJ_VIEW, H2U(sview_f));
		iv.image = dim.i;
		VK_CHECK(v, vkCreateImageView(v->dev, &iv, nullptr, &dview_f));
		vk_track(v, OBJ_VIEW, H2U(dview_f));

		CHECK(buf_create(v, &stg, FW * FH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
		CHECK(buf_create(v, &rb, FW * FH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
		for (uint32_t i = 0; i < FW * FH; i++) {
			uint32_t val = i + seed;

			memcpy(stg.map + i * 4, &val, 4);
		}
		CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
		CHECK(fb_create(v, &fb, rp, &col, nullptr));
		VkDescriptorSetLayoutBinding fb_b[2] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		};
		CHECK(desc_create(v, fb_b, 2, &fdsl, &fset));
		VkDescriptorImageInfo fii[2] = {
			{ VK_NULL_HANDLE, sview_f, VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, dview_f, VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet fwr[2] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fset,
			  .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &fii[0] },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fset,
			  .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &fii[1] },
		};
		vkUpdateDescriptorSets(v->dev, 2, fwr, 0, nullptr);
		CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
		CHECK(shader_create(v, &fs, spv_fragimg_frag, sizeof(spv_fragimg_frag)));
		CHECK(layout_create(v, &flay, 0, 0, &fdsl));
		struct gfx_desc gd = { .rp = rp, .layout = flay, .vs = vs, .fs = fs };

		CHECK(gfx_pipeline(v, &fpipe, &gd));
		VkBufferImageCopy bic = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { FW, FH, 1 },
		};
		VkClearValue clr = { .color.float32 = { 0, 0, 0, 0 } };
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp, .framebuffer = fb,
			.renderArea = { { 0, 0 }, { FW, FH } },
			.clearValueCount = 1, .pClearValues = &clr,
		};
		VkViewport vp = { 0, 0, (float)FW, (float)FH, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { FW, FH } };

		CHECK(cmd_begin(v, &cb));
		img_barrier(cb, &sim, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    0, VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cb, stg.b, sim.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
		img_barrier(cb, &sim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		img_barrier(cb, &dim, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			    0, VK_ACCESS_SHADER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fpipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, flay, 0, 1, &fset, 0,
					nullptr);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		img_barrier(cb, &dim, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		cmd_copy_to_buffer(cb, &dim, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;
		for (uint32_t i = 0; i < FW * FH; i++) {
			uint32_t got, exp = i + seed + 10u;

			memcpy(&got, rb.map + i * 4, 4);
			r->checked++;
			if (got != exp) {
				r->mismatches++;
				snprintf(r->detail, sizeof(r->detail),
					 "frag imageLoad/Store[%u]: got %u expected %u", i, got, exp);
				return R_MISMATCH;
			}
		}
		did_frag_img = true;
	}

	/* Extra 3: push descriptors (VK_KHR_push_descriptor) */
	bool did_push = false;

	if (v->has_push_desc) {
		PFN_vkCmdPushDescriptorSetKHR push_fn =
			(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(v->dev,
									   "vkCmdPushDescriptorSetKHR");
		if (push_fn) {
			struct buf pb;
			VkDescriptorSetLayout pdsl;
			VkPipelineLayout play;
			VkPipeline ppipe;
			VkShaderModule pcs;
			uint32_t pc[4] = { 4, 2, 2, 0 };

			CHECK(buf_create(v, &pb, 16 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
			memset(pb.map, 0, 16 * 4);
			VkDescriptorSetLayoutBinding pbind = {
				0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
				VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
			};
			VkDescriptorSetLayoutCreateInfo pdci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
				.bindingCount = 1,
				.pBindings = &pbind,
			};
			VK_CHECK(v, vkCreateDescriptorSetLayout(v->dev, &pdci, nullptr, &pdsl));
			vk_track(v, OBJ_SET_LAYOUT, H2U(pdsl));
			CHECK(shader_create(v, &pcs, spv_dims_comp, sizeof(spv_dims_comp)));
			CHECK(layout_create(v, &play, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &pdsl));
			VkComputePipelineCreateInfo pcci = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = pcs,
					   .pName = "main" },
				.layout = play,
			};
			VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &pcci,
							     nullptr, &ppipe));
			vk_track(v, OBJ_PIPELINE, H2U(ppipe));

			VkDescriptorBufferInfo pbi = { pb.b, 0, VK_WHOLE_SIZE };
			VkWriteDescriptorSet pwr = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding = 0, .descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &pbi,
			};
			CHECK(cmd_begin(v, &cb));
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ppipe);
			push_fn(cb, VK_PIPELINE_BIND_POINT_COMPUTE, play, 0, 1, &pwr);
			vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
			vkCmdDispatch(cb, 1, 1, 1);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
					     nullptr);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			for (uint32_t z = 0; z < 2; z++)
				for (uint32_t y = 0; y < 2; y++)
					for (uint32_t x = 0; x < 4; x++) {
						uint32_t idx = (z * 2 + y) * 4 + x;
						uint32_t got, exp = 0x10000u + x + y * 256u +
								    z * 65536u;

						memcpy(&got, pb.map + idx * 4, 4);
						r->checked++;
						if (got != exp) {
							r->mismatches++;
							snprintf(r->detail, sizeof(r->detail),
								 "push desc[%u]: got 0x%x exp 0x%x",
								 idx, got, exp);
							return R_MISMATCH;
						}
					}
			did_push = true;
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "UBO, dyn SSBO (+%u), storage img, texel bufs, buf+img atomics, shared mem%s%s",
		 dyn_off, did_frag_img ? ", frag img" : "", did_push ? ", push desc" : "");
	return R_PASS;
}
