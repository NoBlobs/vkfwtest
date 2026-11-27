.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

============
Architecture
============

Source layout
=============

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Path
     - Content
   * - ``src/vkfwtest.c``
     - command line, the test table (name, alias, group, ``in_all``), the
       run loop, the test watchdog, JSON output and the exit code
   * - ``src/vkfwtest.h``
     - results, options, the Vulkan context, the helper API, the test entry
       points
   * - ``src/vk.c``
     - instance and device set-up, resource helpers, submission, checking,
       image dumps
   * - ``src/grid.c``
     - the tiled-triangle workload shared by ``pre-ta``, ``bigpb``,
       ``indirect``, ``prio-ta``
   * - ``src/t_*.c``
     - one file per test (or per family: ``t_hwr.c``, ``t_prio.c``,
       ``t_stress.c``)
   * - ``shaders/*``
     - GLSL 450 sources; ``build/<name>.h`` holds the SPIR-V array
       ``spv_<name>`` (for example ``spv_grid_vert``)

The Vulkan context
==================

``vk_init()`` creates one instance (API 1.2 requested) and one device:

* the physical device is ``--device N``, else the first whose name contains
  "PowerVR", else device 0;
* one queue family with graphics and compute; up to two queues of it;
* optional global priority (``--priority``), with a fallback to the default;
* the optional features the tests use, enabled when present: precise
  occlusion queries, multi-draw indirect, fragment and vertex stores,
  depth clamp and bias clamp, ETC2, cube arrays, sample shading, anisotropy,
  multiview, timeline semaphores; ``robustBufferAccess`` only on request;
* ``VK_EXT_shader_viewport_index_layer`` and other extensions when the
  device has them (flags such as ``has_output_layer``, ``has_timeline``).

Every object a helper creates is recorded in ``v->objs``;
``vk_fini()`` waits for the device (unless it is lost) and destroys them in
reverse order. A test never destroys its own objects.

Helpers
=======

* **Resources:** ``buf_create`` (host-visible buffers are mapped),
  ``img_create``, ``img_create_layers``, ``shader_create``,
  ``sampler_create``, ``desc_create`` (layout, pool and one set).
* **Passes and pipelines:** ``rp_create`` (one colour + optional depth),
  ``rp_build`` (MRT, MSAA, resolve, colour/depth/stencil LOAD),
  ``fb_create``/``fb_create_views``, ``layout_create``,
  ``gfx_pipeline`` (vertex input, topology, MRT, samples, additive blend,
  depth op, stencil), ``compute_pipeline`` (set 0 binding 0 = an SSBO).
* **Commands:** ``cmd_begin``, ``cmd_submit_wait`` (fence with the
  ``--timeout``), ``cmd_submit_async`` + ``fence_wait``,
  ``img_barrier``, ``cmd_copy_to_buffer``, ``cmd_copy_layer_to_buffer``.
* **Checks:** ``check_rgba``, ``check_rgba_tol`` (±tolerance per
  channel), ``check_rgba_step`` (every n-th pixel). They take an
  ``expect_fn(x, y, ctx)``, add to the report, print the first 8
  differences and, with ``--dump``, write PPM images.
* **Other:** ``run_fresh`` runs a test on a device of its own (used by
  ``stress``, ``churn``, ``hwr-*``), ``now_ms``, ``lcg``.

Results
=======

A test returns an ``enum result``:

* ``R_PASS``;
* ``R_MISMATCH``;
* ``R_TIMEOUT``;
* ``R_LOST``;
* ``R_SETUP``;
* ``R_SKIP``.

It also fills a ``struct report``: frames, values checked, mismatches and a
one-line detail. ``VK_CHECK``/``VK_TRY`` turn a failing ``VkResult``
into a result: ``VK_ERROR_DEVICE_LOST`` gives ``R_LOST`` and marks the
device lost; a missing feature or format gives ``R_SKIP``.

Exact expected values
=====================

* **Grid geometry:** ``grid_plan`` picks a cell count that divides both
  sides, so cell edges fall on pixel edges and every pixel's colour is a hash
  of its cell and layer.
* **The LCG:** ``x = x * 1664525 + 1013904223``, run ``n`` times in the
  shaders. ``lcg(x, n)`` computes the n-fold composition of the affine map
  by squaring, in O(log n).
* **Byte order:** pixels are compared byte by byte; words are read with
  ``memcpy`` in the host's byte order, as Vulkan defines them.
