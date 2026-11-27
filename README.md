<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# vkfwtest

Headless Vulkan workloads that drive GPU firmware paths through a normal Vulkan driver.
Every result is **checked on the CPU**, pixel by pixel or word by word.

The NoBlobs clean-room GPU firmwares use it as their main test battery:
- Imagination BXE-2-32;
- Imagination BXM-4-64.

On their boards it runs through the mainline `powervr` kernel driver and Mesa.

A test returns 0 only if both of these hold:
- every fence signalled within the timeout;
- every value read back matches the expected one.

A firmware that skips work, discards a command or renders garbage therefore fails, even
when nothing faults.

## Build

Requirements: GCC with C23 (`-std=gnu23`, GCC ≥ 14), the Vulkan headers and loader
(`vulkan/vulkan.h`, `libvulkan`), and `glslangValidator`. The shaders are compiled at build
time and embedded as `uint32_t` arrays, so the target needs no shader compiler.

```
make                 # build/vkfwtest
make CC=riscv64-linux-gnu-gcc PKG_CONFIG=riscv64-linux-gnu-pkg-config   # cross
```

The code does not depend on the CPU architecture: no assembly, no intrinsics, fixed-width
types, and byte-wise checks.

## Usage

```
vkfwtest [options] <test|group:NAME>... | all | list
  all runs every test except hwr-* and prio-*
  groups: render compute transfer texture sync memory robust power stress hwr prio

  -f, --frames N         frames / rounds per test (default per test)
  -s, --size WxH         render target size (or N for NxN)
  -t, --tris N[K|M]      triangles (pre-ta, bigpb)
  -i, --iters N          LCG iterations per pixel / invocation
  -g, --groups N         compute workgroups (pre-cdm, indirect)
  -S, --seed N           pattern seed (default 1)
  -T, --timeout SEC      per-fence timeout (default 60)
  -L, --test-timeout SEC abort the run if one test takes longer (exit 2)
  -r, --repeat N         run each test N times
  -b, --bench N          run each test N times and print min/avg/max times
  -p, --priority P       low|medium|high (VK_KHR/EXT_global_priority)
  -d, --device N         physical device index (default: first PowerVR, else 0)
  -K, --keep-device      run all the tests on one device (in order)
  -A, --api X.Y          use at most Vulkan X.Y (instance and device)
  -I, --idle-ms MS       idle-resume: idle time (default: autosuspend delay + 500)
  -D, --dump DIR         write got/expected/diff PPM images of mismatches to DIR
  -P, --require-preempt  prio-*: fail unless the probes overtake the long job
  -k, --keep-going       run the remaining tests after a failure
  -j, --json             one JSON line per test, and a summary line, on stdout
  -v, --verbose / -q, --quiet / -V, --version / -h, --help
```

Each test creates its own `VkDevice`, so the kernel creates fresh firmware contexts for it.

**Vulkan versions.** vkfwtest requires only Vulkan 1.0. It asks for the instance version
the loader offers (falling back to 1.0 if the driver refuses) and uses the lower of the
instance and device versions. Features of newer versions are used only when present, by core
version or by extension; otherwise the test, or the part of it, reports `SKIP`. `--api X.Y`
caps the version, to see what a driver of that version would run. See
`docs/versions.rst` for the tests by minimum version.

**Shader caches.** On NVIDIA, run with `__GL_SHADER_DISK_CACHE=0`: the on-disk cache can
return a pipeline built without `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` for the same SPIR-V,
and `compute-dims` then fails. On Mesa drivers, `MESA_SHADER_CACHE_DISABLE=true` does the
same, when a cached pipeline must be ruled out.
Output is one line per test:

```
PASS    multiframe     226.7 ms  256x256, 8 frames
```

With `--json`:

```json
{"test":"multiframe","iteration":0,"result":"PASS","code":0,"ms":226.7,"frames":8,
 "checked":524288,"mismatches":0,"device":"PowerVR B-Series BXE-2-32 MC1","detail":"…"}
```

On a mismatch, the first 8 wrong values are printed on stderr as `(x,y) got … expected …`.

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | all tests passed |
| 1 | output mismatch |
| 2 | fence timeout |
| 3 | `VK_ERROR_DEVICE_LOST` |
| 4 | setup error or bad option |
| 77 | every test skipped (not supported) |

## Tests

