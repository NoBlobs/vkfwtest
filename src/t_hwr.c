// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * hwr-lockup, hwr-fault (oracle stage E): a controlled GPU failure and the
 * recovery after it.
 *  hwr-lockup: a compute shader spins while a buffer word is 1, until the
 *              firmware's watchdog fires (about 30 s on the K1);
 *  hwr-fault:  a compute shader writes and reads gigabytes beyond its storage
 *              buffer (no robust buffer access): an MMU page fault.
 * The guilty submission must end in VK_ERROR_DEVICE_LOST. Then, without
 * reloading the driver, fresh devices must run multiframe and pre-cdm.
 * Disruptive, so `all` does not include them.
 */
#include <stdio.h>
#include <string.h>

#include "vkfwtest.h"

#include "hang.comp.h"
#include "fault.comp.h"
#include "fullscreen.vert.h"
#include "hangfrag.frag.h"
#include "faultfrag.frag.h"

static int sev(enum result r)
{
	return r == R_LOST ? 5 : r == R_TIMEOUT ? 4 : r == R_MISMATCH ? 3 : r == R_SETUP ? 2 : 0;
}

/* Run one test on a new device with the test's own defaults. */
static enum result after(const struct opts *o, struct report *r, test_fn fn)
{
	struct opts o2 = *o;

	o2.frames = 0;
	o2.width = o2.height = 0;
	o2.iters = 0;
	return run_fresh(&o2, fn, r);
}

static enum result hwr_run(struct vk *v, const struct opts *o, struct report *r, bool lockup)
{
	struct buf b;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorSet set;
	VkCommandBuffer cb;
	VkFence fence;
	enum result res;
	uint32_t index = 0x3FFFFFF0u;           /* about 4 GiB past the buffer */
	const char *what = lockup ? "lockup" : "page fault";

	LOG(v, 0, "vkfwtest: hwr-%s: causing a GPU %s on purpose\n", lockup ? "lockup" : "fault",
	    what);
	CHECK(buf_create(v, &b, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(b.map, 0, 64);
	b.map[0] = 1;           /* data[0] = 1 (little-endian word) */
	if (lockup)
		CHECK(compute_pipeline(v, spv_hang_comp, sizeof(spv_hang_comp), 0, &b, &layout, &pipe,
				       &set));
	else
		CHECK(compute_pipeline(v, spv_fault_comp, sizeof(spv_fault_comp), sizeof(index), &b,
				       &layout, &pipe, &set));
	CHECK(cmd_begin(v, &cb));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	if (!lockup)
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(index), &index);
	vkCmdDispatch(cb, 1, 1, 1);
	CHECK(cmd_submit_async(v, cb, &fence));

	double t0 = now_ms();
	int saved = v->verbose;

	v->verbose = -1;        /* the expected failure is not an error */
	res = fence_wait(v, fence, (uint64_t)OPT(o->timeout_s, 120) * 1000000000ull);
	v->verbose = saved;

	double ms = now_ms() - t0;

	if (res == R_PASS && lockup && ms < 2000.0) {
		snprintf(r->detail, sizeof(r->detail),
			 "the spinning dispatch completed in %.0f ms: no lockup to recover", ms);
		return R_MISMATCH;
	}
	if (res == R_TIMEOUT) {
		snprintf(r->detail, sizeof(r->detail),
			 "no recovery: the %s was still running after %.0f s", what, ms / 1e3);
		return R_TIMEOUT;
	}
	if (res != R_PASS && res != R_LOST)
		return res;

	/*
	 * Recovered, or (fault case) maybe no fault at all. Stage E wants
	 * VK_ERROR_DEVICE_LOST for the guilty device. Check the recovery anyway.
	 */
	bool lost = res == R_LOST;

	LOG(v, 0, "vkfwtest: hwr: %s after %.1f s\n",
	    lost ? "device lost" : "fence signalled without device lost", ms / 1e3);
	v->lost = true;
	vk_fini(v);             /* destroy the guilty device before the next ones */

	enum result mf = after(o, r, test_multiframe);
	enum result cdm = after(o, r, test_precdm);

	snprintf(r->detail, sizeof(r->detail),
		 "%s: %s after %.1f s; then on new devices: multiframe %s, pre-cdm %s", what,
		 lost ? "device lost" :
		 lockup ? "NO device lost (fence signalled)" :
			  "NO device lost (no fault raised, or not reported)",
		 ms / 1e3, result_name(mf), result_name(cdm));
	res = sev(mf) >= sev(cdm) ? mf : cdm;
	if (res == R_PASS && !lost)
		res = R_MISMATCH;
	return res;
}

enum result test_hwr_lockup(struct vk *v, const struct opts *o, struct report *r)
{
	return hwr_run(v, o, r, true);
}

enum result test_hwr_fault(struct vk *v, const struct opts *o, struct report *r)
{
	return hwr_run(v, o, r, false);
}

enum result test_hwr_frag(struct vk *v, const struct opts *o, struct report *r)
{
	constexpr uint32_t W = 64;
	constexpr uint32_t H = 64;
	struct img color;
	struct buf ubo;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	VkFence fence;
	enum result res;

