// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * rta: layered rendering into a 6-layer 2D array (render-target arrays,
 * firmware 1.1 "gs_rta_support"). A colour and a D32 depth array, both
 * attached with framebuffer layers = 6.
 *
 * With gs_rta_support the driver sends the whole framebuffer as one render
 * with 6 layers, so the firmware must run each layer (region headers, PBE
 * address, ZLS base per layer). Without it, each layer is a render of its
 * own. The expected images are the same either way.
 *
 *  1. one render pass:
 *     - load op CLEAR: every colour layer to B, every depth layer to 1.0;
 *     - a draw without gl_Layer: a per-pixel LCG pattern, layer 0 only;
 *     - vkCmdClearAttachments of the odd colour layers, one layer each;
 *     - one vkCmdClearAttachments rect over layers 2-4 (a multi-layer clear);
 *     - a depth clear of each layer l >= 1 to l / 8;
 *  2. layered draw, when VK_EXT_shader_viewport_index_layer is available:
 *     one full-screen triangle per instance, sent to layer gl_InstanceIndex;
 *  3. some layers only: load op LOAD, then a clear of layers 1 (whole area)
 *     and 5 (an inner rect); the
 *     firmware renders only the layers the geometry marked, and the others
 *     must keep what step 1 or 2 left;
 *  4. stencil, when D24S8 or D32S8 is an attachment format: a clear of each
 *     layer l >= 1 to its own stencil value (per-layer stencil base);
 *  5. a 3D image viewed as a 6-layer 2D array (Vulkan 1.1): the layers are
 *     slices of a TWIDDLE_3D surface, addressed by the PBE z slice;
 *  6. multiview (VK_KHR_multiview) with 6 views over the 6 layers: one draw
 *     puts geometry in every layer, each with its own colour (gl_ViewIndex).
 *     It stands in for the gl_Layer draw where that extension is missing,
 *     and runs on a device of its own, as the first render there.
 * Every colour pixel, depth texel and stencil value of every layer is checked.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "color.frag.h"
#include "layer.vert.h"
#include "layer.frag.h"
#include "mvlayer.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat DFMT = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t L = 6;
constexpr uint32_t MULTI_BASE = 2;      /* the multi-layer clear: layers 2-4 */
constexpr uint32_t MULTI_COUNT = 3;

struct color_pc {
	uint32_t color, iters, seed;
};

static uint32_t layer_color(uint32_t layer, uint32_t seed)
{
	uint32_t hsh = (layer + 1u) * 0x9E3779B1u ^ seed;

	hsh ^= hsh >> 13;
	return (hsh & 0x00FFFFFFu) | 0xFF000000u;
}

/* What one layer must hold: a solid colour or the LCG pattern, plus a rect. */
struct layer_expect {
	uint32_t c, seed;
	bool lcg;
	bool rect;
	uint32_t rect_c, x0, y0, x1, y1;
};

static struct rgba layer_fn(uint32_t x, uint32_t y, const void *ctx)
{
	const struct layer_expect *e = ctx;

	if (e->rect && x >= e->x0 && x < e->x1 && y >= e->y0 && y < e->y1)
		return rgba_of(e->rect_c);
	if (e->lcg)
		return rgba_of(lcg(e->seed + x + y * 65536u, 1) | 0xFF000000u);
	return rgba_of(e->c);
}

static VkClearValue clear_rgba(uint32_t c)
{
	struct rgba b = rgba_of(c);

	return (VkClearValue){ .color.float32 = { b.r / 255.0f, b.g / 255.0f, b.b / 255.0f,
						  b.a / 255.0f } };
}

