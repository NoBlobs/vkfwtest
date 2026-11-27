.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=================
Status and limits
=================

State on 2026-11-27:

* 46 tests: 39 in ``all``, 4 ``hwr-*``, 3 ``prio-*``;
* on a desktop GPU (NVIDIA GTX 1060), all 39 tests of ``all`` pass at every
  ``--api`` from 1.1 to 1.4, and 34 (5 skipped) at 1.0 (:doc:`versions`);
* only Vulkan 1.0 is required: newer features are used when present;
* on the BXE-2-32 board, the failures point at firmware bugs, not at the
  harness. They are recorded in the firmware repository (experiment E044).

The work list is ``docs/steps.md`` in the repository.

Known limits
============

* ``rta``: the ``gl_Layer`` draw needs
  ``VK_EXT_shader_viewport_index_layer``, which the current PowerVR driver
  lacks; there, only the layered clears run.
* ``indirect``: the multi-draw part needs ``multiDrawIndirect``.
* ``queries``: exact occlusion counts need ``occlusionQueryPrecise``;
  without it only zero / non-zero is checked.
* ``msaa``: the diagonal coverage is checked only with standard sample
  positions.
* ``hwr-*``: never run on a GPU that drives a display.
* Optional features (sparse binding, subgroups, ETC2/ASTC, push descriptors,
  dynamic rendering, ``sync_file``) are skipped where the device lacks them.
