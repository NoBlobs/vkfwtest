.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=====
Usage
=====

.. code-block:: text

   vkfwtest [options] <test|group:NAME>... | all | list

* a test name (or its alias, such as ``pre-3d``) runs that test;
* ``group:NAME`` runs every test of a group: ``render``, ``compute``,
  ``transfer``, ``texture``, ``sync``, ``memory``, ``robust``,
  ``power``, ``stress``, ``hwr``, ``prio``;
* ``all`` runs every test except the ``hwr`` and ``prio`` groups;
* ``list`` prints the tests (with ``--json``, as a JSON array).

Tests run in command-line order.

Options
=======

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Option
     - Meaning
   * - ``-f, --frames N``
     - frames or rounds per test (each test has its own default)
   * - ``-s, --size WxH``
     - render-target size, or ``N`` for N×N
   * - ``-t, --tris N[K|M]``
     - triangles (``pre-ta``, ``bigpb``)
   * - ``-i, --iters N``
     - LCG iterations per pixel or invocation
   * - ``-g, --groups N``
     - compute workgroups (``pre-cdm``, ``indirect``)
   * - ``-S, --seed N``
     - pattern seed (default 1)
   * - ``-T, --timeout SEC``
     - per-fence timeout (default 60)
   * - ``-L, --test-timeout SEC``
     - abort the whole run (exit 2) if one test takes longer
   * - ``-r, --repeat N``
     - run each test N times
   * - ``-b, --bench N``
     - run each test N times and print min/avg/max times
   * - ``-p, --priority P``
     - ``low``, ``medium`` or ``high`` (``VK_KHR/EXT_global_priority``);
       falls back to the default when not allowed
   * - ``-d, --device N``
     - physical device index (default: the first PowerVR device, else 0)
   * - ``-K, --keep-device``
     - run all the tests on one device, in order (to reproduce bugs that need
       earlier work on the same context)
   * - ``-A, --api X.Y``
     - use at most Vulkan X.Y for the instance and the device, as a driver of
       that version would (:doc:`versions`)
   * - ``-I, --idle-ms MS``
     - ``idle-resume``: idle time (default: the autosuspend delay + 500 ms)
   * - ``-D, --dump DIR``
     - write ``<test>-<what>-{got,expected,diff}.ppm`` for every mismatching
       image to DIR
   * - ``-P, --require-preempt``
     - ``prio-*``: fail unless the probes overtake the long job
   * - ``-k, --keep-going``
     - run the remaining tests after a failure
   * - ``-j, --json``
     - one JSON line per test, and a summary line
   * - ``-v`` / ``-q``
     - more output / only the result lines
   * - ``-V``, ``-h``
     - version, help

Every test creates its own ``VkDevice`` (unless ``--keep-device``), so the
kernel creates fresh firmware contexts for it.

Output
======

One line per test, then a total and a per-group line:

.. code-block:: text

   PASS    multiframe       248.3 ms  256x256, 8 frames
   FAIL    zs-load          185.9 ms  frame 0 pass 2 (loaded depth): 4096 of 4096 pixels wrong
   == 34 pass, 1 fail, 0 timeout, 0 lost, 0 error, 0 skip
   == groups: render 16/16 compute 3/4 transfer 5/5 ...

On a mismatch, the first 8 wrong values go to stderr as
``(x,y) got … expected …``.

With ``--json``, each test prints:

.. code-block:: json

   {"test":"multiframe","group":"render","iteration":0,"result":"PASS","code":0,
    "ms":226.7,"frames":8,"checked":524288,"mismatches":0,
    "device":"PowerVR B-Series BXE-2-32 MC1","detail":"256x256, 8 frames"}

and the run ends with
``{"summary":{"pass":…,"fail":…,"timeout":…,"lost":…,"error":…,"skip":…,"code":…}}``.

Exit codes
==========

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Code
     - Meaning
   * - 0
     - every test passed
   * - 1
     - output mismatch
   * - 2
     - fence timeout, or the ``--test-timeout`` limit
   * - 3
     - ``VK_ERROR_DEVICE_LOST``
   * - 4
     - setup error or bad option
   * - 77
     - every test was skipped (not supported)

The exit code is the worst result of the run.

Examples
========

.. code-block:: sh

   vkfwtest -v all                        # everything, with the device name
   vkfwtest -k -j all > results.json      # CI: all results and a summary
   vkfwtest group:transfer
   vkfwtest bigpb -t 2M -f 4              # a smaller parameter-buffer stress
   vkfwtest -D /tmp/out multiframe        # images of a wrong frame
   vkfwtest -K multiframe pre-ta          # both on one device
   vkfwtest -b 10 pre-cdm                 # timings