static enum result check_depth(struct report *r, const char *what, uint32_t layer,
			       const uint8_t *px, uint32_t n, float want)
{
	for (uint32_t i = 0; i < n; i++) {
		float got;

		memcpy(&got, px + (size_t)i * 4, 4);
		r->checked++;
		if (got != want) {
			uint32_t bad = 0;

			for (uint32_t j = i; j < n; j++) {
				memcpy(&got, px + (size_t)j * 4, 4);
				bad += got != want;
			}
			r->mismatches += bad;
			memcpy(&got, px + (size_t)i * 4, 4);
			snprintf(r->detail, sizeof(r->detail),
				 "%s depth layer %u: %u of %u texels wrong (texel %u: %g, expected %g)",
				 what, layer, bad, n, i, (double)got, (double)want);
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

/* Read every colour and depth layer back and compare. */
static enum result check_layers(struct vk *v, struct report *r, const char *what,
				const struct img *im, const struct buf *rb,
				const struct buf *db, const struct layer_expect *ce,
				const float *de)
{
	size_t layer_bytes = (size_t)im->w * im->h * 4;

	for (uint32_t l = 0; l < L; l++) {
		char w[48];

		snprintf(w, sizeof(w), "%s layer %u", what, l);
		CHECK(check_rgba(v, r, w, rb->map + l * layer_bytes, im->w, im->h, layer_fn,
				 &ce[l]));
		CHECK(check_depth(r, what, l, db->map + l * layer_bytes, im->w * im->h, de[l]));
	}
	return R_PASS;
}

/* odd layers one by one, then one rect over layers 2-4: what step 1 clears */
static void clears_odd_and_multi(VkCommandBuffer cb, struct layer_expect *ce, uint32_t w,
				 uint32_t h, uint32_t s)
{
	VkRect2D full = { { 0, 0 }, { w, h } };
	VkRect2D quarter = { { (int32_t)(w / 4), (int32_t)(h / 4) }, { w / 2, h / 2 } };
	uint32_t multi = layer_color(77, s);

	for (uint32_t l = 1; l < L; l += 2) {
		VkClearAttachment ca = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.clearValue = clear_rgba(layer_color(l, s)),
		};
		VkClearRect cr = { full, l, 1 };

		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
		ce[l].c = layer_color(l, s);
	}
	VkClearAttachment ma = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.clearValue = clear_rgba(multi),
	};
	VkClearRect mr = { quarter, MULTI_BASE, MULTI_COUNT };

	vkCmdClearAttachments(cb, 1, &ma, 1, &mr);
	for (uint32_t l = MULTI_BASE; l < MULTI_BASE + MULTI_COUNT; l++) {
		ce[l].rect = true;
		ce[l].rect_c = multi;
		ce[l].x0 = w / 4;
		ce[l].y0 = h / 4;
		ce[l].x1 = w / 4 + w / 2;
		ce[l].y1 = h / 4 + h / 2;
	}
}

static enum result check_colour_layers(struct vk *v, struct report *r, const char *what,
				       const struct buf *rb, uint32_t w, uint32_t h,
				       const struct layer_expect *ce)
{
	enum result res = R_PASS;
	char first[sizeof(r->detail)] = "";

	/* every layer, so that a failure shows which layers are wrong */
	for (uint32_t l = 0; l < L; l++) {
		char t[48];
		enum result e;

		snprintf(t, sizeof(t), "%s layer %u", what, l);
		e = check_rgba(v, r, t, rb->map + (size_t)l * w * h * 4, w, h, layer_fn, &ce[l]);
		if (e != R_PASS && res == R_PASS) {
			res = e;
			memcpy(first, r->detail, sizeof(first));
		}
	}
	if (res != R_PASS)
		memcpy(r->detail, first, sizeof(first));
	return res;
}

/* 3. LOAD, then clears of layers 1 and 5 only; `ce` holds the previous contents */
static enum result rta_subset(struct vk *v, struct report *r, struct img *im,
			      const struct buf *rb, struct layer_expect *ce, uint32_t s)
{
	static const uint32_t pick[2] = { 1, 5 };      /* 5: no rect from step 1 */
	uint32_t w = im->w, h = im->h;
	struct rp_desc rd = { .color = FMT, .color_load = true };
	VkRenderPass rp;
	VkFramebuffer fb;
	VkCommandBuffer cb;

	CHECK(rp_build(v, &rp, &rd));
	CHECK(fb_create_views(v, &fb, rp, &im->view, 1, w, h, L));
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
	};
	VkRect2D full = { { 0, 0 }, { w, h } };

	CHECK(cmd_begin(v, &cb));
	img_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	/* layer 1: the whole render area; layer 5: an inner rect over the old contents */
	for (uint32_t i = 0; i < 2; i++) {
		uint32_t l = pick[i], c = layer_color(l + 10, s);
		VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					 .clearValue = clear_rgba(c) };
		VkRect2D inner = { { (int32_t)(w / 8), (int32_t)(h / 8) }, { w / 2, h / 2 } };
		VkClearRect cr = { i ? inner : full, l, 1 };

		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
		if (!i) {
			ce[l] = (struct layer_expect){ .c = c };
			continue;
		}
		ce[l].rect = true;
		ce[l].rect_c = c;
		ce[l].x0 = w / 8;
		ce[l].y0 = h / 8;
		ce[l].x1 = w / 8 + w / 2;
		ce[l].y1 = h / 8 + h / 2;
	}
	vkCmdEndRenderPass(cb);
	for (uint32_t l = 0; l < L; l++)
		cmd_copy_layer_to_buffer(cb, im, l, rb, (VkDeviceSize)l * w * h * 4);
	CHECK(cmd_submit_wait(v, cb));
	return check_colour_layers(v, r, "subset", rb, w, h, ce);
}

