.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

==============
Writing a test
==============

The contract
============

A test is a function

.. code-block:: c

   enum result test_name(struct vk *v, const struct opts *o, struct report *r);

and an entry in the ``tests[]`` table of ``src/vkfwtest.c``, with:

* a name;
* an optional alias;
* a group;
* ``in_all``;
* a one-line description.

Its prototype goes in ``src/vkfwtest.h``.

The rules:

* **Headless.** No window system, no swapchain.
* **Exact expectations, computed on the CPU.** Choose inputs whose results are
  exactly representable: cell edges on pixel edges, even values for averages,
  colours of the form ``k/255``. Allow a tolerance only where the API allows
  rounding (filtering, sRGB, multisample averages), and say so in the
  description.
* **Check everything.** Read back the whole output and compare every pixel or
  word.
* **Fast by default.** A default run takes seconds; options such as
  ``--frames``, ``--size`` or ``--tris`` scale it up.
* **Skip, do not fail, on missing support.** Return ``R_SKIP`` with the
  reason in ``r->detail`` when a feature, format, extension or limit is
  missing.
* **Use the helpers and the object table.** Create objects through the
  helpers (or ``vk_track`` them) and never destroy them; ``vk_fini``
  does.
* **Report.** Count ``r->frames``; the check helpers fill
  ``checked``/``mismatches``; write a short ``r->detail`` on success
  too.
* **Disruptive tests** (lockups, faults) and long ones get
  ``in_all = false`` and a group of their own.

Adding a shader
===============

Put the GLSL source in ``shaders/`` (``.vert``, ``.frag`` or
``.comp``). The Makefile compiles it to ``build/<file>.h`` with the array
``spv_<file with dots replaced>``. Include that header in the test file:

.. code-block:: c

   #include "fill.comp.h"          /* spv_fill_comp */

Clean-room rule
===============

Write tests only from the public Vulkan API and the published clean-room
specification. Do not contribute if you have seen the vendor's driver (DDK) or
firmware sources, binaries or disassembly.
