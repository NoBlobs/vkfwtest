// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * formats: render targets other than RGBA8, plus blending and discard.
 * Each case draws a full-screen constant colour chosen to be exactly
 * representable and checks the raw bytes:
 *  R5G6B5_UNORM_PACK16, B8G8R8A8_UNORM, R8G8B8A8_SRGB (+-1),
 *  R16G16B16A16_SFLOAT, R32_UINT (integer output),
 *  RGBA8 with additive blending (saturating) and RGBA8 with discard
 *  (checkerboard of drawn and cleared pixels).
 * A format the device cannot render to is skipped and listed.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "fullscreen.vert.h"
#include "fmt.frag.h"
#include "fmtu.frag.h"

constexpr uint32_t W = 48;
constexpr uint32_t H = 32;

enum fcase { F_565, F_BGRA, F_SRGB, F_F16, F_U32, F_BLEND, F_DISCARD, F_COUNT };

static const struct {
	const char *name;
	VkFormat fmt;
	uint32_t bpp;
} cases[F_COUNT] = {
	[F_565] = { "R5G6B5", VK_FORMAT_R5G6B5_UNORM_PACK16, 2 },
	[F_BGRA] = { "B8G8R8A8", VK_FORMAT_B8G8R8A8_UNORM, 4 },
	[F_SRGB] = { "R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, 4 },
	[F_F16] = { "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, 8 },
	[F_U32] = { "R32_UINT", VK_FORMAT_R32_UINT, 4 },
	[F_BLEND] = { "RGBA8 additive blend", VK_FORMAT_R8G8B8A8_UNORM, 4 },
	[F_DISCARD] = { "RGBA8 discard", VK_FORMAT_R8G8B8A8_UNORM, 4 },
};

struct fmt_pc {
	float color[4];
	uint32_t mode;
};

static uint8_t srgb_encode(float lin)
{
	double s = lin <= 0.0031308 ? lin * 12.92 : 1.055 * pow(lin, 1.0 / 2.4) - 0.055;

	return (uint8_t)lround(s * 255.0);
}

/* The expected bytes of pixel (x, y), and the per-byte tolerance. */
static void expect_px(enum fcase c, uint32_t x, uint32_t y, uint8_t *out, uint32_t *tol)
{
	*tol = 0;
	switch (c) {
	case F_565: {
		/* r = 21/31, g = 40/63, b = 9/31 */
		uint16_t v = (uint16_t)(21u << 11 | 40u << 5 | 9u);

		memcpy(out, &v, 2);     /* packed formats: host byte order */
		break;
	}
	case F_BGRA:
		out[0] = 0x30, out[1] = 0x20, out[2] = 0x10, out[3] = 0xFF;   /* rgba 10 20 30 ff */
		break;
	case F_SRGB:
		out[0] = srgb_encode(0.5f), out[1] = srgb_encode(0.2f);
		out[2] = srgb_encode(1.0f), out[3] = 0x80;      /* alpha stays linear */
		*tol = 1;
		break;
	case F_F16: {
		const uint16_t h[4] = { 0x3800, 0xC000, 0x6400, 0x2000 };   /* 0.5 -2 1024 2^-7 */

		memcpy(out, h, 8);
		break;
	}
	case F_U32: {
		uint32_t v = 0xDEADBEEFu;

		memcpy(out, &v, 4);
		break;
	}
	case F_BLEND:
		/* clear (100, 30, 200, 10) + colour (100, 250, 10, 50), saturating */
		out[0] = 200, out[1] = 255, out[2] = 210, out[3] = 60;
		break;
	case F_DISCARD:
		if ((x + y) & 1)
			out[0] = 0, out[1] = 0, out[2] = 0, out[3] = 0;
		else
			out[0] = 0x11, out[1] = 0x22, out[2] = 0x33, out[3] = 0x44;
		break;
	case F_COUNT:
		break;
	}
}

static enum result run_case(struct vk *v, struct report *r, enum fcase c, VkShaderModule vs,
			    VkShaderModule fs, VkShaderModule fsu, uint32_t f)
{
	VkFormat fmt = cases[c].fmt;
	uint32_t bpp = cases[c].bpp;
	struct img color;
	struct buf rb;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	bool integer = c == F_U32;

	CHECK(img_create(v, &color, W, H, fmt,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &rb, (VkDeviceSize)W * H * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
	CHECK(rp_create(v, &rp, fmt, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT,
			    integer ? 16 : sizeof(struct fmt_pc), nullptr));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = integer ? fsu : fs,
			      .blend_add = c == F_BLEND };
	CHECK(gfx_pipeline(v, &pipe, &d));

	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	struct fmt_pc pc = { { 0, 0, 0, 0 }, 0 };
	uint32_t upc[4] = { 0xDEADBEEFu, 1, 2, 3 };

	switch (c) {
	case F_565: pc = (struct fmt_pc){ { 21 / 31.0f, 40 / 63.0f, 9 / 31.0f, 1 }, 0 }; break;
	case F_BGRA: pc = (struct fmt_pc){ { 0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 1 }, 0 };
		break;
	case F_SRGB: pc = (struct fmt_pc){ { 0.5f, 0.2f, 1.0f, 128 / 255.0f }, 0 }; break;
	case F_F16: pc = (struct fmt_pc){ { 0.5f, -2.0f, 1024.0f, 0.0078125f }, 0 }; break;
	case F_BLEND:
		clear.color = (VkClearColorValue){ .float32 = { 100 / 255.0f, 30 / 255.0f,
								200 / 255.0f, 10 / 255.0f } };
		pc = (struct fmt_pc){ { 100 / 255.0f, 250 / 255.0f, 10 / 255.0f, 50 / 255.0f }, 0 };
		break;
	case F_DISCARD:
		pc = (struct fmt_pc){ { 0x11 / 255.0f, 0x22 / 255.0f, 0x33 / 255.0f, 0x44 / 255.0f },
				      1 };
		break;
	default:
		break;
	}
	if (integer)
		clear.color = (VkClearColorValue){ .uint32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	if (integer)
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(upc), upc);
	else
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	cmd_copy_to_buffer(cb, &color, &rb);
	CHECK(cmd_submit_wait(v, cb));

	uint64_t bad = 0;

	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			const uint8_t *p = rb.map + ((size_t)y * W + x) * bpp;
			uint8_t e[8];
			uint32_t tol;
			bool ok = true;

			expect_px(c, x, y, e, &tol);
			for (uint32_t i = 0; i < bpp; i++)
				ok &= (uint32_t)(p[i] > e[i] ? p[i] - e[i] : e[i] - p[i]) <= tol;
			if (ok)
				continue;
			if (bad < 4 && v->verbose >= 0) {
				fprintf(stderr, "vkfwtest: frame %u %s (%u,%u): got", f, cases[c].name,
					x, y);
				for (uint32_t i = 0; i < bpp; i++)
					fprintf(stderr, " %02x", p[i]);
				fprintf(stderr, ", expected");
				for (uint32_t i = 0; i < bpp; i++)
					fprintf(stderr, " %02x", e[i]);
				fprintf(stderr, "\n");
			}
			bad++;
		}
	r->checked += (uint64_t)W * H;
	r->mismatches += bad;
	if (bad) {
		snprintf(r->detail, sizeof(r->detail), "frame %u %s: %" PRIu64 " of %u pixels wrong",
			 f, cases[c].name, bad, W * H);
		return R_MISMATCH;
	}
	return R_PASS;
}

enum result test_formats(struct vk *v, const struct opts *o, struct report *r)
{
	uint32_t frames = OPT(o->frames, 1);
	VkShaderModule vs, fs, fsu;
	char skipped[96] = "";
	uint32_t run = 0;

	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_fmt_frag, sizeof(spv_fmt_frag)));
	CHECK(shader_create(v, &fsu, spv_fmtu_frag, sizeof(spv_fmtu_frag)));
	for (uint32_t f = 0; f < frames; f++) {
		run = 0;
		skipped[0] = '\0';
		for (enum fcase c = 0; c < F_COUNT; c++) {
			VkFormatProperties fp;
			VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
						    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

			if (c == F_BLEND)
				need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
			vkGetPhysicalDeviceFormatProperties(v->phys, cases[c].fmt, &fp);
			if ((fp.optimalTilingFeatures & need) != need) {
				size_t n = strlen(skipped);

				snprintf(skipped + n, sizeof(skipped) - n, "%s%s", n ? ", " : "",
					 cases[c].name);
				continue;
			}
			CHECK(run_case(v, r, c, vs, fs, fsu, f));
			run++;
		}
		r->frames++;
	}
	snprintf(r->detail, sizeof(r->detail), "%u cases pass%s%s; %u rounds", run,
		 skipped[0] ? "; skipped: " : "", skipped, frames);
	return R_PASS;
}