static uint8_t stencil_of(uint32_t l)
{
	return (uint8_t)(l * 37u + 5u);
}

/* 4. per-layer stencil clears; R_SKIP without a depth/stencil attachment format */
static enum result rta_stencil(struct vk *v, struct report *r, uint32_t w, uint32_t h,
			       VkFormat *used)
{
	static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT,
					  VK_FORMAT_D32_SFLOAT_S8_UINT };
	VkFormat dsf = VK_FORMAT_UNDEFINED;
	struct img col, ds;
	struct buf sb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkCommandBuffer cb;
	VkImageView dsv;

	for (uint32_t i = 0; i < 2 && dsf == VK_FORMAT_UNDEFINED; i++) {
		VkFormatProperties fp;

		vkGetPhysicalDeviceFormatProperties(v->phys, cand[i], &fp);
		if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			dsf = cand[i];
	}
	if (dsf == VK_FORMAT_UNDEFINED)
		return R_SKIP;
	*used = dsf;
	CHECK(img_create_layers(v, &col, w, h, L, FMT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(img_create_layers(v, &ds, w, h, L, dsf,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	/* the attachment view covers both aspects */
	ds.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = ds.i,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = dsf,
		.subresourceRange = { ds.aspect, 0, 1, 0, L },
	};
	VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &dsv));
	vk_track(v, OBJ_VIEW, H2U(dsv));
	CHECK(buf_create(v, &sb, (VkDeviceSize)w * h * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	memset(sb.map, 0xEE, (size_t)w * h * L);
	struct rp_desc rd = { .color = FMT, .depth = dsf, .stencil = true };

	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { col.view, dsv };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, L));
	VkClearValue clear[2] = { { .color.float32 = { 0, 0, 0, 1 } },
				  { .depthStencil = { 1.0f, 0 } } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 2,
		.pClearValues = clear,
	};
	VkRect2D full = { { 0, 0 }, { w, h } };

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	for (uint32_t l = 1; l < L; l++) {
		VkClearAttachment sa = {
			.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
			.clearValue = { .depthStencil = { 0.0f, stencil_of(l) } },
		};
		VkClearRect sr = { full, l, 1 };

		vkCmdClearAttachments(cb, 1, &sa, 1, &sr);
	}
	vkCmdEndRenderPass(cb);
	img_barrier(cb, &ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT);
	for (uint32_t l = 0; l < L; l++) {
		VkBufferImageCopy c = {
			.bufferOffset = (VkDeviceSize)l * w * h,
			.imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, l, 1 },
			.imageExtent = { w, h, 1 },
		};

		vkCmdCopyImageToBuffer(cb, ds.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb.b, 1, &c);
	}
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = sb.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
			     nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	for (uint32_t l = 0; l < L; l++) {
		const uint8_t *px = sb.map + (size_t)l * w * h;
		uint8_t want = l ? stencil_of(l) : 0;
		uint32_t bad = 0, first = 0;

		for (uint32_t i = 0; i < w * h; i++) {
			r->checked++;
			if (px[i] != want && !bad++)
				first = i;
		}
		if (bad) {
			r->mismatches += bad;
			snprintf(r->detail, sizeof(r->detail),
				 "stencil layer %u: %u of %u values wrong (texel %u: %u, expected %u)",
				 l, bad, w * h, first, px[first], want);
			return R_MISMATCH;
		}
	}
	return R_PASS;
}

