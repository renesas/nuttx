==============
Renesas RZ/V2H
==============

The RZ/V2H is a Renesas AI-accelerated multicore MPU built around a
heterogeneous set of Arm cores: dual Cortex-A55 (application cores) plus
dual Cortex-R8 (real-time cores), alongside a DSP and an AI-MAC (DRP-AI3)
accelerator.

.. image:: RZV2H-block-chart-DRP-AI3.png
   :alt: RZV2H block chart DRP-AI3
   :width: 100%
   :align: center

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

==========  =======  ============================================================
Peripheral  Support  Notes
==========  =======  ============================================================
CLOCK       Yes      BSP clock tree init (PLL), internal use only
GIC         Yes      Interrupt controller init (CPU0 + per-CPU)
MPU         Yes      Static PMSAv7 region table (ITCM/DTCM/SRAM/code)
TIMER       Yes      MPCore Private Timer used as the system tick source
SCI         Yes      Interrupt-driven SCI-B asynchronous UART on channels 0-9
SPI         Yes      Interrupt-driven SPI-B on channels 0-2
GPIO        Yes      Basic operations (config/read/write), no interrupt support
I2C         Yes      Interrupt-driven RIIC master on channels 0-8
ADC         Yes      ADC-E unit 0 with 8 analog inputs and group-A scans
RTC         Yes      Calendar, one-shot alarm, periodic wakeup, system clock.
PWM         Yes      GPT (General PWM Timer) PWM on channels 0-15
==========  =======  ============================================================

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

The Serial Communication Interface (SCI) provides multiple serial
communication modes. NuttX currently supports interrupt-driven UART operation
on channels 0 through 9 through the SCI-B lower-half driver.
``CONFIG_RZV2H_UART_SCI`` enables the driver and
``CONFIG_RZV2H_SCI_B_UARTn`` selects each required channel. The standard
``CONFIG_SCIn_*`` options configure the channel and select the console.

The driver supports early console output and registers the selected
console as ``/dev/console`` and ``/dev/ttyS0``. Other enabled channels are
registered in ascending channel order.

Runtime baud-rate changes are supported through the standard termios
``TCGETS`` and ``TCSETS`` operations.

Hardware flow control and DMA are not implemented yet.

GPIO
----

The GPIO driver provides fundamental support for pin configuration, read,
and write operations. GPIO interrupt support is currently unavailable.

RTC
---

The RTC driver provides a calendar clock (date and time) through the
standard NuttX RTC lower-half. ``CONFIG_RZV2H_RTC`` enables it and requires
``CONFIG_RTC``, ``CONFIG_RTC_DATETIME``, ``CONFIG_RTC_DRIVER`` and
``CONFIG_RTC_ARCH``. It exposes ``/dev/rtc0`` and seeds the NuttX system
clock at boot. The supported calendar range is 2000-2099. If the RTC has
never been set, the system clock falls back to the build-time default date.

``CONFIG_RTC_ALARM`` adds a single one-shot alarm (``CONFIG_RTC_NALARMS``
must be 1) supporting absolute, relative, read-back and cancel operations.
``CONFIG_RTC_PERIODIC`` adds a periodic wakeup at rates from `1/128s` to `2s`.

INTC SEL (Interrupt Select) Lines
---------------------------------

The CR8 core's INTC provides a
pool of 127 programmable "SEL" lines (SEL0-SEL126, GIC IRQ numbers
353-479) whose peripheral event mapping is set through the INTC INTSEL
registers. Peripheral drivers that need one of these programmable lines
request a specific SEL number and event through
``rzv2h_intsel_connect_event()``.

Default SEL IDs
^^^^^^^^^^^^^^^

The 127-line SEL pool (SEL0-SEL126, IRQ 353-479) is conventionally
partitioned by module, allocated from the top of the range downward:

======  ==================  ================================
Module  Count                IRQ range (>= 353)
======  ==================  ================================
TINT    32 (tint0-tint31)    479-448
I2C     9 (i2c0-i2c8)        447-430 (txi/rxi only)
ADC     1 (adc0)             429
SPI     3 (spi0-spi2)        428-423 (txi/rxi only)
======  ==================  ================================

