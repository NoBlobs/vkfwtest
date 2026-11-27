.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=====
Tests
=====

Every test computes its expected values on the CPU and compares the whole
output. Most geometry comes from ``gl_VertexIndex``, and cell edges fall on
pixel edges, so expected colours are exact. Filtering, sRGB and multisample
averages allow ±1. The shaders' integer LCG (``--iters``) is checked with its
closed form, so jobs of millions of iterations are still checked exactly.

A test, or the part of a test, that the device cannot run (a missing feature,
format, extension or Vulkan version) reports ``SKIP``. :doc:`versions` lists
the tests by the Vulkan version they need.

Render
======

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``multiframe``
     - geometry + fragment over several frames on one device and framebuffer, so the driver alternates the two HWRTData of the render target
     - every pixel of every frame: a per-frame colour, or with ``--iters`` a per-pixel integer LCG
   * - ``pre-ta``
     - geometry-heavy frames: layers of tiny tiled triangles, 100 K by default
     - every pixel equals the last layer's colour for its cell
   * - ``bigpb``
     - parameter-buffer out-of-memory: millions of triangles (4 M by default) into 1024×1024 + ``D32_SFLOAT`` (stored), so free-list grows and partial renders happen
     - every pixel equals the **first** layer's colour (later layers are farther away), so a partial render that loses depth or colour fails
   * - ``input-attach``
     - one render pass with two subpasses (two HWRTData). Subpass 0: clear and a depth-tested rectangle. Subpass 1: reads colour + ``D32`` depth as input attachments
     - both colour attachments, every pixel
   * - ``rta``
     - a 6-layer render target (render-target arrays): the render pass clear, per-layer ``vkCmdClearAttachments``, and, with ``VK_EXT_shader_viewport_index_layer``, one ``gl_Layer`` draw per layer
     - every pixel of every layer
   * - ``zs-load``
     - depth + stencil (D24S8 or D32S8, else depth only) stored by one render pass and loaded by the next two
     - the depth test and the stencil test of the later passes select the expected halves
   * - ``msaa``
     - 2×/4×/8× rendering resolved by a resolve attachment + depth resolve; a 4× triangle edge along the diagonal
     - full pixels exact; 4× diagonal pixels equal the average ±1; resolved depth exact
   * - ``mrt``
     - up to 8 colour attachments written by one subpass
     - every pixel of every attachment
   * - ``sizes``
     - render targets of 1×1, 7×5, 33×17, 97×61, 256×1, 1×256, 1000×3, 1920×1080, 4096×4096, plus a partial ``renderArea`` with LOAD
     - a unique per-pixel value everywhere; pixels outside the render area keep their colour
   * - ``vertex``
     - vertex and index buffers (16- and 32-bit indices, ``firstIndex``, ``vertexOffset``, ``firstInstance``), instancing, point and line lists
     - every pixel
   * - ``indirect``
     - ``vkCmdDrawIndirect``, ``vkCmdDrawIndexedIndirect``, multi-draw indirect (when supported), ``vkCmdDispatchIndirect``
     - the drawn cells; the dispatched words, with a sentinel after them
   * - ``queries``
     - occlusion queries (exact with ``occlusionQueryPrecise``), read on the host and copied by the GPU; timestamps
     - 960 / 0 / 0 samples; increasing timestamps
   * - ``formats``
     - R5G6B5, B8G8R8A8, R8G8B8A8_SRGB, R16G16B16A16_SFLOAT, R32_UINT targets; additive blending; ``discard``
     - the raw bytes of every pixel (sRGB ±1)
   * - ``secondary``
     - secondary command buffers (``VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS``) and partial-rect ``vkCmdClearAttachments`` between draws
     - every pixel
   * - ``multiview``
     - ``VK_KHR_multiview`` (core 1.1): one draw into 2 layers with ``gl_ViewIndex``
     - every pixel of both views
   * - ``raster``
     - viewport offset + negative height, triangle strip + primitive restart, fan, line strip, depth bias/clamp/bounds, wide lines, large points, 4× sample mask, sample shading + alpha-to-coverage, no-attachment pass, dynamic rendering
     - every pixel and atomic fragment count
   * - ``load-store-none``
     - attachments with ``STORE_OP_NONE`` (1.3) and ``LOAD_OP_NONE`` (1.4), or ``VK_KHR/EXT_load_store_op_none``: bound but never written, their uploaded pattern must survive
     - every pixel of both attachments
   * - ``local-read``
     - dynamic rendering local read (1.4 or ``VK_KHR_dynamic_rendering_local_read``): one rendering, a draw, a by-region barrier, then the attachment read as an input attachment
     - both attachments, every pixel
   * - ``vertex-ext``
     - 8-bit indices and instance-rate divisors including 0 (1.4, or ``index_type_uint8`` / ``vertex_attribute_divisor`` extensions)
     - every pixel