/* 5. a 3D image as a 2D array: the step-1 clears; R_SKIP below Vulkan 1.1 */
static enum result rta_3d(struct vk *v, struct report *r, uint32_t w, uint32_t h, uint32_t s)
{
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImageFormatProperties ifp;
	struct img im;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkCommandBuffer cb;
	struct layer_expect ce[L];
	uint32_t base = layer_color(99, s);
	struct rp_desc rd = { .color = FMT };
	enum result e;

	if (v->api < VK_API_VERSION_1_1 ||
	    vkGetPhysicalDeviceImageFormatProperties(v->phys, FMT, VK_IMAGE_TYPE_3D,
						     VK_IMAGE_TILING_OPTIMAL, usage,
						     VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
						     &ifp) != VK_SUCCESS ||
	    ifp.maxExtent.depth < L)
		return R_SKIP;
	e = img_create_3d_array(v, &im, w, h, L, FMT, usage);
	if (e != R_PASS)
		return e;
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_build(v, &rp, &rd));
	CHECK(fb_create_views(v, &fb, rp, &im.view, 1, w, h, L));
	VkClearValue clear = clear_rgba(base);
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};

	for (uint32_t l = 0; l < L; l++)
		ce[l] = (struct layer_expect){ .c = base };
	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	clears_odd_and_multi(cb, ce, w, h, s);
	vkCmdEndRenderPass(cb);
	for (uint32_t l = 0; l < L; l++) {
		VkBufferImageCopy c = {
			.bufferOffset = (VkDeviceSize)l * w * h * 4,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageOffset = { 0, 0, (int32_t)l },
			.imageExtent = { w, h, 1 },
		};

		vkCmdCopyImageToBuffer(cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &c);
	}
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = rb.b,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
			     nullptr, 1, &hb, 0, nullptr);
	CHECK(cmd_submit_wait(v, cb));
	return check_colour_layers(v, r, "3D slice", &rb, w, h, ce);
}

