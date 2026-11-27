.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=====
Build
=====

Requirements
============

* GCC with C23 (``-std=gnu23``, GCC 14 or newer);
* the Vulkan headers and loader (``vulkan/vulkan.h``, ``libvulkan``); the
  flags come from ``pkg-config vulkan`` when it is available;
* ``glslangValidator`` to compile the shaders.

The shaders are compiled at build time (``--target-env vulkan1.1``) and
embedded as ``static const uint32_t`` arrays, so the target needs no shader
compiler at run time.

Native and cross builds
=======================

.. code-block:: sh

   make                    # build/vkfwtest
   make clean
   make CC=riscv64-linux-gnu-gcc PKG_CONFIG=riscv64-linux-gnu-pkg-config

``CC``, ``CFLAGS``, ``LDFLAGS``, ``PKG_CONFIG`` and ``GLSLANG`` can be
overridden. The build uses ``-Wall -Wextra -Werror -Wshadow -Wformat=2``.

Portability
===========

The code does not depend on the CPU architecture:

* no assembly and no intrinsics;
* fixed-width integer types; no assumption on the size of ``long``;
* pixel and word checks compare bytes, or read words with ``memcpy``;
* Vulkan handles are stored through ``H2U``/``U2H``, which work whether
  non-dispatchable handles are pointers (64-bit hosts) or ``uint64_t``.

On the K1 board
===============

The board has GCC 14, ``libvulkan`` and ``glslangValidator``.
``tools/deploy.sh vkfwtest`` in the firmware repository sends the sources
over ssh (the board has no ``rsync``) and builds them in
``~/vkfwtest-clean``.
