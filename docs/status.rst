.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=================
Status and limits
=================

State on 2026-11-27:

* 57 tests: 50 in ``all``, 4 ``hwr-*``, 3 ``prio-*``;
* on a desktop GPU (NVIDIA GTX 1060), 49 of the 50 tests of ``all`` pass and
  ``suspend-after`` is skipped (no runtime PM); the earlier 39 pass at every
  ``--api`` from 1.1 to 1.4, and 34 (5 skipped) at 1.0 (:doc:`versions`);
* only Vulkan 1.0 is required: newer features are used when present;
* on the BXE-2-32 board, the failures point at firmware bugs, not at the
  harness. With the read-back barrier, the clean firmware passes 42 of 50; the
  6 failures are the ``second-*`` Mesa-bug detectors (firmware experiment
  E047).
  ``--baseline`` compares a run with the reference runs in ``baselines/``.

The work list is ``docs/steps.md`` in the repository.

Read-back barrier
=================

Every read-back copy (``cmd_copy_to_buffer``, ``cmd_copy_layer_to_buffer``) is
preceded by an explicit pipeline barrier (all writes -> transfer reads). The
render passes already declare a dependency to ``VK_SUBPASS_EXTERNAL`` for the
transfer, which is enough by the Vulkan spec, but Mesa ``pvr`` ignores external
subpass dependencies: without the barrier the copy raced the render, and most
render tests failed on the board with any firmware (firmware experiment E047).
The ``second-*`` tests keep the old behaviour to show the driver bug.

Known limits
============

* ``rta``: the ``gl_Layer`` draw needs
  ``VK_EXT_shader_viewport_index_layer``, which the current PowerVR driver
  lacks; there, the layered clears, the layer-0 draw, the depth and stencil
  layers, the 3D image and the 6-view multiview draw run.
  They are what exercises the firmware's layer loop: with firmware 1.1 the
  driver turns on ``gs_rta_support`` and sends all six layers as one render.
* ``indirect``: the multi-draw part needs ``multiDrawIndirect``.
* ``queries``: exact occlusion counts need ``occlusionQueryPrecise``;
  without it only zero / non-zero is checked.
* ``msaa``: the diagonal coverage uses the measured sample positions when the
  device has ``sampleRateShading`` and ``fragmentStoresAndAtomics``, else the
  standard table; with custom positions and no measurement it is skipped.
* ``hwr-*``: never run on a GPU that drives a display.
* Optional features (sparse binding, subgroups, ETC2/ASTC, push descriptors,
  dynamic rendering, ``sync_file``) are skipped where the device lacks them.