/* 6. one full-screen draw in a 6-view multiview render pass; R_SKIP without multiview */
static enum result rta_multiview(struct vk *v, struct report *r, uint32_t w, uint32_t h,
				 uint32_t s)
{
	VkPhysicalDeviceMultiviewProperties mvp = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 p2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &mvp,
	};
	struct img im;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	struct layer_expect ce[L];

	if (!v->has_multiview || v->api < VK_API_VERSION_1_1)
		return R_SKIP;
	vkGetPhysicalDeviceProperties2(v->phys, &p2);
	if (mvp.maxMultiviewViewCount < L)
		return R_SKIP;
	CHECK(img_create_layers(v, &im, w, h, L, FMT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	VkAttachmentDescription a = {
		.format = FMT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &cref,
	};
	VkSubpassDependency dep = {
		.srcSubpass = 0,
		.dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	uint32_t view_mask = (1u << L) - 1, corr_mask = view_mask;
	VkRenderPassMultiviewCreateInfo mv = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
		.subpassCount = 1,
		.pViewMasks = &view_mask,
		.correlationMaskCount = 1,
		.pCorrelationMasks = &corr_mask,
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.pNext = &mv,
		.attachmentCount = 1,
		.pAttachments = &a,
		.subpassCount = 1,
		.pSubpasses = &sp,
		.dependencyCount = 1,
		.pDependencies = &dep,
	};
	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, &rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(rp));
	CHECK(fb_create_views(v, &fb, rp, &im.view, 1, w, h, 1));  /* multiview: layers = 1 */
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_mvlayer_frag, sizeof(spv_mvlayer_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };

	CHECK(gfx_pipeline(v, &pipe, &d));
	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };
	uint32_t seed = s + 555u;

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(seed), &seed);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	for (uint32_t l = 0; l < L; l++) {
		cmd_copy_layer_to_buffer(cb, &im, l, &rb, (VkDeviceSize)l * w * h * 4);
		ce[l] = (struct layer_expect){ .c = layer_color(l, seed) };
	}
	CHECK(cmd_submit_wait(v, cb));
	return check_colour_layers(v, r, "multiview", &rb, w, h, ce);
}

/* step 6 on a fresh device: run_fresh() calls it with the device it made */
static enum result rta_multiview_fresh(struct vk *v, const struct opts *o, struct report *r)
{
	enum result e = rta_multiview(v, r, OPT(o->width, 64), OPT(o->height, 64),
				      OPT(o->seed, 1));

	if (e == R_SKIP)
		snprintf(r->detail, sizeof(r->detail), "multiview skipped");
	return e;
}

