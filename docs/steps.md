<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# vkfwtest — what is left to implement

State on 2026-11-27 (commit `029c4bc`):
- 25 tests;
- all 19 tests of `all` pass on a desktop GPU (NVIDIA GTX 1060);
- on the BXE-2-32 board, the failures point at firmware bugs, not at the harness.

Items are ordered by how much they help firmware work. Each new test keeps the rules in
[Writing a test](writing-tests.rst):
- headless;
- exact expected values computed on the CPU;
- a default that runs in seconds;
- `R_SKIP` when the device lacks a feature.

## 1. Debugging aids

- [ ] `--dump DIR`: on a mismatch, write the image read back and the expected image (PPM,
      no library) and a diff mask. The first 8 wrong values on stderr are not enough to
      see a missing tile or a shifted layer.
- [ ] A summary at the end of a run: one JSON object with every result, so CI does not
      have to collect the lines.
- [ ] Select tests by group: `--group render|compute|transfer|stress|hwr|prio`. Also
      `vkfwtest list --json`.
- [ ] Per-test time limit (`--test-timeout`), so a test whose fences keep signalling but
      never finishes cannot hold up the run.
- [ ] `--keep-device`: run several tests on one device, to reproduce firmware bugs that
      need earlier renders on the same context (the "second render" family).

## 2. Power

- [ ] `idle-resume`: render; wait longer than the runtime-PM autosuspend (read
      `/sys/.../power/autosuspend_delay_ms` when available, else a `--idle` time); render
      again on the same device. This is the warm-boot path from inside the harness;
      today only the oracle's stage D covers it.

## 3. Shader resources and compute

- [ ] Uniform buffers (std140), dynamic uniform/storage offsets, push descriptors when
      supported.
- [ ] Storage images: `imageLoad`/`imageStore` in compute and fragment shaders.
- [ ] Texel buffers (uniform and storage).
- [ ] Atomics on buffers and images; shared memory with `barrier()`.
- [ ] Workgroups at `maxComputeWorkGroupSize`/`Invocations`; 2D/3D dispatches and
      `vkCmdDispatchBase`.
- [ ] Subgroup operations (basic, vote, ballot) when supported.

## 4. Transfers

- [ ] `vkCmdCopyBuffer` (overlapping regions, odd sizes), `vkCmdFillBuffer`,
      `vkCmdUpdateBuffer`.
- [ ] Depth/stencil: `vkCmdClearDepthStencilImage`, depth copies image↔buffer.
- [ ] Scaled blits with `VK_FILTER_LINEAR`; mip-chain generation with blits.
- [ ] 3D images and multi-layer / multi-level copies.
- [ ] Copies between images of different (compatible) formats.

## 5. Textures

- [ ] Cube maps and cube arrays; 3D textures; 2D array textures with a layer index.
- [ ] Depth-compare samplers (shadow), `textureGather`, `texelFetch`, unnormalized
      coordinates.
- [ ] Anisotropic filtering (presence only, the exact values are implementation defined).
- [ ] Compressed formats the device exposes (ETC2, ASTC LDR): upload known blocks and
      check the decoded texels.
- [ ] Integer and float textures (R32_UINT, RGBA16F) sampled with NEAREST.

## 6. Render passes and rasterization

- [ ] Secondary command buffers (`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`).
- [ ] Multiview (`VK_KHR_multiview`, core 1.1) and dynamic rendering (1.3 / extension).
- [ ] `vkCmdClearAttachments` with partial rectangles, inside a render pass with draws
      before and after.
- [ ] Depth bias, depth clamp, depth bounds; viewport with non-zero origin, negative
      height and min/max depth.
- [ ] Early fragment tests, sample shading, alpha-to-coverage, sample masks.
- [ ] MSAA 2× and 8× where supported; depth/stencil resolve (1.2 / extension).
- [ ] Load `DONT_CARE` / store `DONT_CARE` combinations; render passes with no
      attachments.
- [ ] Wide lines and large points (features), line strips, triangle strips and fans,
      primitive restart.

## 7. Synchronisation

- [ ] More than one queue when the family offers it; submissions that depend on each
      other through binary semaphores.
- [ ] Timeline semaphores waited and signalled across submissions and from the host.
- [ ] `vkCmdSetEvent` / `vkCmdWaitEvents` inside command buffers; host-set events.
- [ ] External fences / `sync_file` export and import when supported.
- [ ] Many small submissions in a tight loop (fence throughput).

## 8. Robustness and failures

- [ ] `robustBufferAccess`: out-of-bounds reads return zero / in-bounds data, no fault.
- [ ] More induced failures for stage E: a fragment-shader lockup, a fault in the 3D pipe,
      a fault during a transfer. Each must end in `VK_ERROR_DEVICE_LOST` and leave later
      devices working.
- [ ] After `VK_ERROR_DEVICE_LOST`, check that the same process can create a new device.

## 9. Preemption

- [ ] `prio-*` with `--require-preempt` as a pass criterion once the host enables context
      switching (`fw_os_config_flags`), and a check that both outputs stay right across
      many store/resume cycles.

## 10. Memory

- [ ] Large allocations close to the heap size; many small allocations; mapping /
      unmapping in a loop.
- [ ] Sparse binding / residency when supported (not on the current PowerVR driver).

## 11. Optional

- [ ] `--bench`: repeat each test and report times, to catch performance regressions in
      the firmware.
- [ ] A one-line summary per group for the oracle (`stage B: 7/7`).

## Known limits of the current tests

- `rta`: the `gl_Layer` draw needs `VK_EXT_shader_viewport_index_layer`, which the
  current PowerVR driver lacks. There, only the layered clears run.
- `indirect`: the multi-draw part needs `multiDrawIndirect`.
- `queries`: exact occlusion counts need `occlusionQueryPrecise`; without it only
  zero / non-zero is checked.
- `msaa`: the diagonal coverage is checked only with standard sample positions.
- `hwr-*`: never run on a GPU that drives a display.
