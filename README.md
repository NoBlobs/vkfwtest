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
vkfwtest [options] <test>... | all | list

  -f, --frames N      frames / rounds per test (default per test)
  -s, --size WxH      render target size (or N for NxN)
  -t, --tris N[K|M]   triangles (pre-ta, bigpb)
  -i, --iters N       LCG iterations per pixel / invocation
  -g, --groups N      compute workgroups (pre-cdm)
  -S, --seed N        pattern seed (default 1)
  -T, --timeout SEC   per-fence timeout (default 60)
  -r, --repeat N      run each test N times
  -p, --priority P    low|medium|high (VK_KHR/EXT_global_priority)
  -d, --device N      physical device index (default: first PowerVR, else 0)
  -k, --keep-going    run the remaining tests after a failure
  -j, --json          one JSON line per test on stdout
  -v, --verbose / -q, --quiet / -V, --version / -h, --help
```

Each test creates its own `VkDevice`, so the kernel creates fresh firmware contexts for it.
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
| `pre-cdm` | compute dispatches (local size 64) | every word equals `lcg(id ^ seed, iters)` |
| `xfer` | 3D-pipe transfers on a 137×91 (NPOT) and a 256×256 image: upload, copy with offsets onto a cleared image, mirrored 1:1 blit, 4× MSAA clear + resolve | every byte |
| `bigpb` | parameter-buffer out-of-memory: millions of triangles (4 M by default) into 1024×1024 + `D32_SFLOAT` (stored), so free-list grows and partial renders happen | every pixel equals the **first** layer's colour (later layers are farther away), so a partial render that loses depth or colour fails |

All geometry is generated from `gl_VertexIndex`, with no vertex buffers. Cell edges fall on
pixel edges, so every expected colour is exact.

## Clean-room note

This program is written only from the public Vulkan API and the published clean-room
specification (`docsClean`, `board-testing-oracle.md` section 4). Contributions must follow
the same rule. Do not contribute if you have seen the vendor's driver (DDK) or firmware
sources, binaries or disassembly.

## Licence

GPL-3.0-or-later; see `LICENSE`.