enum result test_rta(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t w = OPT(o->width, 64), h = OPT(o->height, 64), seed = OPT(o->seed, 1);
	uint32_t frames = OPT(o->frames, 2);
	struct img im, dim;
	struct buf rb, db;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule fvs, cfs;
	VkPipelineLayout clayout, layout = VK_NULL_HANDLE;
	VkPipeline cpipe, pipe = VK_NULL_HANDLE;
	bool draw = v->has_output_layer;
	struct rp_desc rd = { .color = FMT, .depth = DFMT };
	struct layer_expect last[L];
	VkFormat dsf = VK_FORMAT_UNDEFINED;
	enum result st, d3, mvr;

	if (w < 8 || h < 8)
		return R_SETUP;
	CHECK(img_create_layers(v, &im, w, h, L, FMT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(img_create_layers(v, &dim, w, h, L, DFMT,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)w * h * 4 * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(buf_create(v, &db, (VkDeviceSize)w * h * 4 * L, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_build(v, &rp, &rd));
	VkImageView views[2] = { im.view, dim.view };

	CHECK(fb_create_views(v, &fb, rp, views, 2, w, h, L));

	/* the draw without gl_Layer: layer 0 only */
	CHECK(shader_create(v, &fvs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &cfs, spv_color_frag, sizeof(spv_color_frag)));
	CHECK(layout_create(v, &clayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct color_pc),
			    nullptr));
	struct gfx_desc cd = { .rp = rp, .layout = clayout, .vs = fvs, .fs = cfs };

	CHECK(gfx_pipeline(v, &cpipe, &cd));
	if (draw) {
		VkShaderModule vs, fs;

		CHECK(shader_create(v, &vs, spv_layer_vert, sizeof(spv_layer_vert)));
		CHECK(shader_create(v, &fs, spv_layer_frag, sizeof(spv_layer_frag)));
		CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t),
				    nullptr));
		struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };

		CHECK(gfx_pipeline(v, &pipe, &d));
	}

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 101u;
		uint32_t base = layer_color(99, s);
		struct layer_expect ce[L];
		float de[L];
		VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
		VkRect2D full = { { 0, 0 }, { w, h } };

		for (uint32_t pass = 0; pass < (draw ? 2u : 1u); pass++) {
			VkClearValue clear[2] = { clear_rgba(base), { .depthStencil = { 1.0f, 0 } } };
			VkRenderPassBeginInfo rbi = {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = rp,
				.framebuffer = fb,
				.renderArea = full,
				.clearValueCount = 2,
				.pClearValues = clear,
			};
			VkCommandBuffer cb;

			for (uint32_t l = 0; l < L; l++) {
				ce[l] = (struct layer_expect){ .c = base };
				de[l] = 1.0f;
			}
			CHECK(cmd_begin(v, &cb));
			vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &full);
			if (pass == 0) {
				struct color_pc pc = { 0, 1, s };

				/* a draw without gl_Layer goes to layer 0 */
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
				vkCmdPushConstants(cb, clayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
						   sizeof(pc), &pc);
				vkCmdDraw(cb, 3, 1, 0, 0);
				ce[0] = (struct layer_expect){ .lcg = true, .seed = s };
				clears_odd_and_multi(cb, ce, w, h, s);
				/* depth: layer l to l / 8 */
				for (uint32_t l = 1; l < L; l++) {
					VkClearAttachment da = {
						.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
						.clearValue = { .depthStencil = { (float)l / 8.0f, 0 } },
					};
					VkClearRect dr = { full, l, 1 };

					vkCmdClearAttachments(cb, 1, &da, 1, &dr);
					de[l] = (float)l / 8.0f;
				}
			} else {
				/* 2. one instance per layer, routed with gl_Layer */
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
						   sizeof(s), &s);
				vkCmdDraw(cb, 3, L, 0, 0);
				for (uint32_t l = 0; l < L; l++)
					ce[l] = (struct layer_expect){ .c = layer_color(l, s) };
			}
			vkCmdEndRenderPass(cb);
			img_barrier(cb, &dim, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				    VK_ACCESS_TRANSFER_READ_BIT,
				    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT);
			for (uint32_t l = 0; l < L; l++) {
				VkDeviceSize off = (VkDeviceSize)l * w * h * 4;

				cmd_copy_layer_to_buffer(cb, &im, l, &rb, off);
				cmd_copy_layer_to_buffer(cb, &dim, l, &db, off);
			}
			CHECK(cmd_submit_wait(v, cb));
			CHECK(check_layers(v, r, pass ? "draw" : "clear", &im, &rb, &db, ce, de));
			memcpy(last, ce, sizeof(last));
		}
		r->frames++;
	}
	/* steps 3-5 all run; the first failure is reported */
	enum result sub = rta_subset(v, r, &im, &rb, last, seed), bad = R_PASS;
	char detail[sizeof(r->detail)] = "";

	if (sub != R_PASS) {
		bad = sub;
		memcpy(detail, r->detail, sizeof(detail));
	}
	st = rta_stencil(v, r, w, h, &dsf);
	if (st != R_PASS && st != R_SKIP && bad == R_PASS) {
		bad = st;
		memcpy(detail, r->detail, sizeof(detail));
	}
	d3 = rta_3d(v, r, w, h, seed);
	if (d3 != R_PASS && d3 != R_SKIP && bad == R_PASS) {
		bad = d3;
		memcpy(detail, r->detail, sizeof(detail));
	}
	mvr = run_fresh(o, rta_multiview_fresh, r);
	if (mvr != R_PASS && mvr != R_SKIP && bad == R_PASS) {
		bad = mvr;
		memcpy(detail, r->detail, sizeof(detail));
	}
	if (bad != R_PASS) {
		memcpy(r->detail, detail, sizeof(detail));
		return bad;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "%ux%u x %u: colour + D32, draw, clears%s, %u frames; layers 1+5 only; "
		 "stencil %s; 3D image %s; multiview %s",
		 w, h, L, draw ? ", gl_Layer draw" : " (no gl_Layer)", frames,
		 st == R_SKIP ? "skipped" : dsf == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
		 d3 == R_SKIP ? "skipped" : "as 2D array", mvr == R_SKIP ? "skipped" : "6 views");
	return R_PASS;
}
