.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

==========================
Tests by Vulkan version
==========================

vkfwtest itself needs only **Vulkan 1.0**:

* **Instance version.** It asks for the version the loader offers, and retries with 1.0
  if the driver refuses.
* **Version it uses.** It uses the lower of the instance and device versions.
* **Newer features.** They are enabled only when the device has them, by core version or
  by extension. A test, or the part of a test, that needs a missing one reports ``SKIP``.

Nothing newer is ever required, so the harness runs on the current mainline
PowerVR driver as it is.

Each test below is listed under the **lowest core version** it runs on in
full. Two notes say more:

* **with extension** — the same test runs on an older version when the device
  offers the extension;
* **parts** — pieces of a test that need more, and are skipped otherwise.

``--api X.Y`` caps the version vkfwtest asks for and uses, to see what a driver
of that version runs:

.. code-block:: sh

   vkfwtest -k -A 1.0 all

Vulkan 1.0
==========

These need only Vulkan 1.0. Some use optional 1.0 features (precise
occlusion, multi-draw indirect, depth clamp/bounds, wide lines, large points,
sample shading, ETC2/ASTC, anisotropy, sparse binding, fragment stores) and
skip just that part without them.

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Test
     - Parts that need more
   * - ``multiframe``, ``pre-ta``, ``bigpb``, ``input-attach``, ``zs-load``,
       ``mrt``, ``sizes``, ``vertex``, ``indirect``, ``queries``, ``formats``,
       ``secondary``
     - none
   * - ``rta``
     - the ``gl_Layer`` draw: ``VK_EXT_shader_viewport_index_layer``
   * - ``msaa``
     - depth/stencil resolve: 1.2, or 1.1 with ``VK_KHR_depth_stencil_resolve``
   * - ``raster``
     - the dynamic-rendering part: 1.3, or 1.1 with ``VK_KHR_dynamic_rendering``
   * - ``pre-cdm``, ``texture``, ``texture2``, ``xfer``, ``buffers``,
       ``ds-copy``, ``blit-mips``, ``image-copy``, ``memory``, ``sparse``,
       ``robust``
     - none (``sparse`` needs the sparse-binding features)
   * - ``resources``
     - push descriptors: 1.4, or 1.1 with ``VK_KHR_push_descriptor``
   * - ``compute-dims``
     - ``vkCmdDispatchBase``: 1.1
   * - ``sync``
     - timeline semaphores: 1.2; ``sync_file`` fences: 1.1 with
       ``VK_KHR_external_fence_fd``
   * - ``idle-resume``, ``stress``, ``soak``, ``churn``
     - none
   * - ``hwr-lockup``, ``hwr-fault``, ``hwr-frag``, ``hwr-3d-fault``
     - none (fragment stores for the 3D fault)
   * - ``prio-3d``, ``prio-ta``, ``prio-cdm``
     - the priorities: ``VK_KHR/EXT_global_priority`` (without it they run at
       the default priority)
   * - ``load-store-none``
     - **with extension**: ``VK_EXT_load_store_op_none`` or
       ``VK_KHR_load_store_op_none`` on any version; otherwise 1.3
       (``STORE_OP_NONE``) and 1.4 (``LOAD_OP_NONE``)

Vulkan 1.1
==========

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Test
     - Needs
   * - ``multiview``
     - the ``multiview`` feature (core 1.1)
   * - ``subgroups``
     - subgroup operations (core 1.1, SPIR-V 1.3); basic, ballot and
       arithmetic in compute

Vulkan 1.2
==========

No test needs 1.2 as a whole. The 1.2 parts are timeline semaphores (in
``sync``) and depth/stencil resolve (in ``msaa``).

Vulkan 1.3
==========

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Test
     - Needs
   * - ``sync2``
     - ``synchronization2`` (core 1.3); **with extension**: 1.1 with
       ``VK_KHR_synchronization2``

Vulkan 1.4
==========

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Test
     - Needs
   * - ``local-read``
     - ``dynamicRenderingLocalRead`` (core 1.4); **with extension**: 1.1 with
       ``VK_KHR_dynamic_rendering`` and ``VK_KHR_dynamic_rendering_local_read``
   * - ``vertex-ext``
     - ``indexTypeUint8`` and/or ``vertexAttributeInstanceRateDivisor`` (core
       1.4); **with extension**: 1.1 with ``VK_KHR/EXT_index_type_uint8`` and/or
       ``VK_KHR/EXT_vertex_attribute_divisor``

The larger minimum image limits of newer versions are covered by ``sizes``.
Its thin targets go up to 16384 pixels within the device limits, and
``-s 8192x8192`` runs a full 8192×8192 target.

Measured
========

On the development PC (NVIDIA GTX 1060, driver 1.4, all extensions present),
``vkfwtest -k -A X.Y all`` gives:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - ``--api``
     - Result
   * - 1.0
     - 34 pass, 5 skip: ``multiview``, ``subgroups``, ``sync2``, ``local-read``
       and ``vertex-ext``; the 1.1+ parts of ``msaa``, ``raster``,
       ``resources``, ``compute-dims`` and ``sync`` are skipped
   * - 1.1
     - 39 pass: the 1.3/1.4 tests run through their extensions; timeline
       semaphores (1.2) are skipped
   * - 1.2 – 1.4
     - 39 pass, every part included
