==============
Renesas RZ/V2H
==============

The RZ/V2H is a Renesas AI-accelerated multicore MPU built around a
heterogeneous set of Arm cores: dual Cortex-A55 (application cores) plus
dual Cortex-R8 (real-time cores), alongside a DSP and an AI-MAC (DRP-AI3)
accelerator.

Supported MCUs
==============

The following list includes MCUs from the RZ/V2H series and indicates
whether they are supported in NuttX:

=============  ======= ================
MCU            Support Note
=============  ======= ================
R9A09G057      Yes     CR8_0 core only
=============  ======= ================

Peripheral Support
==================

The following list indicates peripherals supported in NuttX on the CR8_0
core:

==========  =======  =====================================
Peripheral  Support  Notes
==========  =======  =====================================
CLOCK       Yes      BSP clock tree init (PLL), internal use only
GIC         Yes      Interrupt controller init (CPU0 + per-CPU)
MPU         Yes      Static PMSAv7 region table (ITCM/DTCM/SRAM/code)
TIMER       Yes      MPCore Private Timer used as the system tick source
SCI         No       Kconfig option present, driver not yet implemented
GPIO        No
LED         No       Board hooks present, not yet wired to hardware
==========  =======  =====================================

CLOCK
-----

``rzv2h_clockconfig()`` calls into ``bsp_clock_init()`` from the vendored
Renesas FSP BSP to program the PLL and clock tree. It is invoked once
during boot and is not user-configurable through Kconfig.

GIC
---

The Generic Interrupt Controller is brought up by ``up_irqinitialize()``,
using NuttX's common ARM GIC support: CPU0-only initialization followed by
per-CPU initialization.

MPU
---

A static PMSAv7 region table protects the CR8_0 memory map (ITCM, DTCM,
SRAM and the executable code window). Regions are set up once at boot and
are not reconfigurable at runtime.

TIMER
-----

The Arm MPCore Private Timer is used as the system tick source, clocked
from the BSP clock tree and reloaded to match ``CLK_TCK``.

SCI
---

The Serial Communications Interface Kconfig options are present
(``CONFIG_SCI1_*``) but there is no backing driver yet: ``rzv2h_serial.c``
and ``rzv2h_lowputc.c`` only contain empty stubs, so no console output is
available.

GPIO
----

No GPIO driver has been implemented yet.

LED
---

Board-level LED hooks (``board_userled_*()``,
``board_autoled_on()``/``off()``) exist in
``boards/arm/rzv2h/rzv2h-evk/src/rzv2h_auto_leds.c`` but are currently
no-ops.

Supported Boards
================

.. toctree::
   :glob:
   :maxdepth: 1

   boards/*/*
