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
