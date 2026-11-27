<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# vkfwtest — what is left to implement

State on 2026-11-27:
- 57 tests (50 in `all`, 4 `hwr-*`, 3 `prio-*`);
- 49 of the 50 tests of `all` pass on a desktop GPU (NVIDIA GTX 1060), `suspend-after` is
  skipped there (no runtime PM);
- on the BXE-2-32 board, the failures point at firmware bugs, not at the harness.

Items are ordered by how much they help firmware work. Each new test keeps the rules in
[Writing a test](writing-tests.rst):
- headless;
- exact expected values computed on the CPU;
- a default that runs in seconds;
- `R_SKIP` when the device lacks a feature.

## 1. Debugging aids

- [x] `--dump DIR`: on a mismatch, write the image read back and the expected image (PPM,
      no library) and a diff mask. The first 8 wrong values on stderr are not enough to
      see a missing tile or a shifted layer.
- [x] A summary at the end of a run: one JSON object with every result, so CI does not
      have to collect the lines.
- [x] Select tests by group: `group:render|compute|transfer|texture|sync|memory|robust|power|stress|hwr|prio`. Also
      `vkfwtest --json list`.
- [x] Per-test time limit (`--test-timeout`), so a test whose fences keep signalling but
      never finishes cannot hold up the run.
- [x] `--keep-device`: run several tests on one device, to reproduce firmware bugs that
      need earlier renders on the same context (the "second render" family).

## 2. Power

- [x] `idle-resume`: render; wait longer than the runtime-PM autosuspend (read
      `/sys/.../power/autosuspend_delay_ms` when available, else `--idle-ms` / default); render
      again on the same device. This is the warm-boot path from inside the harness;
      today only the oracle's stage D covers it.

## 3. Shader resources and compute

- [x] Uniform buffers (std140), dynamic storage offsets, push descriptors (`resources`).
- [x] Storage images: `imageLoad`/`imageStore` in compute and fragment shaders (`resources`).
- [x] Texel buffers (uniform and storage) (`resources`).
- [x] Atomics on buffers and images; shared memory with `barrier()` (`resources`, `compute-dims`).
- [x] Workgroups at `maxComputeWorkGroupSize`/`Invocations`; 3D dispatches and
      `vkCmdDispatchBase` (`compute-dims`).
- [x] Subgroup operations (basic, elect, ballot, arithmetic) when supported (`subgroups`).

## 4. Transfers

- [x] `vkCmdCopyBuffer` (multiple regions, odd sizes/offsets), `vkCmdFillBuffer`,
      `vkCmdUpdateBuffer` (`buffers`).
- [x] Depth/stencil: `vkCmdClearDepthStencilImage`, depth/stencil copies image↔buffer (`ds-copy`).
- [x] Scaled blits with `VK_FILTER_LINEAR`; mip-chain generation with blits (`blit-mips`).
- [x] 3D images and multi-layer copies (`image-copy`).
- [x] Copies between images of different (compatible) formats (`image-copy`).

## 5. Textures

- [x] Cube maps and cube arrays; 3D textures; 2D array textures with a layer index (`texture2`).
- [x] Depth-compare samplers (shadow), `textureGather`, `texelFetch`, unnormalized
      coordinates (`texture2`).
- [x] Anisotropic filtering (presence check when supported) (`texture2`).
- [x] Compressed formats the device exposes (ETC2 RGBA8, ASTC 4×4 LDR): upload a known block and
      check the decoded texels (`texture2`).
- [x] Integer and float textures (`R32G32B32A32_UINT`, `R32G32B32A32_SFLOAT`) sampled with `texelFetch` (`texture2`).

## 6. Render passes and rasterization

- [x] Secondary command buffers (`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`) (`secondary`).
- [x] Multiview (`VK_KHR_multiview`, core 1.1) and dynamic rendering (`VK_KHR_dynamic_rendering` / 1.3) (`multiview`, `raster`).
- [x] `vkCmdClearAttachments` with partial rectangles, inside a render pass with draws
      before and after (`secondary`).
- [x] Depth bias, depth clamp, depth bounds; viewport with non-zero origin and negative height (`raster`).
- [x] Sample shading, alpha-to-coverage, sample masks (`raster`).
- [x] MSAA 2×, 4×, and 8× where supported; depth resolve (`VK_KHR_depth_stencil_resolve` / 1.2) (`msaa`).
- [x] Load/store `DONT_CARE` combinations; render passes with no attachments (`raster`, `msaa`).
- [x] Wide lines and large points, line strips, triangle strips and fans, primitive restart (`raster`).

## 7. Synchronisation

- [x] More than one queue when the family offers it; submissions that depend on each
      other through binary semaphores (`sync`).
- [x] Timeline semaphores waited and signalled across submissions and from the host (`sync`).
- [x] `vkCmdSetEvent` / `vkCmdWaitEvents` inside command buffers; host-set events (`sync`).
- [x] External fences / `sync_file` export and import when supported (`sync`).
- [x] Many small submissions in a tight loop (500 submits, fence throughput) (`sync`).

## 8. Robustness and failures

- [x] `robustBufferAccess`: out-of-bounds reads return zero / in-bounds data, no fault (`robust`).
- [x] Fragment-shader lockup (`hwr-frag`) and 3D-pipe MMU page fault (`hwr-3d-fault`) for stage E:
      must end in `VK_ERROR_DEVICE_LOST` and leave later devices working.
- [x] After `VK_ERROR_DEVICE_LOST`, check that the same process can create a new device (`hwr-*`).

## 9. Preemption

- [x] `prio-*` with `--require-preempt` implemented (passes on board once host enables context switching).

## 10. Memory

- [x] Large allocation (up to 64 MiB / 1/8 heap); many small allocations; mapping /
      unmapping in a loop (`memory`).
- [x] Sparse binding / residency when supported (`sparse`).

## 11. Optional

- [x] `--bench`: repeat each test and report min/avg/max times, to catch performance regressions in
      the firmware.
- [x] A one-line summary per group (`== groups: ...` and JSON `"groups"`).

## 12. Firmware investigation (2026-11-27)

- [x] `second-*`: the second render on a device, one state change at a time (group `second`);
      `second-split` and `second-barrier` found the cause (Mesa ignores external subpass
      dependencies). The read-back helpers now issue an explicit barrier.
- [x] `--baseline FILE` and `baselines/`: compare a run with the reference runs of `docsClean`.
- [x] `bigpb-layers`: parameter-buffer partial renders in a render-target array.
- [x] `suspend-after`: runtime suspend after compute and sync work.
- [x] `ctx-switch`: two firmware contexts with interleaved renders and dispatches.
- [x] `rta` step 6: a 6-view multiview draw puts geometry in every layer; `msaa` measures the
      sample positions.

## Known limits of the current tests

- `rta`: the `gl_Layer` draw needs `VK_EXT_shader_viewport_index_layer`, which the
  current PowerVR driver lacks. The 6-view multiview step gives every layer geometry instead.
- `indirect`: the multi-draw part needs `multiDrawIndirect`.
- `queries`: exact occlusion counts need `occlusionQueryPrecise`; without it only
  zero / non-zero is checked.
- `msaa`: without `sampleRateShading` and `fragmentStoresAndAtomics` the positions are not
  measured, and with custom positions the diagonal is then skipped.
- `hwr-*`: never run on a GPU that drives a display.
