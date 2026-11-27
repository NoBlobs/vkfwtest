.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

===============
Troubleshooting
===============

Shader caches
=============

On-disk shader caches can hand back a pipeline compiled earlier for the same
SPIR-V. vkfwtest does not change them; set the variables yourself when a
cached pipeline must be ruled out:

.. code-block:: sh

   __GL_SHADER_DISK_CACHE=0 vkfwtest all            # NVIDIA
   MESA_SHADER_CACHE_DISABLE=true vkfwtest all      # Mesa drivers

On NVIDIA, the cache returned a ``compute-dims`` pipeline built without
``VK_PIPELINE_CREATE_DISPATCH_BASE_BIT``. The dispatch then ignored the base
workgroup, and the test failed at ``(0,0,0)``.

The Mesa PowerVR driver
=======================

The PowerVR driver is not conformant, and refuses to start without:

.. code-block:: sh

   export VK_ICD_FILENAMES=/opt/mesa-pvr/share/vulkan/icd.d/powervr_mesa_icd.riscv64.json
   export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1

``HIGH`` global priority needs privileges; without them the test notes the
fallback and continues.

Disruptive tests
================

Do not run ``hwr-*`` on a GPU that drives your display: the screen can freeze
until the driver recovers.

A run that hangs
================

``--timeout`` bounds each fence wait. A test whose fences keep signalling but
never ends is bounded by ``--test-timeout``: the watchdog prints a
``TIMEOUT`` line and exits with code 2.

Looking at wrong images
=======================

``--dump DIR`` writes three PPM files per wrong image:

* the pixels read back;
* the expected ones;
* a red mask of the differences.

Any image viewer opens them.