Changing interrupt-select IDs (RIIC2 as an example)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the default interrupt-select ID needs to change, e.g. to move
RIIC2 off its default TXI/RXI SEL IDs (443/442) and onto SEL IDs
397/396 instead:

#. Run ``make menuconfig`` and navigate to
   ``System Type -> RZ/V2H Configuration Options -> RIIC I2C support ->
   RIIC 2``.
#. Set ``RIIC2 TXI interrupt-select (SEL) IRQ number`` to ``397``.
#. Set ``RIIC2 RXI interrupt-select (SEL) IRQ number`` to ``396``.

Rebuild and reflash after changing either option so the new INTSEL
values are picked up.

I2C
---

The RIIC driver provides interrupt-driven I2C master
operation on channels 0 through 8.

``CONFIG_RZV2H_RIIC_I2C`` enables the driver and
``CONFIG_RZV2H_RIICn`` selects each required channel. Each channel has
independent Kconfig options for bus bitrate, SCL rise/fall times, duty
cycle, noise filter stages, TXI/RXI interrupt-select (SEL) IRQ numbers,
and interrupt priority.

I2C bus reset recovery (``CONFIG_I2C_RESET``) is supported. The driver
bit-bangs SCL to clock out stuck slaves, then generates a START+STOP
sequence to reset slave state machines.

ADC
---

The ADC-E driver supports the single RZ/V2H ADC0 unit and provides eight
dedicated analog inputs, ``ANI000`` through ``ANI007``.
``CONFIG_RZV2H_ADC`` enables the ADC-E driver. The following functions are
currently supported:

* 8-bit or 12-bit conversion results
* Polling or scan-end interrupt
* Software-triggered group-A scans
* Asynchronous external ``ADTRG`` input triggering in interrupt mode
* Configurable addition or averaging of results
* Clear-after-read operation
* Window A comparison and event status

``ANIOC_TRIGGER`` starts a software-triggered scan or arms the external
trigger, depending on the selected Kconfig trigger source.
``ANIOC_GET_NCHANNELS`` reports the number of configured inputs. Window A
builds also provide ``ANIOC_RZV2H_WINDOW_A_STATUS``.

The scan-end request is connected to a programmable INTC SEL line.
Currently set ADC's scan-end interrupt-select IRQ number to ``429`` (SEL76).

Group-B and group-C scans, GPT/ELC trigger routing, DMA delivery, and Window B
comparison are not currently supported.

SPI
---

The interrupt-driven SPI lower half supports SPI-B channels 0 through 2.  Each
channel can be enabled independently and configured at build time as either a
master or a slave using the standard NuttX SPI frameworks.

Master channels use ``struct spi_dev_s`` and support runtime frequency,
CPOL/CPHA mode, and 4- to 32-bit word-width configuration.  Slave channels use
``struct spi_slave_ctrlr_s`` and fixed-length transactions.  Both 3-wire
clock-synchronous operation without SSL and 4-wire operation with native
hardware SSL are supported.  RXI and TXI are routed through INTSEL, while TEI
and ERI use fixed SPI-B interrupts.

The RZ/V2H-EVK ``nsh-spi`` configuration provides SPI1 master loopback and
SPI0-master-to-SPI2-slave validation.  See the board documentation for wiring
and commands.

PWM
---

The RZ/V2H use the pulsed output control
implemented using the GPT (General PWM Timer). The driver supports PWM with
configurable frequency and duty cycle on channels 0 through 15.

``CONFIG_RZV2H_GPT_PWM`` enables the GPT PWM driver. Each channel is
individually selectable via ``CONFIG_RZV2H_GPTn_PWM``.
The output pin (GTIOCA or GTIOCB) for each channel is defined in the
board's ``board.h`` (``BOARD_GPTn_GTIOC_GPIO`` and ``BOARD_GPTn_USE_GTIOCA``).

Supported Boards
================

.. toctree::
   :glob:
   :maxdepth: 1

   boards/*/*