Compute
=======

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``pre-cdm``
     - compute dispatches (local size 64)
     - every word equals ``lcg(id ^ seed, iters)``
   * - ``resources``
     - UBO, dynamic SSBO offset, ``r32ui`` storage image, uniform/storage texel buffers, buffer + image atomics, shared memory + ``barrier()``, fragment ``imageLoad``/``imageStore``, push descriptors
     - every word, texel and sentinel
   * - ``compute-dims``
     - 3D ``vkCmdDispatchBase`` and largest 1D workgroup size (specialisation constant) with shared-memory reduction
     - every element of the 3D grid and every workgroup sum
   * - ``subgroups``
     - compute subgroup size, ``subgroupElect``, ``subgroupBallot``, ``subgroupAdd`` (when supported)
     - every invocation

Transfer
========

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``xfer``
     - 3D-pipe transfers on a 137×91 (NPOT) and a 256×256 image: upload, copy with offsets onto a cleared image, mirrored 1:1 blit, 4× MSAA clear + resolve
     - every byte
   * - ``buffers``
     - ``vkCmdFillBuffer``, ``vkCmdUpdateBuffer``, ``vkCmdCopyBuffer`` (multiple regions, odd sizes/offsets, 1 B)
     - every byte
   * - ``ds-copy``
     - ``vkCmdClearDepthStencilImage`` and depth/stencil image ↔ buffer copies
     - every texel
   * - ``blit-mips``
     - scaled ``VK_FILTER_LINEAR`` blits (upscale/downscale) and 7-level mip-chain generation
     - every pixel (±1)
   * - ``image-copy``
     - 3D sub-volume, multi-layer 2D array, and format-compatible (``R32_UINT`` → ``RGBA8``) copies
     - every texel

Textures
========

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``texture``
     - texture unit: a 37×23 (NPOT) texture with REPEAT, MIRRORED_REPEAT, CLAMP_TO_EDGE, CLAMP_TO_BORDER (white, transparent), LINEAR halfway between texels, and 6 mip levels
     - every texel or border colour; the linear case allows ±1
   * - ``texture2``
     - cube map + cube array, 3D, 2D array, shadow compare, ``textureGather``, integer/float ``texelFetch``, unnormalized coords, ETC2, ASTC 4×4, anisotropic sampler (when supported)
     - every probe

Synchronisation
===============

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``sync``
     - binary semaphores (multi-queue when available), timeline semaphores, host/GPU events, 500 small fence-synced submits, external ``sync_file`` fence export/import
     - every word
   * - ``sync2``
     - ``vkQueueSubmit2`` with semaphores and ``vkCmdPipelineBarrier2`` (1.3 or ``VK_KHR_synchronization2``)
     - every word, in the order both barriers impose

Memory
======

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``memory``
     - large device-local allocation (up to 64 MiB), 256 small allocations, 200 map/unmap cycles
     - every checked word
   * - ``sparse``
     - sparse buffer binding and residency (``vkQueueBindSparse``) when supported
     - every word of the bound sparse page

Robustness
==========

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``robust``
     - ``robustBufferAccess``: out-of-bounds read returns 0 or in-bounds data, out-of-bounds write discarded, no fault
     - all 16 words

Power
=====

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``idle-resume``
     - render, wait past runtime-PM autosuspend delay (``--idle-ms`` or sysfs + 500 ms), render again on the same device
     - every pixel of both renders

Stress
======

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``stress``
     - four threads with a device each, running ``multiframe``, ``pre-ta``, ``pre-cdm`` and ``xfer`` at the same time
     - the four tests' own checks
   * - ``soak``
     - one device running ``multiframe`` ×120, ``pre-cdm`` ×120, ``pre-ta`` ×20, ``xfer`` ×10
     - the same
   * - ``churn``
     - 20 devices created and destroyed, each with 2 frames and a dispatch
     - the same

Hardware recovery (not in all)
==============================

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``hwr-lockup``
     - **not in ``all``**. A compute shader that spins forever. The submission must end in ``VK_ERROR_DEVICE_LOST`` (watchdog recovery); then ``multiframe`` and ``pre-cdm`` must pass on new devices, without reloading the driver
     - device lost, then the two tests
   * - ``hwr-fault``
     - **not in ``all``**. A compute shader that writes and reads about 4 GiB beyond its storage buffer: an MMU page fault. Then the same recovery checks as ``hwr-lockup``
     - device lost, then the two tests
   * - ``hwr-frag``
     - **not in ``all``**. A fragment shader that spins forever. Must end in ``VK_ERROR_DEVICE_LOST``, then ``multiframe`` and ``pre-cdm`` must pass on new devices
     - device lost, then the two tests
   * - ``hwr-3d-fault``
     - **not in ``all``**. A fragment shader that writes and reads far beyond its SSBO: a 3D-pipe MMU page fault. Then the same recovery checks
     - device lost, then the two tests

Priority and preemption (not in all)
====================================

.. list-table::
   :header-rows: 1
   :widths: 18 52 30

   * - Test
     - Firmware paths
     - Checked
   * - ``prio-3d``, ``prio-ta``, ``prio-cdm``
     - **not in ``all``**. A long job calibrated to about 1.5 s on a LOW-priority device, while 10 short probes run on a HIGH-priority device
     - every output. It reports whether the probes were delayed; with ``--require-preempt``, a delay is a failure

