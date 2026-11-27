// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * texture: sampling through the texture unit. A 37x23 (NPOT) texture is
 * drawn into a 64x64 target, shifted so that coordinates fall outside it on
 * every side, with:
 *  - NEAREST and REPEAT, MIRRORED_REPEAT, CLAMP_TO_EDGE, CLAMP_TO_BORDER
 *    (opaque white, transparent black): exact texels or border colours;
 *  - LINEAR halfway between two texels horizontally: their average (+-1);
 *  - a 32x32 texture with 6 mip levels of distinct solid colours, sampled at
 *    LOD 0..5.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "tex.frag.h"

constexpr VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t TW = 37;
constexpr uint32_t TH = 23;
constexpr uint32_t OUT = 64;
constexpr int32_t OX = -13;                     /* texel (0,0) lands on pixel (13,11) */
constexpr int32_t OY = -11;

struct tex_pc {
	float off[2], size[2], lod;
};

/* texel colours: even channel values, so a two-texel average is exact */
static uint32_t texel(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t h = (x * 2654435761u) ^ (y * 40503u + 0x9E37u) ^ seed;

	h ^= h >> 13;
	return h & 0xFEFEFEFEu;
}

enum wrap { W_REPEAT, W_MIRROR, W_CLAMP, W_BORDER };

/* Texel index after the address mode, or -1 for the border. */
static int64_t wrap(int64_t i, int64_t n, enum wrap m)
{
	switch (m) {
	case W_REPEAT:
		return ((i % n) + n) % n;
	case W_MIRROR: {
		int64_t t = ((i % (2 * n)) + 2 * n) % (2 * n);

		return t < n ? t : 2 * n - 1 - t;
	}
	case W_CLAMP:
		return i < 0 ? 0 : i >= n ? n - 1 : i;
	case W_BORDER:
		return i < 0 || i >= n ? -1 : i;
	}
	return 0;
}

struct tex_case {
	const char *name;
	enum wrap mode;
	VkBorderColor border;
	bool linear;
	uint32_t seed;
};

static struct rgba border_rgba(VkBorderColor b)
{
	return b == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? (struct rgba){ 255, 255, 255, 255 } :
	       b == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ? (struct rgba){ 0, 0, 0, 255 } :
							 (struct rgba){ 0, 0, 0, 0 };
}

static struct rgba sample_texel(const struct tex_case *c, int64_t x, int64_t y)
{
	int64_t tx = wrap(x, TW, c->mode), ty = wrap(y, TH, c->mode);

	if (tx < 0 || ty < 0)
		return border_rgba(c->border);
	return rgba_of(texel((uint32_t)tx, (uint32_t)ty, c->seed));
}

static struct rgba tex_expect(uint32_t x, uint32_t y, const void *ctx)
{
	const struct tex_case *c = ctx;
	int64_t i = (int64_t)x - OX, j = (int64_t)y - OY;

	if (!c->linear)
		return sample_texel(c, i, j);
	/* halfway between texels i and i + 1 of row j */
	struct rgba a = sample_texel(c, i, j), b = sample_texel(c, i + 1, j);

	return (struct rgba){ (uint8_t)((a.r + b.r) / 2), (uint8_t)((a.g + b.g) / 2),
			      (uint8_t)((a.b + b.b) / 2), (uint8_t)((a.a + b.a) / 2) };
}

static uint32_t mip_color(uint32_t level, uint32_t seed)
{
	return (lcg(level + seed, 7) & 0x00FFFFFFu) | 0xFF000000u;
}

static struct rgba mip_expect(uint32_t x, uint32_t y, const void *ctx)
{
	(void)x;
	(void)y;
	return rgba_of(*(const uint32_t *)ctx);
}

/* Upload `levels` mip levels from the host buffer (level after level). */
static enum result upload(struct vk *v, const struct img *im, uint32_t levels, const struct buf *b)
{
	VkCommandBuffer cb;
	VkBufferImageCopy c[8];
	VkDeviceSize off = 0;
	VkImageMemoryBarrier ib = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = im->i,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
	};

	for (uint32_t l = 0; l < levels; l++) {
		uint32_t w = im->w >> l ? im->w >> l : 1, h = im->h >> l ? im->h >> l : 1;

		c[l] = (VkBufferImageCopy){
			.bufferOffset = off,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 },
			.imageExtent = { w, h, 1 },
		};
		off += (VkDeviceSize)w * h * 4;
	}
	CHECK(cmd_begin(v, &cb));
	ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			     0, nullptr, 0, nullptr, 1, &ib);
	vkCmdCopyBufferToImage(cb, b->b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, c);
	ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
	return cmd_submit_wait(v, cb);
}

/* A sampled image with `levels` mip levels (img_create makes single-level ones). */
static enum result tex_image(struct vk *v, struct img *im, uint32_t w, uint32_t h,
			     uint32_t levels)
{
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FMT,
		.extent = { w, h, 1 },
		.mipLevels = levels,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements req;

	memset(im, 0, sizeof(*im));
	im->fmt = FMT;
	im->w = w;
	im->h = h;
	im->layers = 1;
	im->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	VK_CHECK(v, vkCreateImage(v->dev, &ici, nullptr, &im->i));
	vk_track(v, OBJ_IMAGE, H2U(im->i));
	vkGetImageMemoryRequirements(v->dev, im->i, &req);
	/* reuse the buffer allocator's memory type choice through a dummy buffer is
	 * not needed: any device-local type in the mask works */
	uint32_t t = 0;

	while (t < v->mem.memoryTypeCount &&
	       !((req.memoryTypeBits & (1u << t)) &&
		 (v->mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
		t++;
	if (t == v->mem.memoryTypeCount)
		for (t = 0; t < v->mem.memoryTypeCount && !(req.memoryTypeBits & (1u << t)); t++)
			;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = t,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, &im->m));
	vk_track(v, OBJ_MEMORY, H2U(im->m));
	VK_CHECK(v, vkBindImageMemory(v->dev, im->i, im->m, 0));
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = im->i,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = FMT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
	};
	VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &im->view));
	vk_track(v, OBJ_VIEW, H2U(im->view));
	return R_PASS;
}

