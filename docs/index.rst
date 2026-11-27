.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

========
vkfwtest
========

Headless Vulkan workloads that drive GPU firmware paths through a normal
Vulkan driver. Every result is **checked on the CPU**, pixel by pixel or word
by word.

The NoBlobs clean-room GPU firmwares use it as their main test battery:
Imagination BXE-2-32 (SpacemiT K1) and BXM-4-64, through the mainline
``powervr`` kernel driver and Mesa.

A test returns 0 only if both of these hold:

* every fence signalled within the timeout;
* every value read back matches the expected one.

A firmware that skips work, discards a command or renders garbage therefore
fails, even when nothing faults. A desktop GPU with a conformant driver passes
every test, which is how the expected values themselves are checked.

.. toctree::
   :maxdepth: 2

   building
   usage
   tests
   versions
   architecture
   writing-tests
   troubleshooting
   status
