// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * msaa: a 4x render resolved at the end of the render pass (resolve
 * attachment). A triangle covers the pixels above the main diagonal (x > y):
 * those must be C1, those below C0. On the diagonal, the standard 4x sample
 * positions put exactly 2 of 4 samples inside, so the resolve gives
 * (C1 + C0) / 2 (+-1); with non-standard positions the diagonal is skipped.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "poly.vert.h"
#include "poly.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct poly_pc {
	float p01[4], p2[4];
	uint32_t color;
};

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

static bool close8(uint8_t a, uint8_t b)
{
	return (a > b ? a - b : b - a) <= 1;
}

static enum result run_msaa_half(struct vk *v, struct report *r, uint32_t w, uint32_t h,
				 uint32_t seed, VkSampleCountFlagBits samples, uint32_t n)
{
	struct img ms, res;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;
	uint32_t c0 = (lcg(seed + n, 2) & 0x00FFFFFFu) | 0xFF000000u;
	uint32_t c1 = (lcg(seed + n, 3) & 0x00FFFFFFu) | 0xFF000000u;

	CHECK(img_create(v, &ms, w, h, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, samples));
	CHECK(img_create(v, &res, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	struct rp_desc rd = { .color = FMT, .samples = samples, .resolve = true };

	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { ms.view, res.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, 1));
	CHECK(shader_create(v, &vs, spv_poly_vert, sizeof(spv_poly_vert)));
	CHECK(shader_create(v, &fs, spv_poly_frag, sizeof(spv_poly_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct poly_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs, .samples = samples };
	CHECK(gfx_pipeline(v, &pipe, &d));

	VkClearValue clear = clear_rgba(c0);
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	/* large triangle covering the right half (x >= w/2) */
	struct poly_pc pc = { { 0, -1, 2, -1 }, { 0, 3, 0, 0 }, c1 };
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };
	VkCommandBuffer cb;

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			   0, sizeof(pc), &pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &res, &rb);
	CHECK(cmd_submit_wait(v, cb));
	r->frames++;

	struct rgba a = rgba_of(c0), b = rgba_of(c1);
	uint64_t bad = 0;

	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = rb.map + ((size_t)y * w + x) * 4;
			struct rgba e = x >= w / 2 ? b : a;

			if (p[0] == e.r && p[1] == e.g && p[2] == e.b && p[3] == e.a)
				continue;
			if (bad < 8 && v->verbose >= 0)
				fprintf(stderr, "vkfwtest: %ux: (%u,%u) got %02x%02x%02x%02x"
					" expected %02x%02x%02x%02x\n",
					n, x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
			bad++;
		}
	r->checked += (uint64_t)w * h;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail), "%ux: %" PRIu64 " of %" PRIu64 " pixels wrong",
			 n, bad, (uint64_t)w * h);
		return R_MISMATCH;
	}
	return R_PASS;
}