	LOG(v, 0, "vkfwtest: hwr-frag: causing a fragment-shader lockup on purpose\n");
	CHECK(img_create(v, &color, W, H, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &ubo, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true));
	memset(ubo.map, 0, 16);
	ubo.map[0] = 1;         /* spin = 1 (little-endian) */
	CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_hangfrag_frag, sizeof(spv_hangfrag_frag)));
	VkDescriptorSetLayoutBinding bind = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	CHECK(desc_create(v, &bind, 1, &dsl, &set));
	VkDescriptorBufferInfo dbi = { ubo.b, 0, 16 };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
	CHECK(layout_create(v, &layout, 0, 0, &dsl));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	CHECK(cmd_submit_async(v, cb, &fence));

	double t0 = now_ms();
	int saved = v->verbose;

	v->verbose = -1;
	res = fence_wait(v, fence, (uint64_t)OPT(o->timeout_s, 120) * 1000000000ull);
	v->verbose = saved;

	double ms = now_ms() - t0;

	if (res == R_PASS && ms < 2000.0) {
		snprintf(r->detail, sizeof(r->detail),
			 "the spinning fragment draw completed in %.0f ms: no lockup to recover",
			 ms);
		return R_MISMATCH;
	}
	if (res == R_TIMEOUT) {
		snprintf(r->detail, sizeof(r->detail),
			 "no recovery: fragment lockup still running after %.0f s", ms / 1e3);
		return R_TIMEOUT;
	}
	if (res != R_PASS && res != R_LOST)
		return res;

	bool lost = res == R_LOST;

	v->lost = true;
	vk_fini(v);

	enum result mf = after(o, r, test_multiframe);
	enum result cdm = after(o, r, test_precdm);

	snprintf(r->detail, sizeof(r->detail),
		 "frag lockup: %s after %.1f s; then on new devices: multiframe %s, pre-cdm %s",
		 lost ? "device lost" : "NO device lost (fence signalled)", ms / 1e3,
		 result_name(mf), result_name(cdm));
	res = sev(mf) >= sev(cdm) ? mf : cdm;
	if (res == R_PASS && !lost)
		res = R_MISMATCH;
	return res;
}

enum result test_hwr_3d_fault(struct vk *v, const struct opts *o, struct report *r)
{
	constexpr uint32_t W = 64;
	constexpr uint32_t H = 64;
	struct img color;
	struct buf ssbo;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkShaderModule vs, fs;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkCommandBuffer cb;
	VkFence fence;
	enum result res;
	uint32_t index = 0x3FFFFFF0u;

	if (!v->feat.fragmentStoresAndAtomics) {
		snprintf(r->detail, sizeof(r->detail), "fragmentStoresAndAtomics not supported");
		return R_SKIP;
	}
	LOG(v, 0, "vkfwtest: hwr-3d-fault: causing a 3D-pipe MMU page fault on purpose\n");
	CHECK(img_create(v, &color, W, H, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			 VK_SAMPLE_COUNT_1_BIT));
	CHECK(buf_create(v, &ssbo, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
	memset(ssbo.map, 0, 64);
	CHECK(rp_create(v, &rp, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED));
	CHECK(fb_create(v, &fb, rp, &color, nullptr));
	CHECK(shader_create(v, &vs, spv_fullscreen_vert, sizeof(spv_fullscreen_vert)));
	CHECK(shader_create(v, &fs, spv_faultfrag_frag, sizeof(spv_faultfrag_frag)));
	VkDescriptorSetLayoutBinding bind = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	CHECK(desc_create(v, &bind, 1, &dsl, &set));
	VkDescriptorBufferInfo dbi = { ssbo.b, 0, 64 };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
	CHECK(layout_create(v, &layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(index), &dsl));
	struct gfx_desc d = { .rp = rp, .layout = layout, .vs = vs, .fs = fs };
	CHECK(gfx_pipeline(v, &pipe, &d));

	VkClearValue clear = { .color.float32 = { 0, 0, 0, 0 } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };

	CHECK(cmd_begin(v, &cb));
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(index), &index);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	CHECK(cmd_submit_async(v, cb, &fence));

	double t0 = now_ms();
	int saved = v->verbose;

	v->verbose = -1;
	res = fence_wait(v, fence, (uint64_t)OPT(o->timeout_s, 120) * 1000000000ull);
	v->verbose = saved;

	double ms = now_ms() - t0;

	if (res == R_TIMEOUT) {
		snprintf(r->detail, sizeof(r->detail),
			 "no recovery: 3D fault still running after %.0f s", ms / 1e3);
		return R_TIMEOUT;
	}
	if (res != R_PASS && res != R_LOST)
		return res;

	bool lost = res == R_LOST;

	v->lost = true;
	vk_fini(v);

	enum result mf = after(o, r, test_multiframe);
	enum result cdm = after(o, r, test_precdm);

	snprintf(r->detail, sizeof(r->detail),
		 "3D fault: %s after %.1f s; then on new devices: multiframe %s, pre-cdm %s",
		 lost ? "device lost" : "NO device lost (no fault raised, or not reported)",
		 ms / 1e3, result_name(mf), result_name(cdm));
	res = sev(mf) >= sev(cdm) ? mf : cdm;
	if (res == R_PASS && !lost)
		res = R_MISMATCH;
	return res;
}