| Test (alias) | Firmware paths | What is checked |
|---|---|---|
| `multiframe` (`pre-3d`) | geometry + fragment over several frames on one device and framebuffer, so the driver alternates the two HWRTData of the render target | every pixel of every frame: a per-frame colour, or with `--iters` a per-pixel integer LCG |
| `pre-ta` | geometry-heavy frames: layers of tiny tiled triangles, 100 K by default | every pixel equals the last layer's colour for its cell |
| `load-store-none` | attachments with `STORE_OP_NONE` (1.3) and `LOAD_OP_NONE` (1.4), or `VK_KHR/EXT_load_store_op_none`: bound but never written, their uploaded pattern must survive | every pixel of both attachments |
| `local-read` | dynamic rendering local read (1.4 or `VK_KHR_dynamic_rendering_local_read`): one rendering, a draw, a by-region barrier, then the attachment read as an input attachment | both attachments, every pixel |
| `vertex-ext` | 8-bit indices and instance-rate divisors including 0 (1.4, or `index_type_uint8` / `vertex_attribute_divisor` extensions) | every pixel |
| `sync2` | `vkQueueSubmit2` with semaphores and `vkCmdPipelineBarrier2` (1.3 or `VK_KHR_synchronization2`) | every word, in the order both barriers impose |
| `pre-cdm` | compute dispatches (local size 64) | every word equals `lcg(id ^ seed, iters)` |
| `xfer` | 3D-pipe transfers on a 137×91 (NPOT) and a 256×256 image: upload, copy with offsets onto a cleared image, mirrored 1:1 blit, 4× MSAA clear + resolve | every byte |
| `bigpb` | parameter-buffer out-of-memory: millions of triangles (4 M by default) into 1024×1024 + `D32_SFLOAT` (stored), so free-list grows and partial renders happen | every pixel equals the **first** layer's colour (later layers are farther away), so a partial render that loses depth or colour fails |
| `input-attach` | one render pass with two subpasses (two HWRTData). Subpass 0: clear and a depth-tested rectangle. Subpass 1: reads colour + `D32` depth as input attachments | both colour attachments, every pixel |
| `rta` | a 6-layer render target (render-target arrays): the render pass clear, per-layer `vkCmdClearAttachments`, and, with `VK_EXT_shader_viewport_index_layer`, one `gl_Layer` draw per layer | every pixel of every layer |
| `texture` | texture unit: a 37×23 (NPOT) texture with REPEAT, MIRRORED_REPEAT, CLAMP_TO_EDGE, CLAMP_TO_BORDER (white, transparent), LINEAR halfway between texels, and 6 mip levels | every texel or border colour; the linear case allows ±1 |
| `zs-load` | depth + stencil (D24S8 or D32S8, else depth only) stored by one render pass and loaded by the next two | the depth test and the stencil test of the later passes select the expected halves |
| `msaa` | 2×/4×/8× rendering resolved by a resolve attachment + depth resolve; a 4× triangle edge along the diagonal | full pixels exact; 4× diagonal pixels equal the average ±1; resolved depth exact |
| `mrt` | up to 8 colour attachments written by one subpass | every pixel of every attachment |
| `sizes` | render targets of 1×1, 7×5, 33×17, 97×61, 256×1, 1×256, 1000×3, 1920×1080, 4096×4096, plus a partial `renderArea` with LOAD | a unique per-pixel value everywhere; pixels outside the render area keep their colour |
| `vertex` | vertex and index buffers (16- and 32-bit indices, `firstIndex`, `vertexOffset`, `firstInstance`), instancing, point and line lists | every pixel |
| `indirect` | `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect`, multi-draw indirect (when supported), `vkCmdDispatchIndirect` | the drawn cells; the dispatched words, with a sentinel after them |
| `queries` | occlusion queries (exact with `occlusionQueryPrecise`), read on the host and copied by the GPU; timestamps | 960 / 0 / 0 samples; increasing timestamps |
| `formats` | R5G6B5, B8G8R8A8, R8G8B8A8_SRGB, R16G16B16A16_SFLOAT, R32_UINT targets; additive blending; `discard` | the raw bytes of every pixel (sRGB ±1) |
| `secondary` | secondary command buffers (`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`) and partial-rect `vkCmdClearAttachments` between draws | every pixel |
| `multiview` | `VK_KHR_multiview` (core 1.1): one draw into 2 layers with `gl_ViewIndex` | every pixel of both views |
| `raster` | viewport offset + negative height, triangle strip + primitive restart, fan, line strip, depth bias/clamp/bounds, wide lines, large points, 4× sample mask, sample shading + alpha-to-coverage, no-attachment pass, dynamic rendering | every pixel and atomic fragment count |
| `resources` | UBO, dynamic SSBO offset, `r32ui` storage image, uniform/storage texel buffers, buffer + image atomics, shared memory + `barrier()`, fragment `imageLoad`/`imageStore`, push descriptors | every word, texel and sentinel |
| `compute-dims` | 3D `vkCmdDispatchBase` and largest 1D workgroup size (specialisation constant) with shared-memory reduction | every element of the 3D grid and every workgroup sum |
| `subgroups` | compute subgroup size, `subgroupElect`, `subgroupBallot`, `subgroupAdd` (when supported) | every invocation |
| `buffers` | `vkCmdFillBuffer`, `vkCmdUpdateBuffer`, `vkCmdCopyBuffer` (multiple regions, odd sizes/offsets, 1 B) | every byte |
| `ds-copy` | `vkCmdClearDepthStencilImage` and depth/stencil image ↔ buffer copies | every texel |
| `blit-mips` | scaled `VK_FILTER_LINEAR` blits (upscale/downscale) and 7-level mip-chain generation | every pixel (±1) |
| `image-copy` | 3D sub-volume, multi-layer 2D array, and format-compatible (`R32_UINT` → `RGBA8`) copies | every texel |
| `texture2` | cube map + cube array, 3D, 2D array, shadow compare, `textureGather`, integer/float `texelFetch`, unnormalized coords, ETC2, ASTC 4×4, anisotropic sampler (when supported) | every probe |
| `sync` | binary semaphores (multi-queue when available), timeline semaphores, host/GPU events, 500 small fence-synced submits, external `sync_file` fence export/import | every word |
| `memory` | large device-local allocation (up to 64 MiB), 256 small allocations, 200 map/unmap cycles | every checked word |
| `sparse` | sparse buffer binding and residency (`vkQueueBindSparse`) when supported | every word of the bound sparse page |
| `robust` | `robustBufferAccess`: out-of-bounds read returns 0 or in-bounds data, out-of-bounds write discarded, no fault | all 16 words |
| `idle-resume` | render, wait past runtime-PM autosuspend delay (`--idle-ms` or sysfs + 500 ms), render again on the same device | every pixel of both renders |
| `stress` | four threads with a device each, running `multiframe`, `pre-ta`, `pre-cdm` and `xfer` at the same time | the four tests' own checks |
| `soak` | one device running `multiframe` ×120, `pre-cdm` ×120, `pre-ta` ×20, `xfer` ×10 | the same |
| `churn` | 20 devices created and destroyed, each with 2 frames and a dispatch | the same |
| `hwr-lockup` | **not in `all`**. A compute shader that spins forever. The submission must end in `VK_ERROR_DEVICE_LOST` (watchdog recovery); then `multiframe` and `pre-cdm` must pass on new devices, without reloading the driver | device lost, then the two tests |
| `hwr-fault` | **not in `all`**. A compute shader that writes and reads about 4 GiB beyond its storage buffer: an MMU page fault. Then the same recovery checks as `hwr-lockup` | device lost, then the two tests |
| `hwr-frag` | **not in `all`**. A fragment shader that spins forever. Must end in `VK_ERROR_DEVICE_LOST`, then `multiframe` and `pre-cdm` must pass on new devices | device lost, then the two tests |
| `hwr-3d-fault` | **not in `all`**. A fragment shader that writes and reads far beyond its SSBO: a 3D-pipe MMU page fault. Then the same recovery checks | device lost, then the two tests |
| `prio-3d`, `prio-ta`, `prio-cdm` | **not in `all`**. A long job calibrated to about 1.5 s on a LOW-priority device, while 10 short probes run on a HIGH-priority device | every output. It reports whether the probes were delayed; with `--require-preempt`, a delay is a failure |

All geometry is generated from `gl_VertexIndex`, with no vertex buffers. Cell edges fall on
pixel edges, so every expected colour is exact.

The shaders' integer LCG (`--iters`) is checked with its closed form: the n-fold
composition of the affine map, computed by squaring. Even jobs of hundreds of millions of
iterations are checked exactly.

**Do not run `hwr-lockup` or `hwr-fault` on a desktop GPU that drives your display**: the screen can
freeze until the driver recovers.

## Clean-room note

This program is written only from the public Vulkan API and the published clean-room
specification (`docsClean`, `board-testing-oracle.md` section 4). Contributions must follow
the same rule. Do not contribute if you have seen the vendor's driver (DDK) or firmware
sources, binaries or disassembly.

## Licence

GPL-3.0-or-later; see `LICENSE`.