enum result test_msaa(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), frames = OPT(o->frames, 2), seed = OPT(o->seed, 1);
	uint32_t h = w;         /* square: the diagonal runs corner to corner */
	bool standard = v->props.limits.standardSampleLocations;
	VkSampleCountFlags sc_mask = v->props.limits.framebufferColorSampleCounts;
	bool did2 = false, did8 = false;
	struct img ms, res;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;

	if (!(sc_mask & VK_SAMPLE_COUNT_4_BIT)) {
		snprintf(r->detail, sizeof(r->detail), "4x colour multisampling not supported");
		return R_SKIP;
	}
	if (sc_mask & VK_SAMPLE_COUNT_2_BIT) {
		CHECK(run_msaa_half(v, r, w, h, seed, VK_SAMPLE_COUNT_2_BIT, 2));
		did2 = true;
	}
	if (sc_mask & VK_SAMPLE_COUNT_8_BIT) {
		CHECK(run_msaa_half(v, r, w, h, seed, VK_SAMPLE_COUNT_8_BIT, 8));
		did8 = true;
	}
	CHECK(img_create(v, &ms, w, h, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			 VK_SAMPLE_COUNT_4_BIT));
	CHECK(img_create(v, &res, w, h, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	struct rp_desc rd = { .color = FMT, .samples = VK_SAMPLE_COUNT_4_BIT, .resolve = true };

	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { ms.view, res.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, 1));
	CHECK(shader_create(v, &vs, spv_poly_vert, sizeof(spv_poly_vert)));
	CHECK(shader_create(v, &fs, spv_poly_frag, sizeof(spv_poly_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    sizeof(struct poly_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs,
			      .samples = VK_SAMPLE_COUNT_4_BIT };
	CHECK(gfx_pipeline(v, &pipe, &d));

	for (uint32_t f = 0; f < frames; f++) {
		/* even channels: the 2-of-4 average is an integer */
		uint32_t c0 = (lcg(seed + f, 2) & 0x00FEFEFEu) | 0xFF000000u;
		uint32_t c1 = (lcg(seed + f, 3) & 0x00FEFEFEu) | 0xFF000000u;
		VkClearValue clear = clear_rgba(c0);
		VkRenderPassBeginInfo rbi = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp,
			.framebuffer = fb,
			.renderArea = { { 0, 0 }, { w, h } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		struct poly_pc pc = { { -1, -1, 1, -1 }, { 1, 1, 0, 0 }, c1 };
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { w, h } };
		VkCommandBuffer cb;
		uint64_t bad = 0, checked = 0;

		CHECK(cmd_begin(v, &cb));
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, sizeof(pc), &pc);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
		cmd_copy_to_buffer(cb, &res, &rb);
		CHECK(cmd_submit_wait(v, cb));
		r->frames++;

		struct rgba a = rgba_of(c0), b = rgba_of(c1);
		struct rgba mid = { (uint8_t)((a.r + b.r) / 2), (uint8_t)((a.g + b.g) / 2),
				    (uint8_t)((a.b + b.b) / 2), 0xFF };

		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				const uint8_t *p = rb.map + ((size_t)y * w + x) * 4;
				struct rgba e = x > y ? b : x < y ? a : mid;
				bool ok;

				if (x == y && !standard)
					continue;
				checked++;
				ok = x == y ? close8(p[0], e.r) && close8(p[1], e.g) &&
						      close8(p[2], e.b) && close8(p[3], e.a)
					    : p[0] == e.r && p[1] == e.g && p[2] == e.b && p[3] == e.a;
				if (ok)
					continue;
				if (bad < 8 && v->verbose >= 0)
					fprintf(stderr,
						"vkfwtest: frame %u: (%u,%u) got %02x%02x%02x%02x expected"
						" %02x%02x%02x%02x\n",
						f, x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
				bad++;
			}
		r->checked += checked;
		r->mismatches += bad;
		if (bad) {
			snprintf(r->detail, sizeof(r->detail),
				 "frame %u: %" PRIu64 " of %" PRIu64 " resolved pixels wrong", f, bad,
				 checked);
			return R_MISMATCH;
		}
	}
	/* Depth resolve (VK_KHR_depth_stencil_resolve / core 1.2) */
	bool did_ds_res = false;

	if (v->has_ds_resolve) {
		VkPhysicalDeviceDepthStencilResolveProperties dsrp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
		};
		VkPhysicalDeviceProperties2 p2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &dsrp,
		};
		vkGetPhysicalDeviceProperties2(v->phys, &p2);
		PFN_vkCreateRenderPass2 crp2 =
			v->api >= VK_API_VERSION_1_2 ? vkCreateRenderPass2 :
			(PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(v->dev, "vkCreateRenderPass2KHR");
		if (crp2 && (dsrp.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) {
			struct img dms, dres;
			struct buf drb;
			VkRenderPass drp;
			VkFramebuffer dfb;
			VkCommandBuffer cb;

			CHECK(img_create(v, &dms, w, h, VK_FORMAT_D32_SFLOAT,
					 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
					 VK_SAMPLE_COUNT_4_BIT));
			CHECK(img_create(v, &dres, w, h, VK_FORMAT_D32_SFLOAT,
					 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
					 VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT));
			CHECK(buf_create(v, &drb, (VkDeviceSize)w * h * 4,
					 VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
			VkAttachmentDescription2 atts[2] = {
				{ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
				  .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_4_BIT,
				  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				  .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
				{ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
				  .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
				  .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				  .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
			};
			VkAttachmentReference2 dref = {
				.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
				.attachment = 0,
				.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			};
			VkAttachmentReference2 rref = {
				.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
				.attachment = 1,
				.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			};
			VkSubpassDescriptionDepthStencilResolve ds_res = {
				.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
				.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
				.stencilResolveMode = VK_RESOLVE_MODE_NONE,
				.pDepthStencilResolveAttachment = &rref,
			};
			VkSubpassDescription2 sp2 = {
				.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
				.pNext = &ds_res,
				.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.pDepthStencilAttachment = &dref,
			};
			VkSubpassDependency2 dep2 = {
				.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
				.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
				.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
				.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			};
			VkRenderPassCreateInfo2 rpci2 = {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
				.attachmentCount = 2, .pAttachments = atts,
				.subpassCount = 1, .pSubpasses = &sp2,
				.dependencyCount = 1, .pDependencies = &dep2,
			};
			VK_CHECK(v, crp2(v->dev, &rpci2, nullptr, &drp));
			vk_track(v, OBJ_RENDER_PASS, H2U(drp));
			VkImageView dviews[2] = { dms.view, dres.view };

			CHECK(fb_create_views(v, &dfb, drp, dviews, 2, w, h, 1));
			VkClearValue dclr = { .depthStencil = { 0.25f, 0 } };
			VkRenderPassBeginInfo drbi = {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = drp, .framebuffer = dfb,
				.renderArea = { { 0, 0 }, { w, h } },
				.clearValueCount = 1, .pClearValues = &dclr,
			};
			VkBufferImageCopy dbic = {
				.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
				.imageExtent = { w, h, 1 },
			};
			VkBufferMemoryBarrier hb = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = drb.b, .size = VK_WHOLE_SIZE,
			};
			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &drbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cb);
			vkCmdCopyImageToBuffer(cb, dres.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					       drb.b, 1, &dbic);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
					     nullptr);
			CHECK(cmd_submit_wait(v, cb));
			r->frames++;
			for (uint32_t i = 0; i < w * h; i++) {
				float got;

				memcpy(&got, drb.map + i * 4, 4);
				r->checked++;
				if (got != 0.25f) {
					r->mismatches++;
					snprintf(r->detail, sizeof(r->detail),
						 "depth resolve[%u]: got %g expected 0.25",
						 i, (double)got);
					return R_MISMATCH;
				}
			}
			did_ds_res = true;
		}
	}

	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u %s4x%s, resolve attachment%s, diagonal edge%s, %u frames", w, h,
		 did2 ? "2x/" : "", did8 ? "/8x" : "",
		 did_ds_res ? " + depth resolve" : "",
		 standard ? " (standard sample positions)" : " (not checked: custom positions)",
		 frames);
	return R_PASS;
}