static enum result draw(struct vk *v, VkRenderPass rp, VkFramebuffer fb, VkPipeline pipe,
			VkPipelineLayout layout, VkDescriptorSet set, const struct tex_pc *pc,
			const struct img *color, const struct buf *rb)
{
	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { OUT, OUT } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, OUT, OUT, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { OUT, OUT } };
	VkCommandBuffer cb;

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*pc), pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, color, rb);
	return cmd_submit_wait(v, cb);
}

static void bind_texture(struct vk *v, VkDescriptorSet set, VkSampler s, const struct img *im)
{
	VkDescriptorImageInfo ii = { s, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &ii,
	};

	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
}

enum result test_texture(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t seed = OPT(o->seed, 1), frames = OPT(o->frames, 1);
	struct img color, tex, mip;
	struct buf rb, up;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkDescriptorSetLayoutBinding b = {
		0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
	};

	CHECK(img_create(v, &color, OUT, OUT, FMT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, OUT * OUT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, FMT, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(desc_create(v, &b, 1, &dsl, &set));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_tex_frag, sizeof(spv_tex_frag)));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(struct tex_pc), &dsl));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));
	CHECK(tex_image(v, &tex, TW, TH, 1));
	CHECK(tex_image(v, &mip, 32, 32, 6));
	CHECK(buf_create(v, &up, 32 * 32 * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));

	for (uint32_t f = 0; f < frames; f++) {
		uint32_t s = seed + f * 17u;

		/* the NPOT texture, texel by texel */
		for (uint32_t y = 0; y < TH; y++)
			for (uint32_t x = 0; x < TW; x++) {
				struct rgba t = rgba_of(texel(x, y, s));
				uint8_t *q = up.map + ((size_t)y * TW + x) * 4;

				q[0] = t.r;
				q[1] = t.g;
				q[2] = t.b;
				q[3] = t.a;
			}
		CHECK(upload(v, &tex, 1, &up));

		static const struct { const char *name; enum wrap mode; VkSamplerAddressMode am;
				      VkBorderColor border; bool linear; } cases[] = {
			{ "nearest repeat", W_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, false },
			{ "nearest mirrored", W_MIRROR, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, 0, false },
			{ "nearest clamp", W_CLAMP, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, false },
			{ "border white", W_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			  VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, false },
			{ "border transparent", W_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			  VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, false },
			{ "linear repeat", W_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, true },
		};
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			VkSamplerCreateInfo sci = {
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = cases[i].linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
				.minFilter = cases[i].linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
				.addressModeU = cases[i].am,
				.addressModeV = cases[i].am,
				.addressModeW = cases[i].am,
				.borderColor = cases[i].border,
			};
			struct tex_case c = { cases[i].name, cases[i].mode, cases[i].border,
					      cases[i].linear, s };
			/* NEAREST: texel centres; LINEAR: half a texel to the right */
			struct tex_pc pc = { { (float)OX - (c.linear ? 0.5f : 0.0f), (float)OY },
					     { TW, TH }, 0.0f };
			VkSampler smp;
			char what[48];

			CHECK(sampler_create(v, &sci, &smp));
			bind_texture(v, set, smp, &tex);
			CHECK(draw(v, rp, fb, pipe, layout, set, &pc, &color, &rb));
			snprintf(what, sizeof(what), "frame %u %s", f, c.name);
			CHECK(check_rgba_tol(v, r, what, rb.map, OUT, OUT, c.linear ? 1 : 0, tex_expect,
					     &c));
		}

		/* mip levels: 32x32 .. 1x1, one colour each */
		VkDeviceSize off = 0;

		for (uint32_t l = 0; l < 6; l++) {
			uint32_t n = (32u >> l) * (32u >> l);
			struct rgba c = rgba_of(mip_color(l, s));

			for (uint32_t k = 0; k < n; k++) {
				up.map[off + k * 4 + 0] = c.r;
				up.map[off + k * 4 + 1] = c.g;
				up.map[off + k * 4 + 2] = c.b;
				up.map[off + k * 4 + 3] = c.a;
			}
			off += (VkDeviceSize)n * 4;
		}
		CHECK(upload(v, &mip, 6, &up));
		VkSamplerCreateInfo msci = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.maxLod = 6.0f,
		};
		VkSampler msmp;

		CHECK(sampler_create(v, &msci, &msmp));
		bind_texture(v, set, msmp, &mip);
		for (uint32_t l = 0; l < 6; l++) {
			struct tex_pc pc = { { 0, 0 }, { 32, 32 }, (float)l };
			uint32_t want = mip_color(l, s);
			char what[48];

			CHECK(draw(v, rp, fb, pipe, layout, set, &pc, &color, &rb));
			snprintf(what, sizeof(what), "frame %u mip level %u", f, l);
			CHECK(check_rgba(v, r, what, rb.map, OUT, OUT, mip_expect, &want));
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "37x23 NPOT: 4 address modes + 2 borders + linear; 6 mip levels; %u rounds", frames);
	return R_PASS;
}
