/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <bsp_api.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/debug.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sdio.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>
#include <nuttx/sched.h>

#include "arm_internal.h"
#include "hardware/rzv2h_sdhi.h"
#include "rzv2h_irq.h"
#include "rzv2h_sdio.h"
#ifdef CONFIG_RZV2H_SDIO_DMA
#  include "rzv2h_sdio_dma.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_RZV2H_SDIO_UHS) && defined(CONFIG_RZV2H_SDIO_HIGHSPEED)
#  error "Select one SD signaling family"
#endif

#if defined(CONFIG_RZV2H_SDIO_UHS) && defined(CONFIG_MMCSD_DSR)
#  error "Private UHS does not support a later upper-half DSR change"
#endif

#define RZV2H_SDHI_CPG_TIMEOUT_US  10000

/* CPG fields for the SoC SDHI0/1 sequence. Low-halfword bits carry
 * values; matching upper-halfword bits enable writes to those fields only.
 * The shift/stride expressions select each host without changing its peer.
 */

#define SDHI_CPG_CLOCK_FIELD_MASK  0x0fu
#define SDHI_CPG_CLOCK_BASE_SHIFT  3
#define SDHI_CPG_CLOCK_STRIDE      4
#define SDHI_CPG_MSTOP_BASE_SHIFT  2
#define SDHI_CPG_RESET_BASE_SHIFT  7
#define SDHI_CPG_RSTMON_BASE_SHIFT 24
#define SDHI_CPG_WRITE_ENABLE_SHIFT 16

/* Finite retry budgets, not calibrated microsecond durations.  Keep IRQs
 * enabled between checks; CPU/compiler/MMIO timing affects elapsed time.
 */

#define SDHI_RESPONSE_POLLS                 10000000u
#define SDHI_IDLE_POLLS                     1000000u
#define SDHI_MAX_BLOCK_SIZE                 512u
#define SDHI_MAX_BLOCKS                     65535u
#define SDHI_STATUS_CD                      (SDHI_SD_INFO1_INFO3 | SDHI_SD_INFO1_INFO4)
#define SDHI_R1_ERRORMASK                   0xfdffe088u
#define SDHI_R1_APP_CMD                     (1u << 5)
#define SDHI_OCR_READY                      (1u << 31)
#define SDHI_OCR_HCS_CCS                    (1u << 30)
#define SDHI_OCR_S18                        (1u << 24)
#define SDHI_R5_ERRORMASK                   0x0000cb00u

/* SD switch-function arguments/status (SD Part 1, switch-function tables).
 * Offsets select bytes in the on-wire 64-byte status block. Keep protocol
 * values separate from the board's supply-current and clock permissions.
 */

/* CMD6: Check and switch between function modes
 * Bit 31 selects SWITCH (1) or CHECK (0)
 * Bits [30:24] are reserved zero
 * Bits [23:0] select groups 6..1 (0xf = group unchanged)
 *   Groups 5 and 6 are reserved and must be zero
 *   Group 4: Power Limit
 *   Group 3: UHS-I mode's driver strength
 *   Group 2: Command system extension
 *   Group 1: Bus speed mode. With 3.3 V signaling, Default Speed and
 *     High Speed are assigned to functions 0 and 1. With 1.8 V signaling,
 *     SDR and DDR modes are assigned to functions 0 through 4.
 *     Bits [3:0]: choose 0 = default, 1 = high speed/SDR25, 2 = SDR50,
 *     or 3 = SDR104.
 *     SET_ACCESS leaves group 1 clear for ORing the chosen mode; SET_CURRENT
 *     leaves group 4 [15:12] clear for ORing the chosen current function.
 */

#define SDHI_SWITCH_CHECK_HIGHSPEED         0x00fffff1
#define SDHI_SWITCH_SET_HIGHSPEED           0x80fffff1
#define SDHI_SWITCH_CHECK_ALL               0x00ffffff
#define SDHI_SWITCH_SET_CURRENT             0x80ff0fff
#define SDHI_SWITCH_CHECK_ACCESS            0x00fffff0
#define SDHI_SWITCH_SET_ACCESS              0x80fffff0
#define SDHI_SWITCH_CURRENT_SHIFT           12
#define SDHI_SWITCH_POWER_HI                0
#define SDHI_SWITCH_POWER_LO                1
#define SDHI_SWITCH_POWER_SHIFT             8
#define SDHI_SWITCH_CURRENT_SUPPORT         7
#define SDHI_SWITCH_ACCESS_SUPPORT          13
#define SDHI_SWITCH_CURRENT_RESULT          15
#define SDHI_SWITCH_ACCESS_RESULT           16
#define SDHI_SWITCH_STATUS_VERSION          17
#define SDHI_SWITCH_CURRENT_BUSY            23
#define SDHI_SWITCH_ACCESS_BUSY             29
#define SDHI_SWITCH_VERSION_BUSY            1
#define SDHI_SWITCH_GROUP_FUNCTION_MASK     0x0fu
#define SDHI_SWITCH_CURRENT_RESULT_SHIFT    4
#define SDHI_SWITCH_DRIVE_TYPE_B            0
#define SDHI_SWITCH_SPEED_DEFAULT           0
#define SDHI_SWITCH_SPEED_HIGHSPEED         1
#define SDHI_SWITCH_SPEED_SDR50             2
#define SDHI_SWITCH_SPEED_SDR104            3

/* Support/busy fields use bit n for function n: high speed is bit 1.
 * Current functions 0..3 map to 200/400/600/800 mA; board policy still gates
 * which supported function may be selected.
 */

#define SDHI_SWITCH_SPEED_HIGHSPEED_BIT     (1u << 1)
#define SDHI_CURRENT_200MA                   200
#define SDHI_CURRENT_400MA                   400
#define SDHI_CURRENT_600MA                   600
#define SDHI_CURRENT_800MA                   800
#define SDHI_CURRENT_200MA_FUNCTION          0
#define SDHI_CURRENT_400MA_FUNCTION          1
#define SDHI_CURRENT_600MA_FUNCTION          2
#define SDHI_CURRENT_800MA_FUNCTION          3
#define SDHI_CURRENT_200MA_BIT               (1u << 0)
#define SDHI_CURRENT_400MA_BIT               (1u << 1)
#define SDHI_CURRENT_600MA_BIT               (1u << 2)
#define SDHI_CURRENT_800MA_BIT               (1u << 3)

/* Nominal protocol/profile targets; board input clock and maximum SDCLK
 * remain in the immutable slot record. MMC retains the existing 20 MHz
 * profile. Divider selection may produce a lower actual frequency.
 */

#define SDHI_IDMODE_MAX_HZ                  400000
#define SDHI_SDR12_HZ                       25000000u
#define SDHI_HIGHSPEED_HZ                   50000000
#define SDHI_SDR50_HZ                       100000000u
#define SDHI_SDR104_HZ                      200000000u
#define SDHI_MMC_TRANSFER_HZ                20000000

/* Existing settling/tuning policy, not newly measured timing bounds. */

#define SDHI_UHS_DAT_LOW_WAIT_US            1000
#define SDHI_UHS_VOLTAGE_SETTLE_US          10000
#define SDHI_UHS_CLOCK_SETTLE_US            1000
#define SDHI_RESET_SETTLE_US                10000

/* SDR104 1:1 SCC uses eight phases; 16 probes sweep each phase twice.
 * A failure in either sweep rejects that phase. Three adjacent passing
 * phases give the chosen midpoint a passing neighbor on each side.
 * CMD19 uses a 150 ms transfer-event watchdog independently per request.
 * Host recovery consumes no scan deadline. This follows retained Linux
 * data.timeout_ns policy. It is not a calibrated whole-call bound and is
 * not PVT-qualified.
 */

#define SDHI_TUNING_TAPS                    8
#define SDHI_TUNING_PROBES                  16
#define SDHI_TUNING_TIMEOUT_MS              150
#define SDHI_TUNING_MIN_WINDOW              3
#define SDHI_TUNING_ALL_TAPS                0xff

/* SD CMD8 voltage/check-pattern fields and OCR operating-condition bits. */

#define SDHI_CMD8_CHECK_MASK                0xfff
#define SDHI_CMD8_CHECK_PATTERN             0x1aa
#define SDHI_OCR_VOLTAGE_MASK               0x00ffffff

/* Software status packing used by rzv2h_readpair/writepair:
 * INFO1[15:0] | (INFO2[15:0] << 16).  These are not native register
 * fields.  INFO1.HPIRES (bit 16) is outside this representation.
 */

#define SDHI_STATUS_RESPEND     SDHI_SD_INFO1_INFO0
#define SDHI_STATUS_ACCESSEND   SDHI_SD_INFO1_INFO2
#define SDHI_STATUS_PRESENT     SDHI_SD_INFO1_INFO5
#define SDHI_STATUS_INDEXERR    (SDHI_SD_INFO2_ERR0 << 16)
#define SDHI_STATUS_CRCERR      (SDHI_SD_INFO2_ERR1 << 16)
#define SDHI_STATUS_ENDERR      (SDHI_SD_INFO2_ERR2 << 16)
#define SDHI_STATUS_DATATIMEOUT (SDHI_SD_INFO2_ERR3 << 16)
#define SDHI_STATUS_ILLEGAL     (SDHI_SD_INFO2_ERR4 << 16)
#define SDHI_STATUS_BUFERR      (SDHI_SD_INFO2_ERR5 << 16)
#define SDHI_STATUS_RESPTIMEOUT (SDHI_SD_INFO2_ERR6 << 16)
#define SDHI_STATUS_BRE         (SDHI_SD_INFO2_BRE << 16)
#define SDHI_STATUS_BWE         (SDHI_SD_INFO2_BWE << 16)
#define SDHI_STATUS_CLKREADY    (SDHI_SD_INFO2_SCLKDIVEN << 16)
#define SDHI_STATUS_BUSY        (SDHI_SD_INFO2_CBSY << 16)
#define SDHI_STATUS_ACCESSERR   (SDHI_SD_INFO2_ILA << 16)
#define SDHI_STATUS_CMDERR \
  (SDHI_STATUS_INDEXERR | SDHI_STATUS_CRCERR | SDHI_STATUS_ENDERR | \
   SDHI_STATUS_RESPTIMEOUT | SDHI_STATUS_ACCESSERR)
#define SDHI_STATUS_CMDEVENTS   (SDHI_STATUS_RESPEND | SDHI_STATUS_CMDERR)
#define SDHI_STATUS_DATAERR \
  (SDHI_STATUS_INDEXERR | SDHI_STATUS_CRCERR | SDHI_STATUS_ENDERR | \
   SDHI_STATUS_DATATIMEOUT | SDHI_STATUS_ILLEGAL | SDHI_STATUS_BUFERR | \
   SDHI_STATUS_RESPTIMEOUT | SDHI_STATUS_ACCESSERR)
#define SDHI_STATUS_DATAEVENTS \
  (SDHI_STATUS_ACCESSEND | SDHI_STATUS_BRE | SDHI_STATUS_BWE | \
   SDHI_STATUS_DATAERR)
#define SDHI_STATUS_MASK_ALL \
  ((SDHI_SD_INFO1_MASK_ALL & 0xffffu) | (SDHI_SD_INFO2_MASK_ALL << 16))

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum rzv2h_xfrstate_e
{
  SDHI_XFR_IDLE = 0,
  SDHI_XFR_PREPARED,
  SDHI_XFR_ACTIVE,
  SDHI_XFR_DONE,
  SDHI_XFR_ERROR
};

struct rzv2h_sdio_s
{
  struct sdio_dev_s dev;             /* NuttX host interface */
  struct rzv2h_sdio_config_s config; /* Channel resources and CD/WP policy */
  uintptr_t base;                    /* Base address of SDHI MMIO registers */
  uint32_t command;                  /* Current NuttX command descriptor */
  uint32_t argument;                 /* Argument for the current command */
  uint32_t blockcount;               /* Accepted CMD23 count; zero if unset */
  uint32_t response[4];              /* Saved response; R2 uses all 4 words */
  uint32_t stopresponse;             /* Saved CMD12 response for later use */
  int result;                        /* Command result: OK or negative errno */
  int fault;                         /* Retained error blocking commands */
  sem_t waitsem;                     /* Semaphore waking the event waiter */
  struct wdog_s waitwdog;            /* Watchdog bounding the event wait */
  sdio_eventset_t waitevents;        /* Events requested by waitenable() */
  sdio_eventset_t wkupevent;         /* Events reported to eventwait() */
  bool highspeed;                    /* True if card selected High Speed */
  bool sdprobe;                      /* True if command is CMD55-led ACMD41 */
  bool sdcard;                       /* True if SD ACMD41 completed OK */
  bool selected;                     /* True if CMD7 selected the card */
  bool cmd6_supported;               /* True if SD CSD advertises class 10 */
  bool hs_checked;                   /* True if HS negotiation was attempted */
  bool hsfailed;                     /* True if card power recovery is needed */
#ifdef CONFIG_RZV2H_SDIO_UHS
  bool cmd8_valid;                   /* Valid SD CMD8 voltage/check echo */
  bool uhs_requested;                /* S18R was added to a real ACMD41 */
  bool uhs_ready;                    /* Mode, status and deselect completed */
  bool voltage18;                    /* CMD11 electrical switch completed */
  bool uhs_started;                  /* CMD11 issued: no implicit 3.3 V reset */
  bool uhs_failed;                   /* Full board/card power cycle required */
  bool tuning;                       /* Controlled CMD19 probe recovery */
  bool reinsert_needed;              /* Removal requires slot preparation */
  bool reinsert_ready;               /* Board profile prepared while empty */
  bool restarting;                   /* Keep host IRQ off during restart */
  uint8_t uhs_mode;                  /* CMD6 group 1: SDR12=0, SDR50=2, SDR104=3 */
  uint8_t tap;                       /* Selected SCC tap after tuning */
  uint16_t current_limit;            /* Verified CMD6 allowance in mA */
#endif
  enum rzv2h_xfrstate_e xfrstate;    /* PIO transfer lifecycle state */
  uint8_t *rxbuffer;                 /* Next destination byte for FIFO reads */
  const uint8_t *txbuffer;           /* Next source byte for FIFO writes */
#ifdef CONFIG_RZV2H_SDIO_DMA
  struct rzv2h_sdiodma_s dma;        /* Optional internal DMAC lifecycle */
#endif
  size_t remaining;                  /* Payload bytes still to move via FIFO */
  size_t total;                      /* Payload bytes in the prepared buffer */
  unsigned int blocklen;             /* Configured bytes per transfer block */
  unsigned int nblocks;              /* Configured number of transfer blocks */
  int xfrresult;                     /* Transfer status or negative errno */
  bool initialized;                  /* True if instance setup is complete */
  bool attached;                     /* True if host IRQ handler is attached */
  bool pending;                      /* True if awaiting command completion */
  bool clocked;                      /* True if SDCLK is enabled */
  bool geometry;                     /* True if block geometry is supported */
  bool read;                         /* True for card-to-host data transfers */
  bool datacmd;                      /* True if current command has data */
  bool multiactive;                  /* True if multiblock transfer is active */
  bool autostop;                     /* True if automatic CMD12 is selected */
  bool appcmd;                       /* True if CMD55 armed the next ACMD */
  bool setcount;                     /* True if command is CMD23, not ACMD23 */
  bool stopvalid;                    /* True if saved CMD12 response is valid */
  bool mediaremoved;                 /* True if absence blocks new commands */
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  struct work_s cdwork;              /* Work item for debounced media events */
  worker_t callback;                 /* Registered upper-half media callback */
  void *cbarg;                       /* Argument passed to the media callback */

  /* Media events enabled for one callback notification */

  sdio_eventset_t cbevents;
  bool cbclosing;                    /* True during callback replace/removal */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register access and controller readiness */

static uint16_t rzv2h_read16(struct rzv2h_sdio_s *priv, unsigned int offset);
static void rzv2h_write16(struct rzv2h_sdio_s *priv, unsigned int offset,
                          uint16_t value);
static uint32_t rzv2h_readpair(struct rzv2h_sdio_s *priv,
                               unsigned int offset);
static void rzv2h_writepair(struct rzv2h_sdio_s *priv, unsigned int offset,
                            uint32_t value);
static int rzv2h_idle(struct rzv2h_sdio_s *priv, bool clock);
static int rzv2h_setclock(struct rzv2h_sdio_s *priv, uint32_t target);
static void rzv2h_mask(struct rzv2h_sdio_s *priv);
static void rzv2h_updatemask(struct rzv2h_sdio_s *priv);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static void rzv2h_mediaworker(void *arg);
static void rzv2h_mediaremoved(struct rzv2h_sdio_s *priv);
#endif

/* Transfer state, FIFO service and interrupt handling */

static void rzv2h_endwait(struct rzv2h_sdio_s *priv,
                          sdio_eventset_t event);

static void rzv2h_endtransfer(struct rzv2h_sdio_s *priv, int result);
static bool rzv2h_dataresponse_error(struct rzv2h_sdio_s *priv);
static void rzv2h_receive(struct rzv2h_sdio_s *priv);
static void rzv2h_transmit(struct rzv2h_sdio_s *priv);
static void rzv2h_eventtimeout(wdparm_t arg);
static void rzv2h_complete(struct rzv2h_sdio_s *priv);
static int rzv2h_interrupt(int irq, void *context, void *arg);
static int rzv2h_setuptransfer(struct rzv2h_sdio_s *priv, bool read,
                               uint8_t *rxbuffer,
                               const uint8_t *txbuffer, size_t nbytes);

/* SDIO host configuration methods */

static int rzv2h_initialize_config(
  const struct rzv2h_sdio_config_s *config, struct sdio_dev_s **dev);
static void rzv2h_reset_host(struct rzv2h_sdio_s *priv, bool preserve);

static void rzv2h_reset(struct sdio_dev_s *dev);
static sdio_capset_t rzv2h_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t rzv2h_status(struct sdio_dev_s *dev);
static void rzv2h_widebus(struct sdio_dev_s *dev, bool enable);
static void rzv2h_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int rzv2h_attach(struct sdio_dev_s *dev);

/* SD-memory High Speed negotiation helpers */

#if defined(CONFIG_RZV2H_SDIO_HIGHSPEED) || defined(CONFIG_RZV2H_SDIO_UHS)
static int rzv2h_sd_switch(struct rzv2h_sdio_s *priv, uint32_t arg,
                            uint8_t status[64]);
#ifdef CONFIG_RZV2H_SDIO_HIGHSPEED
static int rzv2h_sd_highspeed(struct rzv2h_sdio_s *priv);
#endif
#endif

/* SDIO command and response methods */

static int rzv2h_sendcmd(struct sdio_dev_s *dev, uint32_t cmd, uint32_t arg);
static int rzv2h_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int rzv2h_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t *response);
static int rzv2h_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t response[4]);

/* SDIO data setup, completion and cancellation methods */

#ifdef CONFIG_SDIO_BLOCKSETUP
static void rzv2h_blocksetup(struct sdio_dev_s *dev, unsigned int blocklen,
                             unsigned int nblocks);
#endif
static int rzv2h_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                            size_t nbytes);
static int rzv2h_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                            size_t nbytes);
#ifdef CONFIG_RZV2H_SDIO_DMA
static int rzv2h_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                             size_t nbytes);
static int rzv2h_dmasendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                             size_t nbytes);
#endif
static int rzv2h_cancel(struct sdio_dev_s *dev);
static void rzv2h_waitenable(struct sdio_dev_s *dev,
                             sdio_eventset_t events, uint32_t timeout);
static sdio_eventset_t rzv2h_eventwait(struct sdio_dev_s *dev);

#ifdef CONFIG_RZV2H_SDIO_UHS
static int rzv2h_switchvoltage(struct sdio_dev_s *dev);
static int rzv2h_uhs_initialize(struct rzv2h_sdio_s *priv);
static int rzv2h_uhs_fail(struct rzv2h_sdio_s *priv, int error);
static int rzv2h_prepare_reinsert(struct rzv2h_sdio_s *priv);
static int rzv2h_finish_reinsert(struct rzv2h_sdio_s *priv);
#endif

/* SDIO media notification and optional bus locking methods */

static void rzv2h_callbackenable(struct sdio_dev_s *dev,
                                 sdio_eventset_t events);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rzv2h_registercallback(struct sdio_dev_s *dev,
                                   worker_t callback, void *arg);
#endif
#ifdef CONFIG_SDIO_MUXBUS
static int rzv2h_lock(struct sdio_dev_s *dev, bool lock);
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Declared for board callers. */

int rzv2h_sdio_initialize(int channel, struct sdio_dev_s **dev);
int rzv2h_sdio_uninitialize(struct sdio_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct sdio_dev_s g_sdiodev =
{
#ifdef CONFIG_SDIO_MUXBUS
  .lock             = rzv2h_lock,
#endif
  .reset            = rzv2h_reset,
  .capabilities     = rzv2h_capabilities,
  .status           = rzv2h_status,
  .widebus          = rzv2h_widebus,
  .clock            = rzv2h_clock,
  .attach           = rzv2h_attach,
  .sendcmd          = rzv2h_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
  .blocksetup       = rzv2h_blocksetup,
#endif
  .recvsetup        = rzv2h_recvsetup,
  .sendsetup        = rzv2h_sendsetup,
  .cancel           = rzv2h_cancel,
  .waitresponse     = rzv2h_waitresponse,
  .recv_r1          = rzv2h_recvshort,
  .recv_r2          = rzv2h_recvlong,
  .recv_r3          = rzv2h_recvshort,
  .recv_r4          = rzv2h_recvshort,
  .recv_r5          = rzv2h_recvshort,
  .recv_r6          = rzv2h_recvshort,
  .recv_r7          = rzv2h_recvshort,
  .waitenable       = rzv2h_waitenable,
  .eventwait        = rzv2h_eventwait,
  .callbackenable   = rzv2h_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  .registercallback = rzv2h_registercallback,
#endif
#ifdef CONFIG_SDIO_DMA
#ifdef CONFIG_RZV2H_SDIO_DMA
  .dmarecvsetup     = rzv2h_dmarecvsetup,
  .dmasendsetup     = rzv2h_dmasendsetup,
#else
  .dmarecvsetup     = rzv2h_recvsetup,
  .dmasendsetup     = rzv2h_sendsetup,
#endif
#endif
};

static struct rzv2h_sdio_s g_sdio[3];

static mutex_t g_initlock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rzv2h_sdio_waitreg
 *
 * Description:
 *   Wait for a masked CPG monitor field within the existing poll budget.
 *
 ****************************************************************************/

static int rzv2h_sdio_waitreg(const volatile uint32_t *reg, uint32_t mask,
                              uint32_t expected)
{
  unsigned int elapsed;

  for (elapsed = 0; elapsed < RZV2H_SDHI_CPG_TIMEOUT_US; elapsed++)
    {
      if ((*reg & mask) == expected)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rzv2h_read16
 *
 * Description:
 *   Read the low halfword of one SDHI register slot.
 *
 *   Only the low halfword of each eight-byte register slot is used here.
 *   Do not replace these with contiguous 32-bit accesses.
 *
 *   Use a 16-bit MMIO access at the supplied controller-relative offset.
 *
 ****************************************************************************/

static uint16_t rzv2h_read16(struct rzv2h_sdio_s *priv, unsigned int offset)
{
  return getreg16(priv->base + offset);
}

/****************************************************************************
 * Name: rzv2h_write16
 *
 * Description:
 *   Write the low halfword of one SDHI register slot.
 *
 *   Use a 16-bit MMIO access so the unused portion of the slot is not
 *   written.
 *
 ****************************************************************************/

static void rzv2h_write16(struct rzv2h_sdio_s *priv, unsigned int offset,
                          uint16_t value)
{
  putreg16(value, priv->base + offset);
}

/****************************************************************************
 * Name: rzv2h_readpair
 *
 * Description:
 *   Pack two spaced SDHI halfword registers into one software value.
 *
 *   Place the first register in bits 15:0 and the register eight bytes
 *   later in bits 31:16. The result is a software representation, not
 *   one native 32-bit register.
 *
 ****************************************************************************/

static uint32_t rzv2h_readpair(struct rzv2h_sdio_s *priv,
                               unsigned int offset)
{
  return rzv2h_read16(priv, offset) |
         ((uint32_t)rzv2h_read16(priv, offset + 8) << 16);
}

/****************************************************************************
 * Name: rzv2h_writepair
 *
 * Description:
 *   Split a software value across two spaced SDHI halfword registers.
 *
 *   Write the low and high halfwords separately at offsets eight bytes
 *   apart. For status acknowledgements, retain only supported W0C bits
 *   and set the required-one INFO2 bit. Other register pairs preserve
 *   the supplied value without masking.
 *
 ****************************************************************************/

static void rzv2h_writepair(struct rzv2h_sdio_s *priv, unsigned int offset,
                            uint32_t value)
{
  if (offset == RZV2H_SDHI_SD_INFO1)
    {
      /* Apply the reserved-field policy to status writes only. Do not
       * read/modify/write W0C status: a new event could be lost. INFO1
       * HPIRES bit16 is outside this paired low16 register interface.
       */

      rzv2h_write16(priv, offset,
                     (uint16_t)value & SDHI_SD_INFO1_W0C_MASK);
      rzv2h_write16(priv, offset + 8,
                     ((value >> 16) & SDHI_SD_INFO2_W0C_MASK) |
                     SDHI_SD_INFO2_RESERVED_ONE);
      return;
    }

  rzv2h_write16(priv, offset, value);
  rzv2h_write16(priv, offset + 8, value >> 16);
}

/****************************************************************************
 * Name: rzv2h_updatemask
 *
 * Description:
 *   Enable only the interrupts needed by current command, data and media
 *   state.
 *
 *   SD_INFO masks use zero to enable an interrupt. Build the mask from
 *   the command and transfer states so completing one phase cannot
 *   accidentally disable the other phase.
 *
 *   Combine command, transfer and card-detect requirements before
 *   writing the active-low masks. DMA transfers do not enable the PIO
 *   FIFO-ready sources.
 *
 ****************************************************************************/

static void rzv2h_updatemask(struct rzv2h_sdio_s *priv)
{
  uint32_t mask = SDHI_STATUS_MASK_ALL;

  if (priv->attached && priv->pending)
    {
      mask &= ~SDHI_STATUS_CMDEVENTS;
    }

  if (priv->attached && priv->xfrstate == SDHI_XFR_ACTIVE)
    {
      mask &= ~(SDHI_STATUS_ACCESSEND | SDHI_STATUS_DATAERR);
#ifdef CONFIG_RZV2H_SDIO_DMA
      if (!priv->dma.prepared)
#endif
        {
          mask &= ~(priv->read ? SDHI_STATUS_BRE : SDHI_STATUS_BWE);
        }
    }

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  if (priv->config.cd_source == RZV2H_CD_NATIVE && priv->attached &&
      priv->callback != NULL && !priv->cbclosing)
    {
      mask &= ~SDHI_STATUS_CD;
    }
#endif

  rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1_MASK, mask);
}

/****************************************************************************
 * Name: rzv2h_endwait
 *
 * Description:
 *   Publish one wake event and retire the watchdog-backed wait.
 *
 *   This function is called with interrupts disabled or from
 *   interrupt context. A semaphore post is safe in both cases.
 *
 *   Ignore an already completed or unarmed wait. Filter the event
 *   against the requested mask, retain error notification and post the
 *   semaphore once.
 *
 ****************************************************************************/

static void rzv2h_endwait(struct rzv2h_sdio_s *priv,
                          sdio_eventset_t event)
{
  sdio_eventset_t wake;

  if (priv->waitevents == 0 || priv->wkupevent != 0)
    {
      return;
    }

  wake = event & priv->waitevents;
  if ((event & SDIOWAIT_ERROR) != 0)
    {
      wake |= SDIOWAIT_ERROR;
    }

  if (wake == 0)
    {
      return;
    }

  wd_cancel(&priv->waitwdog);
  priv->waitevents = 0;
  priv->wkupevent = wake;
  nxsem_post(&priv->waitsem);
}

#ifdef CONFIG_RZV2H_SDIO_DMA
/****************************************************************************
 * Name: rzv2h_stopdma
 *
 * Description:
 *   Stop internal DMA and retain a fault if it was still running.
 *
 *   Delegate hardware shutdown to the DMA backend. Interrupting a
 *   running DMA retains a host fault rather than allowing a later
 *   command to assume clean completion.
 *
 ****************************************************************************/

static void rzv2h_stopdma(struct rzv2h_sdio_s *priv)
{
  bool running = priv->dma.running;
  int ret;

  ret = rzv2h_sdiodma_stop(&priv->dma);
  if (ret < 0)
    {
      /* Do not let callers release a buffer after failed reset confirmation.
       * Keep the existing fatal policy here, outside the DMA mechanism.
       * Its platform containment contract still requires qualification.
       */

      priv->fault = ret;
      mcerr("ERROR: SDHI%u DMA %s reset not confirmed: offset=%04x "
            "value=%08" PRIx32 "; buffer ownership retained\n",
            priv->config.channel, priv->dma.read ? "RX" : "TX",
            (unsigned int)priv->dma.stop_offset, priv->dma.stop_value);
      PANIC();
    }

  if (running)
    {
      priv->fault = -EIO;
    }
}
#endif

/****************************************************************************
 * Name: rzv2h_endtransfer
 *
 * Description:
 *   Release transfer buffers, publish the result and wake the waiter.
 *
 *   Finish any DMA staging, save the terminal transfer result and clear
 *   buffer ownership. Update interrupt masks and report completion or
 *   failure to the waiting task.
 *
 ****************************************************************************/

static void rzv2h_endtransfer(struct rzv2h_sdio_s *priv, int result)
{
#ifdef CONFIG_RZV2H_SDIO_DMA
  result = rzv2h_sdiodma_finish(&priv->dma, result);
  rzv2h_stopdma(priv);
#endif
  priv->xfrresult = result;
  priv->xfrstate = result == OK ? SDHI_XFR_DONE : SDHI_XFR_ERROR;
#ifdef CONFIG_RZV2H_SDIO_UHS
  if (result < 0 && priv->voltage18 && !priv->tuning)
    {
      rzv2h_uhs_fail(priv, result);
    }
#endif

  priv->rxbuffer = NULL;
  priv->txbuffer = NULL;
  rzv2h_updatemask(priv);
  rzv2h_endwait(priv, result == OK ? SDIOWAIT_TRANSFERDONE :
                                     SDIOWAIT_ERROR);
}

/****************************************************************************
 * Name: rzv2h_dataresponse_error
 *
 * Description:
 *   Check the saved R1/R1b or R5 response for a card-reported error.
 *
 *   Interpret card error bits according to the saved response type.
 *   Response types without an R1/R1b or R5 card-error field return false
 *   here.
 *
 ****************************************************************************/

static bool rzv2h_dataresponse_error(struct rzv2h_sdio_s *priv)
{
  switch (priv->command & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_R1_RESPONSE:
      case MMCSD_R1B_RESPONSE:
        return (priv->response[0] & SDHI_R1_ERRORMASK) != 0;

      case MMCSD_R5_RESPONSE:
        return (priv->response[0] & SDHI_R5_ERRORMASK) != 0;

      default:
        return false;
    }
}

/****************************************************************************
 * Name: rzv2h_receive
 *
 * Description:
 *   Drain one ready FIFO bank into the PIO receive buffer.
 *
 *   The read-ready flag represents one SD_SIZE-sized buffer bank. Clear it
 *   before touching SD_BUF0 as required by the hardware operation
 *   sequence.
 *
 *   Copy full FIFO words efficiently for aligned buffers and use byte-
 *   safe copies for unaligned data or a final partial word. Advance the
 *   receive pointer and remaining count.
 *
 ****************************************************************************/

static void rzv2h_receive(struct rzv2h_sdio_s *priv)
{
  size_t block = priv->remaining < priv->blocklen ?
                 priv->remaining : priv->blocklen;
  size_t offset;

  /* Keep the full-word copy size constant so the compiler can move each
   * FIFO word directly between a CPU register and aligned caller RAM.
   * memcpy avoids imposing a uint32_t alias on the caller's byte buffer.
   */

  if (((uintptr_t)priv->rxbuffer & 3) == 0)
    {
      while (block >= sizeof(uint32_t))
        {
          uint32_t word = getreg32(priv->base + RZV2H_SDHI_SD_BUF0);

          memcpy(priv->rxbuffer, &word, sizeof(word));
          priv->rxbuffer += sizeof(word);
          priv->remaining -= sizeof(word);
          block -= sizeof(word);
        }
    }

  /* Byte-safe copies for unaligned buffers and the final tail. */

  for (offset = 0; offset < block; offset += sizeof(uint32_t))
    {
      uint32_t word;
      size_t count = block - offset;

      if (count > sizeof(word))
        {
          count = sizeof(word);
        }

      word = getreg32(priv->base + RZV2H_SDHI_SD_BUF0);
      memcpy(priv->rxbuffer, &word, count);
      priv->rxbuffer += count;
      priv->remaining -= count;
    }
}

/****************************************************************************
 * Name: rzv2h_transmit
 *
 * Description:
 *   Service one ready TX bank; the caller acknowledges BWE first.
 *
 *   Fill at most one configured block from the transmit buffer. Advance
 *   its pointer and remaining count, zeroing unused bytes of a final
 *   partial FIFO word.
 *
 ****************************************************************************/

static void rzv2h_transmit(struct rzv2h_sdio_s *priv)
{
  size_t block = priv->remaining < priv->blocklen ?
                 priv->remaining : priv->blocklen;
  size_t offset;

  /* Keep the full-word copy size constant so the compiler can move each
   * FIFO word directly between a CPU register and aligned caller RAM.
   * memcpy avoids imposing a uint32_t alias on the caller's byte buffer.
   */

  if (((uintptr_t)priv->txbuffer & 3) == 0)
    {
      while (block >= sizeof(uint32_t))
        {
          uint32_t word;

          memcpy(&word, priv->txbuffer, sizeof(word));
          putreg32(word, priv->base + RZV2H_SDHI_SD_BUF0);
          priv->txbuffer += sizeof(word);
          priv->remaining -= sizeof(word);
          block -= sizeof(word);
        }
    }

  /* Keep unused bytes of a partial TX word zero. */

  for (offset = 0; offset < block; offset += sizeof(uint32_t))
    {
      uint32_t word = 0;
      size_t count = block - offset;

      if (count > sizeof(word))
        {
          count = sizeof(word);
        }

      memcpy(&word, priv->txbuffer, count);
      putreg32(word, priv->base + RZV2H_SDHI_SD_BUF0);
      priv->txbuffer += count;
      priv->remaining -= count;
    }
}

/****************************************************************************
 * Name: rzv2h_eventtimeout
 *
 * Description:
 *   Retire a timed-out prepared or active transfer and wake its waiter.
 *
 *   Retire the pending data command before releasing its buffers so a
 *   late response cannot reactivate it. The watchdog path wakes the
 *   task; task-side cancellation performs controller recovery.
 *
 ****************************************************************************/

static void rzv2h_eventtimeout(wdparm_t arg)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)arg;

  if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0 &&
      priv->wkupevent == 0)
    {
      if (priv->xfrstate == SDHI_XFR_ACTIVE ||
          priv->xfrstate == SDHI_XFR_PREPARED)
        {
#ifdef CONFIG_RZV2H_SDIO_UHS
          if (priv->voltage18 && !priv->tuning)
            {
              rzv2h_uhs_fail(priv, -ETIMEDOUT);
            }
#endif

#ifdef CONFIG_RZV2H_SDIO_DMA
          rzv2h_stopdma(priv);
#endif
          /* Retire the wire command before releasing its buffers. A late
           * response must not activate this transfer or arm a stale CMD23.
           * Task-side eventwait/cancel performs controller recovery.
           */

          priv->pending = false;
          priv->result = -ETIMEDOUT;
          priv->datacmd = false;
          priv->setcount = false;
          priv->blockcount = 0;
          priv->appcmd = false;
          priv->xfrresult = -ETIMEDOUT;
          priv->xfrstate = SDHI_XFR_ERROR;
          priv->rxbuffer = NULL;
          priv->txbuffer = NULL;
          rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1,
                           ~(SDHI_STATUS_CMDEVENTS |
                             SDHI_STATUS_DATAEVENTS));
          rzv2h_updatemask(priv);
        }

      rzv2h_endwait(priv, SDIOWAIT_TIMEOUT);
    }
}

/****************************************************************************
 * Name: rzv2h_idle
 *
 * Description:
 *   Poll until command and optional clock readiness permit register
 *   changes.
 *
 *   These waits run with interrupts enabled. CBSY and SCLKDIVEN have
 *   opposite polarity. An expired wait is an error, never permission to
 *   write anyway.
 *
 *   Use the finite idle-poll budget with interrupts enabled. Return OK
 *   only when the required busy/clock conditions hold, or -EBUSY when
 *   the budget is exhausted.
 *
 ****************************************************************************/

static int rzv2h_idle(struct rzv2h_sdio_s *priv, bool clock)
{
  uint32_t retries = SDHI_IDLE_POLLS;
  uint32_t info;

  do
    {
      info = rzv2h_readpair(priv, RZV2H_SDHI_SD_INFO1);
      if (clock ? (info & (SDHI_STATUS_CLKREADY | SDHI_STATUS_BUSY)) ==
          SDHI_STATUS_CLKREADY : (info & SDHI_STATUS_BUSY) == 0)
        {
          return OK;
        }
    }
  while (--retries > 0);

  return -EBUSY;
}

/****************************************************************************
 * Name: rzv2h_mask
 *
 * Description:
 *   Mask SD-memory and SDIO interrupt sources before reset or IRQ
 *   teardown.
 *
 *   Write the reset masks to the SD-memory status pair and the separate
 *   SDIO interrupt-mask register. This helper does not change the GIC
 *   route.
 *
 ****************************************************************************/

static void rzv2h_mask(struct rzv2h_sdio_s *priv)
{
  rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1_MASK, SDHI_STATUS_MASK_ALL);
  rzv2h_write16(priv, RZV2H_SDHI_SDIO_INFO1_MASK, SDHI_SDIO_INFO1_MASK_ALL);
}

/****************************************************************************
 * Name: rzv2h_setclock
 *
 * Description:
 *   Program the host clock divider without issuing card commands.
 *
 *   Register-only clock programming. The caller owns the serialized
 *   host. Keep continuous clocks, including the required post-CMD6 clock
 *   interval.
 *
 *   Reject an in-flight operation, wait for safe register access and
 *   choose a supported divider within board limits. A zero target stops
 *   SDCLK; register readback verifies the programmed state.
 *
 ****************************************************************************/

static int rzv2h_setclock(struct rzv2h_sdio_s *priv, uint32_t target)
{
  unsigned int divider = 2;
  uint16_t encoding = 0;
  int ret;

  if (target > priv->config.max_frequency)
    {
      target = priv->config.max_frequency;
    }

  if (priv->pending || priv->xfrstate == SDHI_XFR_ACTIVE ||
      priv->xfrstate == SDHI_XFR_PREPARED)
    {
      return -EBUSY;
    }

  ret = rzv2h_idle(priv, true);
  if (ret < 0)
    {
      return ret;
    }

  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, 0);
  priv->clocked = false;
  if (target == 0)
    {
      return OK;
    }

#ifdef CONFIG_RZV2H_SDIO_UHS
  if (priv->voltage18 && priv->uhs_mode == SDHI_SWITCH_SPEED_SDR104 &&
      target >= priv->config.input_clock)
    {
      divider = 1;
      encoding = SDHI_SD_CLK_CTRL_DIV1;
    }
#endif

  while (priv->config.input_clock > (uint64_t)target * divider &&
         divider < SDHI_SD_CLK_CTRL_MAX_DIVIDER)
    {
      divider <<= 1;
      encoding = divider >> 2;
    }

  if (priv->config.input_clock > (uint64_t)target * divider)
    {
      return -ERANGE;
    }

  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, encoding);
  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL,
                encoding | SDHI_SD_CLK_CTRL_SCLKEN);
  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
      (SDHI_SD_CLK_CTRL_DIV_MASK | SDHI_SD_CLK_CTRL_SCLKEN)) !=
      (encoding | SDHI_SD_CLK_CTRL_SCLKEN))
    {
      return -EIO;
    }

  priv->clocked = true;
  return OK;
}

/****************************************************************************
 * Name: rzv2h_clock
 *
 * Description:
 *   Apply the requested MMCSD clock mode and negotiate SD timing when
 *   needed.
 *
 *   Translate the upper-half clock request using immutable slot policy.
 *   Record errors in host state because this callback has no return
 *   value, and preserve UHS recovery restrictions.
 *
 ****************************************************************************/

static void rzv2h_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint32_t target;
  int ret;

  if (priv->pending || priv->xfrstate == SDHI_XFR_ACTIVE ||
      priv->xfrstate == SDHI_XFR_PREPARED)
    {
      priv->fault = -EBUSY;
      return;
    }

#ifdef CONFIG_RZV2H_SDIO_UHS
  if (rate == CLOCK_IDMODE && priv->reinsert_needed)
    {
      priv->fault = rzv2h_finish_reinsert(priv);
      return;
    }

  if (priv->uhs_failed || (priv->uhs_started &&
      rate == CLOCK_IDMODE && !priv->tuning))
    {
      rzv2h_uhs_fail(priv, -EIO);
      return;
    }

  if (priv->voltage18 && (rate == CLOCK_SD_TRANSFER_1BIT ||
                         rate == CLOCK_SD_TRANSFER_4BIT))
    {
      if (!priv->uhs_ready)
        {
          rzv2h_uhs_fail(priv, -EIO);
          return;
        }

      target = priv->uhs_mode == SDHI_SWITCH_SPEED_SDR104 ? SDHI_SDR104_HZ :
               priv->uhs_mode ==
                 SDHI_SWITCH_SPEED_SDR50 ? SDHI_SDR50_HZ : SDHI_SDR12_HZ;
      if (target > CONFIG_RZV2H_SDIO_MAX_FREQUENCY)
        {
          target = CONFIG_RZV2H_SDIO_MAX_FREQUENCY;
        }

      ret = rzv2h_setclock(priv, target);

      if (ret < 0)
        {
          rzv2h_uhs_fail(priv, ret);
        }

      return;
    }
#endif

  if (rate == CLOCK_IDMODE)
    {
      priv->highspeed = false;
      priv->sdcard = false;
      priv->selected = false;
      priv->cmd6_supported = false;
      priv->hs_checked = false;
    }

  if (priv->hsfailed && rate != CLOCK_SDIO_DISABLED)
    {
      priv->fault = -EIO;
      return;
    }

  switch (rate)
    {
      case CLOCK_SDIO_DISABLED:
        target = 0;
        break;
      case CLOCK_IDMODE:
        target = SDHI_IDMODE_MAX_HZ;
        break;
      case CLOCK_MMC_TRANSFER:
        target = SDHI_MMC_TRANSFER_HZ;
        break;
      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
        target = priv->highspeed ?
                 SDHI_HIGHSPEED_HZ :
                 SDHI_SDR12_HZ;
#ifdef CONFIG_RZV2H_SDIO_MAX_FREQUENCY
        if (target > CONFIG_RZV2H_SDIO_MAX_FREQUENCY)
          {
            target = CONFIG_RZV2H_SDIO_MAX_FREQUENCY;
          }
#endif
        break;
      default:
        priv->fault = -ENOTSUP;
        return;
    }

  ret = rzv2h_setclock(priv, target);
  if (ret < 0)
    {
      priv->fault = ret;
      return;
    }

#ifdef CONFIG_RZV2H_SDIO_HIGHSPEED
  if ((rate == CLOCK_SD_TRANSFER_1BIT || rate == CLOCK_SD_TRANSFER_4BIT) &&
      priv->sdcard && priv->selected && priv->cmd6_supported &&
      !priv->hs_checked)
    {
      /* The existing upper half owns the slot lock while requesting the
       * transfer clock. Negotiate once, at the current Default-safe rate.
       * CSD class 10 establishes CMD6 availability without reading SCR here.
       */

      priv->hs_checked = true;
      ret = rzv2h_sd_highspeed(priv);
      if (ret == OK && priv->highspeed)
        {
          target = SDHI_HIGHSPEED_HZ;
#ifdef CONFIG_RZV2H_SDIO_MAX_FREQUENCY
          if (target > CONFIG_RZV2H_SDIO_MAX_FREQUENCY)
            {
              target = CONFIG_RZV2H_SDIO_MAX_FREQUENCY;
            }
#endif

          ret = rzv2h_setclock(priv, target);
        }

      if (ret < 0)
        {
          /* Removal already blocks access and invalidates the old card.
           * Do not retain its fault for a subsequently inserted replacement.
           */

          priv->hsfailed = !priv->mediaremoved;
          priv->fault = priv->mediaremoved ? OK : ret;
          mcerr("SD High Speed failed; recover card power: %d\n", ret);
        }
    }
#endif
}

#if defined(CONFIG_RZV2H_SDIO_HIGHSPEED) || defined(CONFIG_RZV2H_SDIO_UHS)
/****************************************************************************
 * Name: rzv2h_readblock64
 *
 * Description:
 *   Receive one CMD6 status or CMD19 tuning block through the PIO path.
 *
 *   SD Physical Layer 9.10 4.3.10: ordinary CMD6 R1/read, one 64-byte
 *   status block, 100 ms data bound. Do not issue CMD55, CMD16 or CMD12.
 *
 *   Prepare one 64-byte read, arm transfer events, send the command and
 *   validate its R1 response before waiting for data. Return a negative
 *   error on failure and leave terminal-event recovery to eventwait. The
 *   caller supplies the transfer-event timeout: CMD6 uses 100 ms and CMD19
 *   uses 150 ms, following the retained Renesas Linux tuning policy.
 *
 ****************************************************************************/

static int rzv2h_readblock64(struct rzv2h_sdio_s *priv, uint32_t cmd,
                             uint32_t arg, uint8_t status[64],
                             uint32_t timeout_ms)
{
  struct sdio_dev_s *dev = &priv->dev;
  sdio_eventset_t event;
  uint32_t response;
  int ret;

  rzv2h_blocksetup(dev, 64, 1);
  rzv2h_waitenable(dev, SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT |
                    SDIOWAIT_ERROR, timeout_ms);
  ret = rzv2h_recvsetup(dev, status, 64);
  if (ret < 0)
    {
      goto cancel;
    }

  ret = rzv2h_sendcmd(dev, cmd, arg);
  if (ret == OK)
    {
      ret = rzv2h_waitresponse(dev, cmd);
    }

  if (ret == OK)
    {
      ret = rzv2h_recvshort(dev, cmd, &response);
      if (ret == OK && (response & SDHI_R1_ERRORMASK) != 0)
        {
          ret = -EIO;
        }
    }

  if (ret < 0)
    {
      goto cancel;
    }

  event = rzv2h_eventwait(dev);
  ret = (event & SDIOWAIT_TIMEOUT) != 0 ? -ETIMEDOUT :
        (event & SDIOWAIT_ERROR) != 0 ? -EIO :
        (event & SDIOWAIT_TRANSFERDONE) != 0 ? OK : -EIO;
  if (ret == OK || (event & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) != 0)
    {
      /* eventwait owns recovery for terminal errors/timeouts.  Repeating
       * cancellation here resets the host twice for each rejected tuning
       * probe.  Earlier failures and unexpected events still need cleanup.
       */

      return ret;
    }

cancel:
  rzv2h_cancel(dev);
  return ret;
}

/****************************************************************************
 * Name: rzv2h_sd_switch
 *
 * Description:
 *   Issue ordinary CMD6 and return its 64-byte switch-function status.
 *
 *   Encode CMD6 as an ordinary R1 data-read command and pass the
 *   supplied argument to the shared 64-byte read helper. This is not
 *   application command ACMD6.
 *
 ****************************************************************************/

static int rzv2h_sd_switch(struct rzv2h_sdio_s *priv, uint32_t arg,
                            uint8_t status[64])
{
  const uint32_t cmd = MMCSD_CMDIDX6 | MMCSD_R1_RESPONSE | MMCSD_RDDATAXFR;

  return rzv2h_readblock64(priv, cmd, arg, status, 100);
}

#ifdef CONFIG_RZV2H_SDIO_HIGHSPEED
/****************************************************************************
 * Name: rzv2h_sd_highspeed
 *
 * Description:
 *   Check and select SD high speed, recording whether the card accepted
 *   it.
 *
 *   Check CMD6 support, current requirements and the selected-function
 *   result before switching. An unsupported high-speed selection may
 *   return OK with highspeed still false, retaining default-speed
 *   operation.
 *
 ****************************************************************************/

static int rzv2h_sd_highspeed(struct rzv2h_sdio_s *priv)
{
  uint32_t words[16];
  uint8_t *status = (uint8_t *)words;
  unsigned int power;
  int ret;

  if (up_interrupt_context() || !priv->attached)
    {
      return -EPERM;
    }

  /* Send CHECK. */

  ret = rzv2h_sd_switch(priv, SDHI_SWITCH_CHECK_HIGHSPEED, status);
  if (ret < 0)
    {
      return ret;
    }

  if (status[SDHI_SWITCH_STATUS_VERSION] > SDHI_SWITCH_VERSION_BUSY)
    {
      return -EIO;
    }

  power = ((unsigned int)status[SDHI_SWITCH_POWER_HI] <<
      SDHI_SWITCH_POWER_SHIFT) |
          status[SDHI_SWITCH_POWER_LO];
  if ((status[SDHI_SWITCH_ACCESS_SUPPORT] &
       SDHI_SWITCH_SPEED_HIGHSPEED_BIT) == 0 ||
      (status[SDHI_SWITCH_ACCESS_RESULT] &
       SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
      SDHI_SWITCH_SPEED_HIGHSPEED ||
      power == 0 || power > SDHI_CURRENT_200MA ||
      (status[SDHI_SWITCH_STATUS_VERSION] == SDHI_SWITCH_VERSION_BUSY &&
      (status[SDHI_SWITCH_ACCESS_BUSY] & SDHI_SWITCH_SPEED_HIGHSPEED_BIT) !=
        0))
    {
      return OK;
    }

  /* Send SWITCH. */

  ret = rzv2h_sd_switch(priv, SDHI_SWITCH_SET_HIGHSPEED, status);
  if (ret < 0)
    {
      return ret;
    }

  power = ((unsigned int)status[SDHI_SWITCH_POWER_HI] <<
      SDHI_SWITCH_POWER_SHIFT) |
          status[SDHI_SWITCH_POWER_LO];
  if (status[SDHI_SWITCH_STATUS_VERSION] > SDHI_SWITCH_VERSION_BUSY ||
      (status[SDHI_SWITCH_ACCESS_SUPPORT] &
       SDHI_SWITCH_SPEED_HIGHSPEED_BIT) == 0 ||
      power == 0 || power > SDHI_CURRENT_200MA ||
      (status[SDHI_SWITCH_STATUS_VERSION] == SDHI_SWITCH_VERSION_BUSY &&
      (status[SDHI_SWITCH_ACCESS_BUSY] & SDHI_SWITCH_SPEED_HIGHSPEED_BIT) !=
        0))
    {
      return -EIO;
    }

  if ((status[SDHI_SWITCH_ACCESS_RESULT] &
      SDHI_SWITCH_GROUP_FUNCTION_MASK) ==
      SDHI_SWITCH_SPEED_DEFAULT)
    {
      return OK;
    }

  if ((status[SDHI_SWITCH_ACCESS_RESULT] &
      SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
      SDHI_SWITCH_SPEED_HIGHSPEED)
    {
      return -EIO;
    }

  priv->highspeed = true;
  return OK;
}
#endif
#endif

#ifdef CONFIG_RZV2H_SDIO_UHS
/****************************************************************************
 * Name: rzv2h_uhs_fail
 *
 * Description:
 *   Retain a UHS failure and block further I/O without resetting card
 *   voltage.
 *
 *   Fail closed without pretending a controller reset resets card
 *   voltage. A full board/card power cycle is required; reset/cancel
 *   cannot clear this.
 *
 *   Store the failure, mark private UHS preparation as failed and
 *   prevent normal reuse of that session. Stop the clock only when
 *   writable; otherwise assert interface reset without waiting. Retain
 *   observed clock state if shutdown cannot be confirmed. Controller reset
 *   is not evidence that the external card returned to its original voltage.
 *
 ****************************************************************************/

static int rzv2h_uhs_fail(struct rzv2h_sdio_s *priv, int error)
{
  irqstate_t flags = enter_critical_section();
  uint16_t info;

  priv->uhs_failed = true;
  priv->current_limit = 0;
  priv->fault = error < 0 ? error : -EIO;
  info = rzv2h_read16(priv, RZV2H_SDHI_SD_INFO2);
  if ((info & (SDHI_SD_INFO2_SCLKDIVEN | SDHI_SD_INFO2_CBSY)) ==
      SDHI_SD_INFO2_SCLKDIVEN)
    {
      rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, 0);
    }
  else
    {
      /* Clock writes are prohibited during a stalled sequence. SDRST=0
       * stops communication and clears SCLKEN (Rev1.40 pp1550,1620).
       * Hold reset for task-side recovery; never wait or release it here.
       */

      rzv2h_write16(priv, RZV2H_SDHI_SOFT_RST, SDHI_SOFT_RST_ASSERT);
      if ((rzv2h_read16(priv, RZV2H_SDHI_SOFT_RST) &
           SDHI_SOFT_RST_SDRST) == 0)
        {
          priv->clocked = false;
          leave_critical_section(flags);
          return priv->fault;
        }
    }

  /* Failed clock writes/reset assertion must not claim clock shutdown. */

  priv->clocked = (rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
                   SDHI_SD_CLK_CTRL_SCLKEN) != 0;
  leave_critical_section(flags);
  return priv->fault;
}

/****************************************************************************
 * Name: rzv2h_switchvoltage
 *
 * Description:
 *   Perform CMD11 and the serialized transition to 1.8 V signaling.
 *
 *   Called only after the upper half received ready + CCS + S18A. The
 *   SDHI exposes DAT0, not all four DAT inputs; this is the same busy
 *   proxy used by the Renesas Linux host. All-line electrical validation
 *   remains a gate.
 *
 *   Check DAT0 around clock shutdown and the board voltage callback,
 *   then restart the clock after the existing settling waits. Failed
 *   checks retain the UHS fault rather than claiming a usable session.
 *
 ****************************************************************************/

static int rzv2h_switchvoltage(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint16_t clock;
  uint32_t response;
  int ret;

  if (!priv->config.allow_uhs)
    {
      return -ENOTSUP;
    }

  if (up_interrupt_context() || !priv->attached)
    {
      return -EPERM;
    }

  if (priv->uhs_started || priv->uhs_failed || priv->fault < 0 ||
      priv->pending || priv->xfrstate == SDHI_XFR_ACTIVE ||
      priv->xfrstate == SDHI_XFR_PREPARED)
    {
      return -EBUSY;
    }

  /* Never let reset/reprobe mistake a partly switched card for 3.3 V. */

  priv->uhs_started = true;
  ret = rzv2h_sendcmd(dev, SD_CMD11, 0);
  if (ret == OK)
    {
      ret = rzv2h_waitresponse(dev, SD_CMD11);
    }

  if (ret == OK)
    {
      ret = rzv2h_recvshort(dev, SD_CMD11, &response);
      if (ret == OK && (response & SDHI_R1_ERRORMASK) != 0)
        {
          ret = -EIO;
        }
    }

  if (ret < 0)
    {
      return rzv2h_uhs_fail(priv, ret);
    }

  /* Scheduler-based waits avoid the board's loop calibration. The caller
   * holds the slot lock; interrupts remain enabled throughout settling.
   * Add a scheduler tick so tick phase cannot shorten the minimum wait.
   */

  nxsched_usleep(SDHI_UHS_DAT_LOW_WAIT_US + USEC_PER_TICK);
  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_INFO2) &
      SDHI_SD_INFO2_DAT0) != 0)
    {
      return rzv2h_uhs_fail(priv, -EIO);
    }

  clock = rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL);
  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL,
                clock & ~SDHI_SD_CLK_CTRL_SCLKEN);
  priv->clocked = false;
  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
      SDHI_SD_CLK_CTRL_SCLKEN) != 0)
    {
      return rzv2h_uhs_fail(priv, -EIO);
    }

  /* Rev1.40 section 6.2.3.1.2 also requires DAT0 low with SDCLK stopped.
   * Do not switch VQMMC if the card released DAT0 after the first check.
   */

  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_INFO2) &
      SDHI_SD_INFO2_DAT0) != 0)
    {
      return rzv2h_uhs_fail(priv, -EIO);
    }

  ret = rzv2h_sdio_setvoltage(priv->config.channel, true);
  if (ret < 0)
    {
      return rzv2h_uhs_fail(priv, ret);
    }

  nxsched_usleep(SDHI_UHS_VOLTAGE_SETTLE_US + USEC_PER_TICK);
  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, clock);
  priv->clocked = true;
  nxsched_usleep(SDHI_UHS_CLOCK_SETTLE_US + USEC_PER_TICK);
  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
      SDHI_SD_CLK_CTRL_SCLKEN) == 0 ||
      (rzv2h_read16(priv, RZV2H_SDHI_SD_INFO2) &
      SDHI_SD_INFO2_DAT0) == 0)
    {
      return rzv2h_uhs_fail(priv, -EIO);
    }

  priv->voltage18 = true;
  return OK;
}

#ifdef CONFIG_RZV2H_SDIO_UHS_SDR104
/****************************************************************************
 * Name: rzv2h_tuning_tap
 *
 * Description:
 *   Choose the midpoint of the longest acceptable cyclic passing-tap
 *   window.
 *
 *   Choose the midpoint of a cyclic window independently of scan start.
 *   Require at least three passing taps (a center with a neighbor
 *   on each side).
 *
 *   Treat the bitmap as a circular set of sampling phases and choose the
 *   center of its longest passing run. Return -1 when no run reaches the
 *   required minimum width.
 *
 ****************************************************************************/

static int rzv2h_tuning_tap(uint8_t good)
{
  unsigned int start;
  unsigned int length;
  unsigned int best = 0;
  int tap = -1;

  for (start = 0; start < SDHI_TUNING_TAPS; start++)
    {
      for (length = 0; length < SDHI_TUNING_TAPS; length++)
        {
          if ((good & (1u << ((start + length) % SDHI_TUNING_TAPS))) == 0)
            {
              break;
            }
        }

      if (length > best)
        {
          best = length;
          tap = (start + length / 2) % SDHI_TUNING_TAPS;
        }
    }

  return best >= SDHI_TUNING_MIN_WINDOW ? tap : -1;
}

/****************************************************************************
 * Name: rzv2h_tune
 *
 * Description:
 *   Probe SCC sampling phases with CMD19 and install the selected tap.
 *
 *   Compare each tuning read against the standard pattern and combine
 *   data and command-sampling evidence. Restore negotiated host settings
 *   after rejected probes and verify the final SCC state before
 *   publishing the tap. The sixteen probes bound the scan; host recovery
 *   time does not consume a separate scan acceptance deadline.
 *
 ****************************************************************************/

static int rzv2h_tune(struct rzv2h_sdio_s *priv)
{
  /* CMD19 tuning-block pattern in four-bit wire order (64 bytes). */

  static const uint8_t pattern[64] =
  {
    0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc,
    0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
    0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb,
    0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
    0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c,
    0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
    0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff,
    0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde
  };

  uint32_t words[16];
  uint16_t clock;
  uint16_t option;
  uint8_t good = SDHI_TUNING_ALL_TAPS;
  uint8_t sample = SDHI_TUNING_ALL_TAPS;
  unsigned int probe;
  unsigned int index;
  int tap;
  int ret;

  clock = rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL);
  option = rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION);
  priv->tuning = true;
  for (probe = 0; probe < SDHI_TUNING_PROBES; probe++)
    {
      index = probe % SDHI_TUNING_TAPS;
      rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, 0);

      /* Repeated probes may leave DTSEL enabled. Install the prescribed
       * DT2FF timing before enabling it again (manual 6.2.7.2.6).
       */

      putreg32(0, priv->base + RZV2H_SDHI_SCC_CKSEL);
      putreg32(SDHI_SCC_DTCNTL_8TAPS, priv->base + RZV2H_SDHI_SCC_DTCNTL);
      putreg32(SDHI_SCC_DT2FF_TUNING, priv->base + RZV2H_SDHI_SCC_DT2FF);
      putreg32(SDHI_SCC_DTSEL, priv->base + RZV2H_SDHI_SCC_CKSEL);
      putreg32(0, priv->base + RZV2H_SDHI_SCC_RVSCNTL);
      putreg32(0, priv->base + RZV2H_SDHI_SCC_RVSREQ);
      putreg32(index, priv->base + RZV2H_SDHI_SCC_TAPSET);
      rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, clock);
      if (((getreg32(priv->base + RZV2H_SDHI_SCC_DTCNTL) >>
          SDHI_SCC_DTCNTL_TAPNUM_SHIFT) &
          SDHI_SCC_DTCNTL_TAPNUM_MASK) != SDHI_TUNING_TAPS ||
          getreg32(priv->base + RZV2H_SDHI_SCC_DT2FF) !=
          SDHI_SCC_DT2FF_TUNING)
        {
          ret = -EIO;
          goto out;
        }

      ret = rzv2h_readblock64(priv, SD_CMD19, 0, (uint8_t *)words,
                               SDHI_TUNING_TIMEOUT_MS);
      if (ret == -ETIMEDOUT || priv->mediaremoved || priv->fault < 0)
        {
          if (ret >= 0)
            {
              ret = priv->mediaremoved ? -ENODEV : priv->fault;
            }

          goto out;
        }

      if (ret < 0 || memcmp(words, pattern, sizeof(pattern)) != 0)
        {
          good &= ~(1u << index);
        }

      if ((getreg32(priv->base + RZV2H_SDHI_SCC_SMPCMP) &
          SDHI_SCC_CMD_ERROR) != 0)
        {
          sample &= ~(1u << index);
        }

      /* A failed probe's cancel resets host width/clock. Restore the card's
       * negotiated host settings before the next finite tuning transfer.
       * No card power/voltage change occurs during this recovery.
       */

      if (ret < 0)
        {
          priv->sdcard = true;
          priv->selected = true;
          priv->cmd6_supported = true;
          rzv2h_write16(priv, RZV2H_SDHI_SD_OPTION, option);
          rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, clock);
          priv->clocked = true;
        }
    }

  /* If every data probe passed, use command sampling to avoid selecting a
   * marginal command phase merely because the payload happened to pass.
   */

  tap = rzv2h_tuning_tap(good == SDHI_TUNING_ALL_TAPS ? sample : good);
  if (tap < 0)
    {
      ret = -EIO;
      goto out;
    }

  /* The last probe may have reset the controller. Reinstall the complete
   * sampling state before publishing a selected tap, not just TAPSET.
   */

  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, 0);
  putreg32(0, priv->base + RZV2H_SDHI_SCC_CKSEL);
  putreg32(SDHI_SCC_DTCNTL_8TAPS, priv->base + RZV2H_SDHI_SCC_DTCNTL);
  putreg32(SDHI_SCC_DT2FF_TUNING, priv->base + RZV2H_SDHI_SCC_DT2FF);
  putreg32(SDHI_SCC_DTSEL, priv->base + RZV2H_SDHI_SCC_CKSEL);
  putreg32(tap, priv->base + RZV2H_SDHI_SCC_TAPSET);
  if (getreg32(priv->base + RZV2H_SDHI_SCC_TAPSET) != (uint32_t)tap ||
      (getreg32(priv->base + RZV2H_SDHI_SCC_CKSEL) &
      SDHI_SCC_DTSEL) == 0 ||
      (getreg32(priv->base + RZV2H_SDHI_SCC_DTCNTL) &
      SDHI_SCC_DTCNTL_CONFIG_MASK) != SDHI_SCC_DTCNTL_8TAPS ||
      getreg32(priv->base + RZV2H_SDHI_SCC_DT2FF) !=
      SDHI_SCC_DT2FF_TUNING)
    {
      ret = -EIO;
      goto out;
    }

  rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, clock);
  putreg32(0, priv->base + RZV2H_SDHI_SCC_RVSREQ);
  putreg32(SDHI_SCC_RVSEN, priv->base + RZV2H_SDHI_SCC_RVSCNTL);
  priv->tap = tap;
  ret = OK;
out:
  priv->tuning = false;
  if (ret < 0)
    {
      mcerr("ERROR: SDHI%u SDR104 tuning failed: %d good=%02x "
            "sample=%02x\n", priv->config.channel, ret, good, sample);
    }
  else
    {
      mcinfo("SDHI%u SDR104 tuning selected tap=%u good=%02x "
             "sample=%02x\n", priv->config.channel, priv->tap, good, sample);
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: rzv2h_uhs_current
 *
 * Description:
 *   Select a card current limit within board policy and verify CMD6
 *   results.
 *
 *   SD Part 1 v9.10 Tables 4-11/12/13: group 4 is the card power limit.
 *   Removable VDD1-only UHS-I uses function 0 (200 mA) or 1 (400 mA).
 *   Functions 2/3 require a qualified embedded-device board policy.
 *   CHECK current alone cannot establish board capability. Select from
 *   the card support bits and the board budget, then verify the SWITCH
 *   result.
 *
 *   Read the card support and busy fields, intersect them with the board
 *   supply budget and embedded-card permission, then verify the
 *   requested CMD6 current function and Type B drive strength.
 *
 ****************************************************************************/

static int rzv2h_uhs_current(struct rzv2h_sdio_s *priv, uint8_t status[64])
{
  const unsigned int budget = priv->config.max_current_ma;
  const bool embedded = priv->config.embedded_card;
  unsigned int function;
  unsigned int limit;
  unsigned int power;
  int ret;

  priv->current_limit = 0;
  ret = rzv2h_sd_switch(priv, SDHI_SWITCH_CHECK_ALL, status);
  if (ret < 0)
    {
      return ret;
    }

  if (status[SDHI_SWITCH_STATUS_VERSION] > SDHI_SWITCH_VERSION_BUSY ||
      (status[SDHI_SWITCH_CURRENT_RESULT] &
       SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
      SDHI_SWITCH_DRIVE_TYPE_B ||
      (status[SDHI_SWITCH_POWER_HI] == 0 &&
        status[SDHI_SWITCH_POWER_LO] == 0))
    {
      return -EIO;
    }

  if (embedded && budget >= SDHI_CURRENT_800MA &&
      (status[SDHI_SWITCH_CURRENT_SUPPORT] & SDHI_CURRENT_800MA_BIT) != 0)
    {
      function = SDHI_CURRENT_800MA_FUNCTION;
      limit = SDHI_CURRENT_800MA;
    }
  else if (embedded && budget >= SDHI_CURRENT_600MA &&
      (status[SDHI_SWITCH_CURRENT_SUPPORT] & SDHI_CURRENT_600MA_BIT) != 0)
    {
      function = SDHI_CURRENT_600MA_FUNCTION;
      limit = SDHI_CURRENT_600MA;
    }
  else if (budget >= SDHI_CURRENT_400MA &&
      (status[SDHI_SWITCH_CURRENT_SUPPORT] & SDHI_CURRENT_400MA_BIT) != 0)
    {
      function = SDHI_CURRENT_400MA_FUNCTION;
      limit = SDHI_CURRENT_400MA;
    }
  else if ((status[SDHI_SWITCH_CURRENT_SUPPORT] &
            SDHI_CURRENT_200MA_BIT) != 0)
    {
      function = SDHI_CURRENT_200MA_FUNCTION;
      limit = SDHI_CURRENT_200MA;
    }
  else
    {
      return -ENOTSUP;
    }

  if (status[SDHI_SWITCH_STATUS_VERSION] == SDHI_SWITCH_VERSION_BUSY &&
      (status[SDHI_SWITCH_CURRENT_BUSY] & (1u << function)) != 0)
    {
      return -EIO;
    }

  ret = rzv2h_sd_switch(priv, SDHI_SWITCH_SET_CURRENT | (function <<
      SDHI_SWITCH_CURRENT_SHIFT), status);
  if (ret < 0)
    {
      return ret;
    }

  power = ((unsigned int)status[SDHI_SWITCH_POWER_HI] <<
      SDHI_SWITCH_POWER_SHIFT) |
          status[SDHI_SWITCH_POWER_LO];
  if (status[SDHI_SWITCH_STATUS_VERSION] > SDHI_SWITCH_VERSION_BUSY ||
      (status[SDHI_SWITCH_CURRENT_SUPPORT] & (1u << function)) == 0 ||
      (status[SDHI_SWITCH_CURRENT_RESULT] >>
      SDHI_SWITCH_CURRENT_RESULT_SHIFT) != function ||
      (status[SDHI_SWITCH_CURRENT_RESULT] &
       SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
      SDHI_SWITCH_DRIVE_TYPE_B ||
      power == 0 || power > limit ||
      (status[SDHI_SWITCH_STATUS_VERSION] == SDHI_SWITCH_VERSION_BUSY &&
      (status[SDHI_SWITCH_CURRENT_BUSY] & (1u << function)) != 0))
    {
      return -EIO;
    }

  priv->current_limit = limit;
  return OK;
}

/****************************************************************************
 * Name: rzv2h_uhs_initialize
 *
 * Description:
 *   Negotiate UHS current and speed, set the clock and tune SDR104 if
 *   enabled.
 *
 *   Select the current allowance before checking and switching the
 *   access-speed function. Verify status fields and card-reported
 *   requirements, cap the clock by configuration and run tuning for
 *   SDR104.
 *
 ****************************************************************************/

static int rzv2h_uhs_initialize(struct rzv2h_sdio_s *priv)
{
  uint32_t words[16];
  uint8_t *status = (uint8_t *)words;
#ifdef CONFIG_RZV2H_SDIO_UHS_SDR104
  const unsigned int mode = SDHI_SWITCH_SPEED_SDR104;
  uint32_t target = SDHI_SDR104_HZ;
#else
  const unsigned int mode = SDHI_SWITCH_SPEED_SDR50;
  uint32_t target = SDHI_SDR50_HZ;
#endif
  unsigned int phase;
  unsigned int power;
  int ret;

  if (!priv->voltage18 || !priv->selected || !priv->cmd6_supported ||
      (rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION) &
       SDHI_SD_OPTION_WIDTH_MASK) != SDHI_SD_OPTION_WIDTH_4BIT)
    {
      return -EINVAL;
    }

  /* Select group 4 first, retaining Type B drive strength. Then query and
   * select group 1, preserving the verified current allowance.
   */

  ret = rzv2h_uhs_current(priv, status);
  if (ret < 0)
    {
      return ret;
    }

  for (phase = 0; phase < 2; phase++)
    {
      ret = rzv2h_sd_switch(priv,
                            (phase ? SDHI_SWITCH_SET_ACCESS :
                             SDHI_SWITCH_CHECK_ACCESS) | mode, status);
      if (ret < 0)
        {
          return ret;
        }

      /* CHECK reports the mode maximum regardless of the selected limit.
       * SWITCH must honor the selected budget (Part 1, section 4.3.10.4).
       * Never raise that budget based on CHECK alone.
       */

      power = ((unsigned int)status[SDHI_SWITCH_POWER_HI] <<
          SDHI_SWITCH_POWER_SHIFT) |
              status[SDHI_SWITCH_POWER_LO];
      if (status[SDHI_SWITCH_STATUS_VERSION] > SDHI_SWITCH_VERSION_BUSY ||
          power == 0 ||
          (phase != 0 && power > priv->current_limit) ||
          (status[SDHI_SWITCH_ACCESS_SUPPORT] & (1u << mode)) == 0 ||
          (status[SDHI_SWITCH_ACCESS_RESULT] &
           SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
          mode ||
          (status[SDHI_SWITCH_CURRENT_RESULT] &
           SDHI_SWITCH_GROUP_FUNCTION_MASK) !=
          SDHI_SWITCH_DRIVE_TYPE_B ||
          (status[SDHI_SWITCH_CURRENT_RESULT] >>
          SDHI_SWITCH_CURRENT_RESULT_SHIFT) !=
          (priv->current_limit / SDHI_CURRENT_200MA - 1) ||
          (status[SDHI_SWITCH_STATUS_VERSION] == SDHI_SWITCH_VERSION_BUSY &&
          (status[SDHI_SWITCH_ACCESS_BUSY] & (1u << mode)) != 0))
        {
          return -EIO;
        }
    }

  priv->uhs_mode = mode;
  priv->hs_checked = true;
  if (target > CONFIG_RZV2H_SDIO_MAX_FREQUENCY)
    {
      target = CONFIG_RZV2H_SDIO_MAX_FREQUENCY;
    }

  ret = rzv2h_setclock(priv, target);
#ifdef CONFIG_RZV2H_SDIO_UHS_SDR104
  if (ret == OK)
    {
      ret = rzv2h_tune(priv);
    }
#endif

  return ret;
}

/****************************************************************************
 * Name: rzv2h_uhs_command
 *
 * Description:
 *   Issue a private non-data command in the serialized discovery task.
 *   Check response/card errors; a stale response cannot imply success.
 *
 *   Use the normal send/wait/receive helpers in the serialized discovery
 *   task. Return transport or card-status errors; copy the response only
 *   when requested and valid.
 *
 ****************************************************************************/

static int rzv2h_uhs_command(struct rzv2h_sdio_s *priv, uint32_t cmd,
                              uint32_t arg, uint32_t *response)
{
  uint32_t value;
  int ret;

  ret = rzv2h_sendcmd(&priv->dev, cmd, arg);
  if (ret == OK)
    {
      ret = rzv2h_waitresponse(&priv->dev, cmd);
    }

  if (ret == OK && (cmd & MMCSD_RESPONSE_MASK) != MMCSD_NO_RESPONSE)
    {
      ret = rzv2h_recvshort(&priv->dev, cmd, &value);
      if (ret == OK && (value & SDHI_R1_ERRORMASK) != 0)
        {
          ret = -EIO;
        }

      if (ret == OK && response != NULL)
        {
          *response = value;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_uhs_prepare
 *
 * Description:
 *   Select the card, prepare UHS and return it to standby for the upper
 *   half.
 *
 *   Prepare UHS before exposing CSD geometry to the unchanged upper
 *   half. The outer CMD9 has completed in standby. Temporarily select
 *   the card, negotiate width and timing, check status, then return it
 *   to standby for the caller's CMD7. Width and timing remain matched
 *   across selection.
 *
 *   Run after an eligible CSD response while the card is in standby.
 *   Select it, request four-bit operation, negotiate timing, check
 *   transfer state and deselect it before marking UHS ready.
 *
 ****************************************************************************/

static int rzv2h_uhs_prepare(struct rzv2h_sdio_s *priv, uint32_t rca)
{
  uint32_t response;
  int ret;

  if (up_interrupt_context() || !priv->attached)
    {
      return -EPERM;
    }

  if (!priv->voltage18 || !priv->sdcard || !priv->cmd6_supported ||
      rca == 0 || (rca & 0xffff) != 0)
    {
      return -ENOTSUP;
    }

  ret = rzv2h_uhs_command(priv, MMCSD_CMD7S, rca, NULL);
  if (ret == OK)
    {
      ret = rzv2h_uhs_command(priv, SD_CMD55, rca, &response);
      if (ret == OK && (response & SDHI_R1_APP_CMD) == 0)
        {
          ret = -EIO;
        }
    }

  if (ret == OK)
    {
      ret = rzv2h_uhs_command(priv, SD_ACMD6, 2, NULL);
    }

  if (ret == OK)
    {
      rzv2h_widebus(&priv->dev, true);
      ret = priv->fault;
    }

  if (ret == OK)
    {
      ret = rzv2h_uhs_initialize(priv);
    }

  if (ret == OK)
    {
      ret = rzv2h_uhs_command(priv, MMCSD_CMD13, rca, &response);
      if (ret == OK && ((response >> 9) & 15) != 4)
        {
          ret = -EIO;
        }
    }

  if (ret == OK)
    {
      ret = rzv2h_uhs_command(priv, MMCSD_CMD7D, 0, NULL);
    }

  if (ret == OK)
    {
      priv->uhs_ready = true;
    }

  return ret;
}

#endif

/****************************************************************************
 * Name: rzv2h_reset_host
 *
 * Description:
 *   Reset host state, preserving negotiated settings when recovery
 *   requests it.
 *
 *   Stop IRQ delivery and retire command, transfer and wait state before
 *   reinitializing host registers. The preserve flag controls recovery
 *   settings; retained card-session faults are reported through
 *   priv->fault.
 *   Reset the interface before clock/SCC changes and verify readiness
 *   before DMAC/host reinitialization. Recovery also retains faults raised
 *   while stopping ACTIVE DMA.
 *   Called in serialized task context with permission to sleep. Reset
 *   settling waits occur outside the critical section.
 *
 ****************************************************************************/

static void rzv2h_reset_host(struct rzv2h_sdio_s *priv, bool preserve)
{
  irqstate_t flags;
  uint16_t clock;
  uint16_t sdif;
  uint16_t savedoption;
  uint16_t option;
  int fault;

  if (priv->attached)
    {
      up_disable_irq(RZV2H_FSP_TO_GIC_IRQ(priv->config.intsel));
    }

  flags = enter_critical_section();
  clock = rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL);
  sdif = rzv2h_read16(priv, RZV2H_SDHI_SDIF_MODE);
  savedoption = rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION);
  fault = priv->fault;
  rzv2h_mask(priv);
#ifdef CONFIG_RZV2H_SDIO_DMA
  rzv2h_stopdma(priv);

  /* Stopping ACTIVE DMA can raise a new fault.  Preserve it through
   * recovery, while retaining an earlier error if one already existed.
   */

  if (fault == OK)
    {
      fault = priv->fault;
    }

#endif
  priv->pending = false;
  priv->datacmd = false;
  priv->xfrstate = SDHI_XFR_ERROR;
  if (priv->waitevents != 0)
    {
      rzv2h_endwait(priv, SDIOWAIT_ERROR);
    }
  else
    {
      wd_cancel(&priv->waitwdog);
    }

  priv->rxbuffer = NULL;
  priv->txbuffer = NULL;
  leave_critical_section(flags);

  /* Include one tick so tick phase cannot shorten the settling waits. */

  rzv2h_write16(priv, RZV2H_SDHI_SOFT_RST, SDHI_SOFT_RST_ASSERT);
  nxsched_usleep(SDHI_RESET_SETTLE_US + USEC_PER_TICK);

  rzv2h_write16(priv, RZV2H_SDHI_SOFT_RST, SDHI_SOFT_RST_RELEASE);
  nxsched_usleep(SDHI_RESET_SETTLE_US + USEC_PER_TICK);

  rzv2h_mask(priv);

  priv->pending = false;
#ifdef CONFIG_RZV2H_SDIO_UHS
  if (!preserve)
    {
      priv->cmd8_valid = false;
      priv->uhs_requested = false;
    }
#endif

  if (!preserve)
    {
      priv->highspeed = false;
      priv->sdcard = false;
      priv->selected = false;
      priv->cmd6_supported = false;
      priv->hs_checked = false;
    }

  priv->clocked = (rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
                   SDHI_SD_CLK_CTRL_SCLKEN) != 0;
  priv->result = -ECANCELED;
  priv->xfrstate = SDHI_XFR_IDLE;
  priv->xfrresult = -ECANCELED;
  priv->rxbuffer = NULL;
  priv->txbuffer = NULL;
  priv->remaining = 0;
  priv->total = 0;
  priv->blocklen = 0;
  priv->nblocks = 0;
  priv->geometry = false;
  priv->datacmd = false;
  priv->multiactive = false;
  priv->autostop = false;
  priv->appcmd = false;
  priv->setcount = false;
  priv->blockcount = 0;
  priv->stopvalid = false;
  priv->command = 0;
  priv->fault = rzv2h_idle(priv, true);
  if (priv->fault == OK)
    {
      rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, 0);
      if ((rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
           SDHI_SD_CLK_CTRL_SCLKEN) != 0)
        {
          priv->clocked = true;
          priv->fault = fault < 0 ? fault : -EIO;
          goto out;
        }

#ifdef CONFIG_RZV2H_SDIO_UHS
      if (priv->config.allow_uhs)
        {
          putreg32(0, priv->base + RZV2H_SDHI_SCC_CKSEL);
          putreg32(0, priv->base + RZV2H_SDHI_SCC_DTCNTL);
          putreg32(0, priv->base + RZV2H_SDHI_SCC_RVSCNTL);
        }
#endif

#ifdef CONFIG_RZV2H_SDIO_DMA
      if (priv->config.allow_dma)
        {
          rzv2h_sdiodma_initialize(&priv->dma, priv->base);
        }
#endif

      priv->clocked = false;
      rzv2h_write16(priv, RZV2H_SDHI_SDIF_MODE, preserve ? sdif : 0);
      rzv2h_write16(priv, RZV2H_SDHI_CC_EXT_MODE, SDHI_CC_EXT_MODE_RESET);
      rzv2h_write16(priv, RZV2H_SDHI_HOST_MODE,
                    SDHI_HOST_MODE_32BIT);
      option = preserve ? savedoption :
                          rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION);
      if (!preserve)
        {
          option &= ~SDHI_SD_OPTION_WIDTH8;
          option |= SDHI_SD_OPTION_WIDTH;
        }

      rzv2h_write16(priv, RZV2H_SDHI_SD_OPTION, option);
#ifdef CONFIG_RZV2H_SDIO_UHS
      if (priv->uhs_started && !priv->tuning)
        {
          rzv2h_uhs_fail(priv, -EIO);
        }
      else
#endif
      if (preserve)
        {
          /* A controller reset does not renegotiate the external card.
           * Restore its host settings, never silently fall back to IDMODE.
           * Readback failure and previously retained faults fail closed.
           */

          rzv2h_write16(priv, RZV2H_SDHI_SD_CLK_CTRL, clock);
          if (rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION) != option ||
              rzv2h_read16(priv, RZV2H_SDHI_SDIF_MODE) != sdif ||
              rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) != clock)
            {
              priv->fault = -EIO;
            }
          else
            {
              priv->clocked = (clock & SDHI_SD_CLK_CTRL_SCLKEN) != 0;
              priv->fault = fault;
            }
        }
      else
        {
          rzv2h_clock(&priv->dev, CLOCK_IDMODE);
        }
    }
  else if (fault < 0)
    {
      priv->fault = fault;
    }

out:
  if (priv->attached
#ifdef CONFIG_RZV2H_SDIO_UHS
      && !priv->restarting
#endif
     )
    {
      rzv2h_updatemask(priv);
      up_enable_irq(RZV2H_FSP_TO_GIC_IRQ(priv->config.intsel));
    }
}

#ifdef CONFIG_RZV2H_SDIO_UHS
/****************************************************************************
 * Name: rzv2h_prepare_reinsert
 *
 * Description:
 *   Prepare the host and board after delivered native removal.
 *   The application must stop card users and keep the card fully removed
 *   until the ready notice. Dedicated SD pads are not GPIO-isolated.
 *   Board preparation may change supplies or keep a fixed-rail profile.
 *   Preserve UHS restrictions until a later insertion starts discovery.
 *   This is task context only; no clock/power waits occur in the IRQ.
 *
 ****************************************************************************/

static int rzv2h_prepare_reinsert(struct rzv2h_sdio_s *priv)
{
  int ret;

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  if (priv->config.cd_source != RZV2H_CD_NATIVE)
    {
      return -ENOTSUP;
    }

  if ((rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) != 0)
    {
      return -EBUSY;
    }

  priv->restarting = true;
  rzv2h_reset_host(priv, true);
  rzv2h_mask(priv);
  ret = rzv2h_idle(priv, true);
  if (ret == OK &&
      (rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
       SDHI_SD_CLK_CTRL_SCLKEN) != 0)
    {
      ret = -EIO;
    }

  if (ret == OK &&
      (rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) != 0)
    {
      ret = -EBUSY;
    }

  if (ret == OK)
    {
      ret = rzv2h_sdio_prepare_reinsert(priv->config.channel);
    }

  if (ret == OK &&
      (rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) != 0)
    {
      ret = -EBUSY;
    }

  priv->reinsert_ready = ret == OK;
  if (ret < 0)
    {
      priv->fault = ret;
    }

  priv->restarting = false;
  rzv2h_updatemask(priv);
  if (priv->attached)
    {
      up_enable_irq(RZV2H_FSP_TO_GIC_IRQ(priv->config.intsel));
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_finish_reinsert
 *
 * Description:
 *   Start a fresh card session only after empty-slot board preparation
 *   and a subsequent native insertion. Called by CLOCK_IDMODE under the
 *   MMCSD caller's serialization. No upper-half or public SDIO API change.
 *
 ****************************************************************************/

static int rzv2h_finish_reinsert(struct rzv2h_sdio_s *priv)
{
  if (up_interrupt_context())
    {
      return -EPERM;
    }

  if (!priv->reinsert_ready || priv->restarting ||
      priv->config.cd_source != RZV2H_CD_NATIVE)
    {
      return -EAGAIN;
    }

  if ((rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) == 0)
    {
      return -ENODEV;
    }

  priv->reinsert_needed = false;
  priv->reinsert_ready = false;
  priv->cmd8_valid = false;
  priv->uhs_requested = false;
  priv->uhs_ready = false;

  /* Negotiated card-session state, not the board's physical I/O rail. */

  priv->voltage18 = false;
  priv->uhs_started = false;
  priv->uhs_failed = false;
  priv->uhs_mode = 0;
  priv->tap = 0;
  priv->current_limit = 0;
  priv->hsfailed = false;
  priv->fault = OK;
  rzv2h_reset_host(priv, false);
  if (priv->fault < 0)
    {
      priv->reinsert_needed = true;
      priv->uhs_failed = true;
    }

  return priv->fault;
}
#endif

/****************************************************************************
 * Name: rzv2h_reset
 *
 * Description:
 *   Reset the host through the public SDIO callback.
 *
 *   Delegate to the common reset implementation with preserve disabled.
 *   The void callback leaves any reset or retained session failure in
 *   the host fault field.
 *
 ****************************************************************************/

static void rzv2h_reset(struct sdio_dev_s *dev)
{
  rzv2h_reset_host((struct rzv2h_sdio_s *)dev, false);
}

/****************************************************************************
 * Name: rzv2h_capabilities
 *
 * Description:
 *   Report the bus-width and DMA capabilities permitted by this slot.
 *
 *   Advertise one-bit or four-bit support from the copied board record.
 *   Advertise DMA only when built and permitted, while retaining the
 *   setup-before-write capability flag.
 *
 ****************************************************************************/

static sdio_capset_t rzv2h_capabilities(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  sdio_capset_t caps = SDIO_CAPS_DMABEFOREWRITE;

  caps |= priv->config.bus_width == 4 ? SDIO_CAPS_4BIT :
                                      SDIO_CAPS_1BIT_ONLY;
#ifdef CONFIG_RZV2H_SDIO_DMA
  if (priv->config.allow_dma)
    {
      caps |= SDIO_CAPS_DMASUPPORTED;
    }
#endif

  return caps;
}

/****************************************************************************
 * Name: rzv2h_status
 *
 * Description:
 *   Report card presence and write protection from configured board
 *   sources.
 *
 *   Select fixed or native card detection according to board
 *   policy. Report write protection only for present media, honoring the
 *   configured source and polarity.
 *
 ****************************************************************************/

static sdio_statset_t rzv2h_status(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint32_t info = rzv2h_readpair(priv, RZV2H_SDHI_SD_INFO1);
  sdio_statset_t status = 0;
  irqstate_t flags = enter_critical_section();

  if (priv->config.cd_source == RZV2H_CD_FIXED ||
      (priv->config.cd_source == RZV2H_CD_NATIVE &&
       (info & SDHI_STATUS_PRESENT) != 0))
    {
      status = SDIO_STATUS_PRESENT;

      /* INFO7 is inverted ISDWP.  Ignore an unwired board input. */

      if (priv->config.wp_source == RZV2H_WP_NATIVE &&
          (((info & SDHI_SD_INFO1_INFO7) == 0) !=
           priv->config.wp_active_low))
        {
          status |= SDIO_STATUS_WRPROTECTED;
        }
    }

  leave_critical_section(flags);
  return status;
}

/****************************************************************************
 * Name: rzv2h_widebus
 *
 * Description:
 *   Select one or four host data lines when idle and allowed by board
 *   policy.
 *
 *   Reject unsupported width changes and active requests, then wait for
 *   the controller to become idle. Preserve required SD_OPTION bits and
 *   retain a fault if width readback fails.
 *
 ****************************************************************************/

static void rzv2h_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint16_t option;
  int ret;

  if (priv->fault < 0)
    {
      goto out;
    }

  if (enable && priv->config.bus_width != 4)
    {
      priv->fault = -ENOTSUP;
      goto out;
    }

  if (priv->pending || priv->xfrstate == SDHI_XFR_PREPARED ||
      priv->xfrstate == SDHI_XFR_ACTIVE)
    {
      priv->fault = -EBUSY;
      goto out;
    }

  /* SD_OPTION must not change while CBSY is set (manual p.1553). */

  ret = rzv2h_idle(priv, false);
  if (ret < 0)
    {
      priv->fault = ret;
      goto out;
    }

  option = rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION);
  option &= ~SDHI_SD_OPTION_WIDTH_MASK;
  option |= SDHI_SD_OPTION_RESERVED_ONE;
  option |= enable ? SDHI_SD_OPTION_WIDTH_4BIT : SDHI_SD_OPTION_WIDTH_1BIT;
  rzv2h_write16(priv, RZV2H_SDHI_SD_OPTION, option);
  if ((rzv2h_read16(priv, RZV2H_SDHI_SD_OPTION) &
      SDHI_SD_OPTION_WIDTH_MASK) !=
      (option & SDHI_SD_OPTION_WIDTH_MASK))
    {
      priv->fault = -EIO;
    }

out:
  return;
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/****************************************************************************
 * Name: rzv2h_mediaremoved
 *
 * Description:
 *   Invalidate the removed card session and fail pending transfers and
 *   waits.
 *
 *   Called with interrupts disabled. Stop software I/O before
 *   notification.
 *
 *   Mark media unavailable, retire its pending command and predefined
 *   block count, and invalidate negotiation state. End a prepared or
 *   active transfer with -ENODEV, or wake an existing wait with an
 *   error.
 *
 ****************************************************************************/

static void rzv2h_mediaremoved(struct rzv2h_sdio_s *priv)
{
#ifdef CONFIG_RZV2H_SDIO_UHS
  if (priv->uhs_started || priv->uhs_failed || priv->voltage18 ||
      priv->reinsert_needed)
    {
      priv->reinsert_needed = true;
      priv->reinsert_ready = false;
      priv->uhs_ready = false;
    }
#endif

  if (priv->hsfailed)
    {
      priv->hsfailed = false;
      priv->fault = OK;
    }

  priv->highspeed = false;
  priv->hs_checked = false;
  priv->sdcard = false;
  priv->selected = false;
  priv->cmd6_supported = false;
  priv->mediaremoved = true;
  priv->pending = false;
  priv->result = -ENODEV;
  priv->blockcount = 0;
  priv->stopvalid = false;
  priv->multiactive = false;
  if (priv->xfrstate == SDHI_XFR_ACTIVE ||
      priv->xfrstate == SDHI_XFR_PREPARED)
    {
      rzv2h_endtransfer(priv, -ENODEV);
    }
  else
    {
      rzv2h_updatemask(priv);
      rzv2h_endwait(priv, SDIOWAIT_ERROR);
    }
}
#endif

/****************************************************************************
 * Name: rzv2h_complete
 *
 * Description:
 *   Consume latched hardware events and advance command and data
 *   completion.
 *
 *   Called in a short critical section, or from the IRQ handler. Both
 *   the polling and interrupt paths consume the same latched command
 *   result.
 *
 *   Share one event-consumption path between polling and IRQ handling.
 *   Decode command responses, service PIO or DMA progress, process card
 *   removal and publish terminal results only when the required
 *   completion conditions hold.
 *
 ****************************************************************************/

static void rzv2h_complete(struct rzv2h_sdio_s *priv)
{
  uint32_t info = rzv2h_readpair(priv, RZV2H_SDHI_SD_INFO1);
  uint32_t type = priv->command & MMCSD_RESPONSE_MASK;
  uint32_t errors = info & SDHI_STATUS_CMDERR;
  uint32_t ready;
  unsigned int i;

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  if (priv->config.cd_source == RZV2H_CD_NATIVE &&
      (info & SDHI_STATUS_CD) != 0 && priv->callback != NULL &&
      !priv->cbclosing)
    {
      rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~SDHI_STATUS_CD);

      /* Quiesce buffers immediately; debounce only the notification. */

      if ((info & SDHI_SD_INFO1_INFO3) != 0)
        {
          rzv2h_mediaremoved(priv);
        }

      work_queue(HPWORK, &priv->cdwork, rzv2h_mediaworker, priv,
                 MSEC2TICK(priv->config.cd_debounce_ms));
      if (priv->mediaremoved)
        {
          return;
        }
    }
#endif

  if (priv->pending && (info & SDHI_STATUS_CMDEVENTS) != 0)
    {
      if (type == MMCSD_R3_RESPONSE || type == MMCSD_R4_RESPONSE)
        {
          errors &= ~SDHI_STATUS_CRCERR;
        }

      /* A CRC indication alone is not completion of a CRC-less response. */

      if (errors == 0 && (info & SDHI_STATUS_RESPEND) == 0)
        {
          rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~SDHI_STATUS_CRCERR);
        }
      else
        {
          if ((errors & SDHI_STATUS_RESPTIMEOUT) != 0)
            {
              priv->result = -ETIMEDOUT;
            }
          else if ((errors & SDHI_STATUS_ACCESSERR) != 0)
            {
              priv->result = -EIO;
            }
          else if (errors != 0)
            {
              priv->result = -EILSEQ;
            }
          else
            {
              priv->result = OK;
              if (type == MMCSD_R2_RESPONSE)
                {
                  for (i = 0; i < 4; i++)
                    {
                      priv->response[3 - i] =
                        rzv2h_readpair(priv, RZV2H_SDHI_SD_RSP10 + i * 16);
                    }

                  for (i = 0; i < 3; i++)
                    {
                      priv->response[i] = (priv->response[i] << 8) |
                                          (priv->response[i + 1] >> 24);
                    }

                  priv->response[3] <<= 8;
                }
              else if (type != MMCSD_NO_RESPONSE)
                {
                  priv->response[0] =
                    rzv2h_readpair(priv, priv->multiactive ?
                                  RZV2H_SDHI_SD_RSP54 : RZV2H_SDHI_SD_RSP10);
                }
            }

          rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1,
                           ~SDHI_STATUS_CMDEVENTS);

          priv->pending = false;

          if (priv->result == OK)
            {
              if (priv->sdprobe && priv->command == SD_ACMD41)
                {
                  priv->sdcard = true;
                }
              else if ((priv->command & MMCSD_CMDIDX_MASK) == 7 &&
                       (priv->response[0] & SDHI_R1_ERRORMASK) == 0)
                {
                  priv->selected = priv->argument != 0;
                }
            }

          /* Only a successful, ordinary CMD23 arms a predefined transfer.
           * CMD55's APP_CMD status distinguishes ACMD23 (pre-erase).
           * The count survives local setup calls, not another wire command.
           */

          if (priv->setcount && priv->result == OK &&
              !rzv2h_dataresponse_error(priv))
            {
              priv->blockcount = priv->argument;
            }

          if (priv->command == SD_CMD55 && priv->result == OK &&
              !rzv2h_dataresponse_error(priv))
            {
              priv->appcmd = (priv->response[0] & SDHI_R1_APP_CMD) != 0;
            }

          if (priv->setcount && priv->xfrstate == SDHI_XFR_PREPARED &&
              (priv->result < 0 || rzv2h_dataresponse_error(priv)))
            {
              rzv2h_endtransfer(priv, -EIO);
            }

          if (priv->datacmd && priv->xfrstate == SDHI_XFR_PREPARED)
            {
              if (priv->result < 0 || rzv2h_dataresponse_error(priv))
                {
                  rzv2h_endtransfer(priv, -EIO);
                }
              else
                {
                  priv->xfrstate = SDHI_XFR_ACTIVE;
#ifdef CONFIG_RZV2H_SDIO_DMA
                  rzv2h_sdiodma_start(&priv->dma);
#endif
                  rzv2h_updatemask(priv);
                }
            }
          else
            {
              rzv2h_updatemask(priv);
            }
        }
    }

  if (priv->xfrstate != SDHI_XFR_ACTIVE)
    {
      return;
    }

  /* A transfer error wins over FIFO-ready or access-end status from the
   * same snapshot.  This prevents copying data after a failed command.
   */

  if ((info & SDHI_STATUS_DATAERR) != 0)
    {
      rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~SDHI_STATUS_DATAEVENTS);
      rzv2h_endtransfer(priv, (info & (SDHI_STATUS_DATATIMEOUT |
                                      SDHI_STATUS_RESPTIMEOUT)) != 0 ?
                                     -ETIMEDOUT : -EIO);
      return;
    }

#ifdef CONFIG_RZV2H_SDIO_DMA
  if (priv->dma.prepared)
    {
      bool access = (info & SDHI_STATUS_ACCESSEND) != 0;
      int result;

      if (access)
        {
          rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1,
                           ~SDHI_STATUS_ACCESSEND);
        }

      result = rzv2h_sdiodma_poll(&priv->dma, access);
      if (result == -EINPROGRESS)
        {
          return;
        }

      if (result < 0)
        {
          rzv2h_endtransfer(priv, -EIO);
          return;
        }

      priv->remaining = 0;
      info |= SDHI_STATUS_ACCESSEND;
    }
  else
#endif
    {
      ready = priv->read ? SDHI_STATUS_BRE : SDHI_STATUS_BWE;
      if ((info & ready) != 0)
        {
          rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~ready);
          if (priv->remaining == 0)
            {
              rzv2h_endtransfer(priv, -EIO);
              return;
            }

          if (priv->read)
            {
              rzv2h_receive(priv);
            }
          else
            {
              rzv2h_transmit(priv);
            }
        }
    }

  if ((info & SDHI_STATUS_ACCESSEND) != 0)
    {
      rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~SDHI_STATUS_ACCESSEND);
      if (priv->multiactive && priv->remaining == 0)
        {
          /* A predefined transfer finishes at the CMD23 count without
           * CMD12.  Only an automatic-stop transfer has a stop response
           * in RSP10; RSP54 preserves CMD18/CMD25 in either mode.
           */

          priv->multiactive = false;
          if (priv->autostop)
            {
              priv->stopresponse = rzv2h_readpair(priv,
                                                RZV2H_SDHI_SD_RSP10);
              priv->stopvalid = true;
              if ((priv->stopresponse & SDHI_R1_ERRORMASK) != 0)
                {
                  priv->fault = -EIO;
                  rzv2h_endtransfer(priv, -EIO);
                  return;
                }
            }
        }

      rzv2h_endtransfer(priv, priv->remaining == 0 ? OK : -EIO);
    }
}

/****************************************************************************
 * Name: rzv2h_interrupt
 *
 * Description:
 *   Service the routed SDHI IRQ through the shared completion handler.
 *
 *   Run the shared completion handler inside a short critical section
 *   and return OK to the interrupt framework.
 *
 ****************************************************************************/

static int rzv2h_interrupt(int irq, void *context, void *arg)
{
  struct rzv2h_sdio_s *priv = arg;
  irqstate_t flags = enter_critical_section();

  rzv2h_complete(priv);

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rzv2h_attach
 *
 * Description:
 *   Claim the event route, attach the handler and enable the host IRQ.
 *
 *   Reserve the selected interrupt event before changing GIC state.
 *   Install the handler, configure trigger and priority, enable delivery
 *   and unwind the claimed resources on setup failure.
 *
 ****************************************************************************/

static int rzv2h_attach(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  rzv2h_irqn_t sel = (rzv2h_irqn_t)priv->config.intsel;
  rzv2h_irqsel_t event = (rzv2h_irqsel_t)(RZV2H_IRQSEL_SDHI0_OXMN +
                                        2 * priv->config.channel);
  int irq = RZV2H_FSP_TO_GIC_IRQ(sel);
  int ret;

  if (priv->attached)
    {
      return OK;
    }

  /* Claim before changing the GIC: a conflicting owner must remain intact. */

  ret = rzv2h_intsel_connect_event(sel, event, BSP_GIC_SPI_DETECT_LEVEL);
  if (ret < 0)
    {
      return ret;
    }

  up_disable_irq(irq);
  rzv2h_mask(priv);
  ret = irq_attach(irq, rzv2h_interrupt, priv);
  if (ret < 0)
    {
      goto err_route;
    }

  ret = up_set_irq_type(irq, IRQ_HIGH_LEVEL);
  if (ret < 0)
    {
      goto err_attach;
    }

  ret = up_prioritize_irq(irq, priv->config.priority << 4);
  if (ret < 0)
    {
      goto err_attach;
    }

  priv->attached = true;
  rzv2h_updatemask(priv);
  up_enable_irq(irq);
  return OK;

err_attach:
  irq_detach(irq);
err_route:
  rzv2h_intsel_disconnect_event(sel);
  return ret;
}

/****************************************************************************
 * Name: rzv2h_sendcmd
 *
 * Description:
 *   Validate command state, program the controller and launch the
 *   command.
 *
 *   Validate command, geometry, fault and transfer-state constraints
 *   before writing SD_ARG and SD_CMD. Handle cached automatic-stop
 *   responses and select predefined-count or automatic-stop behavior for
 *   multiblock memory transfers.
 *
 ****************************************************************************/

static int rzv2h_sendcmd(struct sdio_dev_s *dev, uint32_t cmd, uint32_t arg)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;

  /* Extract the command index using the mask. */

  uint16_t value = cmd & MMCSD_CMDIDX_MASK;
  bool data = (cmd & MMCSD_DATAXFR) != 0;
  bool write = (cmd & MMCSD_WRXFR) != 0;
  bool multi = (cmd & MMCSD_MULTIBLOCK) != 0;
  bool setcount = cmd == MMCSD_CMD23 && !priv->appcmd;
  irqstate_t flags;
  int ret;

  if (priv->mediaremoved)
    {
      return -ENODEV;
    }

#ifdef CONFIG_RZV2H_SDIO_UHS
  if (priv->uhs_failed || (priv->uhs_started && cmd == MMCSD_CMD0) ||
      (priv->uhs_mode == SDHI_SWITCH_SPEED_SDR104 && !priv->tuning &&
       (getreg32(priv->base + RZV2H_SDHI_SCC_RVSREQ) &
        SDHI_SCC_RVSERR) != 0))
    {
      return rzv2h_uhs_fail(priv, -EIO);
    }
#endif

  if (priv->fault < 0)
    {
      return priv->fault;
    }

  if (!priv->clocked)
    {
      return -EIO;
    }

  if (priv->pending)
    {
      return -EBUSY;
    }

  if ((cmd & (MMCSD_STREAM | SDIO_MULTIBLOCK)) != 0)
    {
      return -ENOTSUP;
    }

  if (priv->xfrstate == SDHI_XFR_ACTIVE)
    {
      return -EBUSY;
    }

  /* NuttX requests CMD12 after eventwait.  If SDHI already transmitted it,
   * return that command's actual, validated response once.  Never invent
   * a successful stop on a failed/incomplete transfer.
   */

  if (cmd == MMCSD_CMD12 && arg == 0 && priv->stopvalid)
    {
      priv->stopvalid = false;
      priv->command = cmd;
      priv->response[0] = priv->stopresponse;
      priv->result = OK;
      priv->datacmd = false;
      return OK;
    }

  /* Plain SD CMD23 uses a 32-bit block count.  Limit it to this driver's
   * transfer bound; zero is legal and means no predefined count.  MMC
   * reliable-write flags and larger counts are outside this port's scope.
   */

  if (setcount && arg > SDHI_MAX_BLOCKS)
    {
      return -ENOTSUP;
    }

  if (multi && cmd != MMCSD_CMD18 && cmd != MMCSD_CMD25)
    {
      return -ENOTSUP;
    }

  if (data)
    {
      if (priv->xfrstate != SDHI_XFR_PREPARED || !priv->geometry ||
          priv->nblocks == 0 || priv->nblocks > SDHI_MAX_BLOCKS ||
          priv->blocklen == 0 ||
          priv->blocklen > SDHI_MAX_BLOCK_SIZE ||
          priv->total != (size_t)priv->blocklen * priv->nblocks ||
          priv->read == write || (!multi && priv->nblocks != 1) ||
          (multi && priv->blocklen != SDHI_MAX_BLOCK_SIZE))
        {
          return -EINVAL;
        }

      if (multi && (priv->appcmd ||
          (priv->blockcount != 0 && priv->blockcount != priv->nblocks)))
        {
          return -EINVAL;
        }

      if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE)
        {
          return -EINVAL;
        }

      value |= SDHI_SD_CMD_MD3;
      if (multi)
        {
          value |= SDHI_SD_CMD_MD5;
          if (priv->blockcount != 0)
            {
              /* The card received CMD23; let both card and host finish
               * at that count.  Renesas TMIO selects NO_CMD12_ISSUE for
               * requests with an sbc (SET_BLOCK_COUNT) command.
               */

              value |= SDHI_SD_CMD_STOP_NO_AUTO;
            }
        }

      if (!write)
        {
          value |= SDHI_SD_CMD_MD4;
        }

      /* CMD53 block mode is a finite SDIO transfer and must never cause
       * the SD memory automatic-CMD12 sequence.
       */

      if ((cmd & MMCSD_CMDIDX_MASK) == SDIO_CMDIDX53 &&
          (arg & (1u << 27)) != 0)
        {
          value |= SDHI_SD_CMD_MD5 | SDHI_SD_CMD_STOP_NO_AUTO;
        }
    }
  else if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      return -ENOTSUP;
    }

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        value |= SDHI_SD_CMD_MD_NORESP;
        break;
      case MMCSD_R1_RESPONSE:
      case MMCSD_R5_RESPONSE:
      case MMCSD_R6_RESPONSE:
      case MMCSD_R7_RESPONSE:
        value |= SDHI_SD_CMD_MD_R1;
        break;
      case MMCSD_R1B_RESPONSE:
        value |= SDHI_SD_CMD_MD_R1B;
        break;
      case MMCSD_R2_RESPONSE:
        value |= SDHI_SD_CMD_MD_R2;
        break;
      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        value |= SDHI_SD_CMD_MD_R3;
        break;
      default:
        return -EINVAL;
    }

  ret = rzv2h_idle(priv, false);
  if (ret < 0)
    {
      return ret;
    }

  flags = enter_critical_section();
  rzv2h_mask(priv);
  rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1,
                   ~(SDHI_STATUS_CMDEVENTS | SDHI_STATUS_DATAEVENTS));
  if (cmd == MMCSD_CMD0)
    {
#ifdef CONFIG_RZV2H_SDIO_UHS
      priv->cmd8_valid = false;
      priv->uhs_requested = false;
#endif
      priv->highspeed = false;
      priv->sdcard = false;
      priv->selected = false;
      priv->cmd6_supported = false;
      priv->hs_checked = false;
    }

  priv->sdprobe = cmd == SD_ACMD41 && priv->appcmd;
#ifdef CONFIG_RZV2H_SDIO_UHS
  if (priv->config.allow_uhs &&
      priv->sdprobe)
    {
      priv->uhs_requested = priv->cmd8_valid &&
                            (arg & SDHI_OCR_HCS_CCS) != 0 &&
                            (arg & SDHI_OCR_VOLTAGE_MASK) != 0;
      if (priv->uhs_requested)
        {
          arg |= SDHI_OCR_S18;
        }
    }
#endif

  priv->command = cmd;
  priv->argument = arg;
  priv->setcount = setcount;
  priv->appcmd = false;
  priv->result = -EINPROGRESS;
  priv->pending = true;
  priv->datacmd = data;
  priv->multiactive = multi;
  priv->autostop = multi && priv->blockcount == 0;
  priv->blockcount = 0;
  priv->stopvalid = false;
  memset(priv->response, 0, sizeof(priv->response));

  if (data)
    {
      /* SECCNT is one contiguous 32-bit register, not a pair of separated
       * halfwords like ARG/RSP/INFO.  Also enable finite CMD53 block mode.
       */

      putreg32(priv->nblocks, priv->base + RZV2H_SDHI_SD_SECCNT);
      rzv2h_write16(priv, RZV2H_SDHI_SD_STOP,
                    (value & SDHI_SD_CMD_MD5) != 0 ? SDHI_SD_STOP_SEC : 0);
      rzv2h_write16(priv, RZV2H_SDHI_SD_SIZE, priv->blocklen);
#ifdef CONFIG_RZV2H_SDIO_DMA
      if (priv->dma.prepared && cmd != MMCSD_CMD17 &&
          cmd != MMCSD_CMD18 && cmd != MMCSD_CMD24 && cmd != MMCSD_CMD25)
        {
          rzv2h_stopdma(priv);
          rzv2h_write16(priv, RZV2H_SDHI_HOST_MODE, SDHI_HOST_MODE_32BIT);
        }
#endif
    }

  rzv2h_updatemask(priv);
  rzv2h_writepair(priv, RZV2H_SDHI_SD_ARG, arg);

  rzv2h_write16(priv, RZV2H_SDHI_SD_CMD, value);
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rzv2h_waitresponse
 *
 * Description:
 *   Poll for a command result, including R1b busy release and timeout
 *   cleanup.
 *
 *   Consume completion through the same handler used by the ISR and
 *   require R1b busy release. Recheck a terminal IRQ result before
 *   retiring a true timeout; the finite poll budget is not a calibrated
 *   duration.
 *
 ****************************************************************************/

static int rzv2h_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint32_t retries = SDHI_RESPONSE_POLLS;
  irqstate_t flags;
  bool done;
  int ret;

  if (cmd != priv->command)
    {
      return -EINVAL;
    }

  do
    {
      flags = enter_critical_section();
      rzv2h_complete(priv);
      done = !priv->pending;
      ret = priv->result;
      leave_critical_section(flags);

      /* Response end can precede busy release for R1b. */

      if (done && (ret < 0 ||
          (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R1B_RESPONSE ||
          (rzv2h_readpair(priv, RZV2H_SDHI_SD_INFO1) &
           SDHI_STATUS_BUSY) == 0))
        {
          goto response_done;
        }

      /* Finite command-response retry budget, not a calibrated delay.
       * IRQs remain enabled between completion checks.  Real timeout
       * duration depends on CPU, compiler, MMIO latency and interrupts.
       */
    }
  while (--retries > 0);

  flags = enter_critical_section();

  /* An IRQ can finish the command after the last poll sampled its state.
   * Preserve that terminal result before retiring a true timeout. R1b still
   * requires busy release; a recorded error does not.
   */

  ret = priv->result;
  if (!priv->pending && (ret < 0 ||
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R1B_RESPONSE ||
      (rzv2h_readpair(priv, RZV2H_SDHI_SD_INFO1) &
       SDHI_STATUS_BUSY) == 0))
    {
      leave_critical_section(flags);
      goto response_done;
    }

  priv->pending = false;
  priv->result = -ETIMEDOUT;
  rzv2h_writepair(priv, RZV2H_SDHI_SD_INFO1, ~SDHI_STATUS_CMDEVENTS);
  if ((priv->datacmd || priv->setcount) &&
      (priv->xfrstate == SDHI_XFR_PREPARED ||
                        priv->xfrstate == SDHI_XFR_ACTIVE))
    {
      rzv2h_endtransfer(priv, -ETIMEDOUT);
    }
  else
    {
      rzv2h_updatemask(priv);
    }

  leave_critical_section(flags);
  mcerr("ERROR: SDHI%u CMD%u poll-budget timeout\n",
        priv->config.channel, (unsigned int)(cmd & MMCSD_CMDIDX_MASK));
  return -ETIMEDOUT;

response_done:
  if (ret < 0)
    {
#ifdef CONFIG_RZV2H_SDIO_UHS_SDR104
      /* A rejected tuning probe is not a final tuning failure.
       * Keep its negative result so the scan excludes this tap.
       */

      if (priv->tuning && cmd == SD_CMD19 && ret == -EILSEQ)
        {
          mcinfo("SDHI%u CMD19 tuning probe rejected: %d\n",
                 priv->config.channel, ret);
        }
      else
#endif
        {
          mcerr("ERROR: SDHI%u CMD%u failed: %d\n",
                priv->config.channel,
                (unsigned int)(cmd & MMCSD_CMDIDX_MASK), ret);
        }
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_recvshort
 *
 * Description:
 *   Return a short response and apply the SD discovery protocol hooks.
 *
 *   Validate that the requested response matches the completed command,
 *   return its saved value and update discovery context for CMD8/ACMD41.
 *   UHS negotiation modifies only the intended protocol fields.
 *
 ****************************************************************************/

static int rzv2h_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t *response)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  irqstate_t flags;
  uint32_t value;
#ifdef CONFIG_RZV2H_SDIO_UHS
  uint32_t argument;
  bool probe;
#endif
  int ret;

  if (priv->fault < 0)
    {
      return priv->fault;
    }

  if (response == NULL || cmd != priv->command ||
      (cmd & MMCSD_RESPONSE_MASK) == MMCSD_R2_RESPONSE ||
      (cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();
  ret = priv->result;
  value = priv->response[0];
#ifdef CONFIG_RZV2H_SDIO_UHS
  argument = priv->argument;
  probe = priv->sdprobe;
#endif
  leave_critical_section(flags);

#ifdef CONFIG_RZV2H_SDIO_UHS
  if (ret == OK && cmd == SD_CMD8)
    {
      priv->cmd8_valid = (argument & SDHI_CMD8_CHECK_MASK) ==
        SDHI_CMD8_CHECK_PATTERN &&
                        (value & SDHI_CMD8_CHECK_MASK) ==
                          SDHI_CMD8_CHECK_PATTERN;
    }

  if (ret == OK &&
      priv->config.allow_uhs &&
      cmd == SD_ACMD41 &&
      probe && !priv->voltage18 && (argument & SDHI_OCR_VOLTAGE_MASK) != 0 &&
      (value & SDHI_OCR_READY) != 0)
    {
      if (!priv->uhs_requested ||
          (value & (SDHI_OCR_HCS_CCS | SDHI_OCR_S18)) !=
          (SDHI_OCR_HCS_CCS | SDHI_OCR_S18))
        {
          ret = -ENOTSUP;
        }
      else
        {
          /* No waits or private commands inside the critical section.
           * Return the original OCR, not CMD11's R1, to the caller.
           */

          ret = rzv2h_switchvoltage(dev);
        }

      flags = enter_critical_section();
      if (ret == OK && (priv->mediaremoved || priv->fault < 0))
        {
          ret = priv->mediaremoved ? -ENODEV : priv->fault;
        }

      priv->command = cmd;
      priv->argument = argument;
      priv->response[0] = value;
      priv->sdprobe = probe;
      priv->pending = false;
      priv->result = ret;
      if (ret < 0)
        {
          rzv2h_uhs_fail(priv, ret);
        }

      leave_critical_section(flags);
    }
#endif

  if (ret == OK)
    {
      *response = value;
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_recvlong
 *
 * Description:
 *   Return the long response and prepare private UHS after eligible CSD
 *   reads.
 *
 *   Validate and copy the saved long response under protection. Decode
 *   CSD support and perform private UHS preparation only in the eligible
 *   SD discovery context.
 *
 ****************************************************************************/

static int rzv2h_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t response[4])
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  uint32_t value[4];
#ifdef CONFIG_RZV2H_SDIO_UHS
  uint32_t argument;
#endif
  irqstate_t flags;
  int ret;

  if (priv->fault < 0)
    {
      return priv->fault;
    }

  if (response == NULL || cmd != priv->command ||
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R2_RESPONSE)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();
  ret = priv->result;
  memcpy(value, priv->response, sizeof(value));
#ifdef CONFIG_RZV2H_SDIO_UHS
  argument = priv->argument;
#endif
  leave_critical_section(flags);

  if (ret == OK)
    {
#if defined(CONFIG_RZV2H_SDIO_HIGHSPEED) || defined(CONFIG_RZV2H_SDIO_UHS)
      if (cmd == MMCSD_CMD9)
        {
          priv->cmd6_supported = priv->sdcard && (value[1] & (1u << 30));
        }

#endif
#ifdef CONFIG_RZV2H_SDIO_UHS
      if (priv->config.allow_uhs &&
          cmd == MMCSD_CMD9 &&
          !priv->uhs_ready)
        {
          ret = rzv2h_uhs_prepare(priv, argument);
          flags = enter_critical_section();
          if (ret == OK && (priv->mediaremoved || priv->fault < 0))
            {
              ret = priv->mediaremoved ? -ENODEV : priv->fault;
            }

          priv->command = cmd;
          priv->argument = argument;
          memcpy(priv->response, value, sizeof(value));
          priv->pending = false;
          priv->datacmd = false;
          priv->result = ret;
          if (ret < 0)
            {
              rzv2h_uhs_fail(priv, ret);
            }

          leave_critical_section(flags);
        }
#endif
    }

  /* A failed private sequence must not expose capacity to the upper half. */

  if (ret == OK)
    {
      memcpy(response, value, sizeof(value));
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_setuptransfer
 *
 * Description:
 *   Validate buffers and geometry, then prepare shared transfer state.
 *
 *   Reject invalid geometry, buffers or an active/failed transfer, and
 *   reject hardware-busy setup before modifying register/buffer state.
 *   Prepare direction, buffer ownership, byte counts and PIO registers,
 *   returning the accepted byte count or a negative errno.
 *
 ****************************************************************************/

static int rzv2h_setuptransfer(struct rzv2h_sdio_s *priv, bool read,
                               uint8_t *rxbuffer,
                               const uint8_t *txbuffer, size_t nbytes)
{
  irqstate_t flags;
  int ret = OK;

  if (nbytes == 0 || nbytes > SDHI_MAX_BLOCK_SIZE * SDHI_MAX_BLOCKS ||
      (read ? rxbuffer == NULL : txbuffer == NULL))
    {
      return -EINVAL;
    }

  flags = enter_critical_section();
  if (priv->xfrstate == SDHI_XFR_ACTIVE ||
      (priv->pending && priv->datacmd))
    {
      ret = -EBUSY;
    }
  else if (priv->xfrstate == SDHI_XFR_ERROR)
    {
      ret = priv->xfrresult < 0 ? priv->xfrresult : -EIO;
    }
  else if ((rzv2h_read16(priv, RZV2H_SDHI_SD_INFO2) &
            SDHI_SD_INFO2_CBSY) != 0)
    {
      ret = -EBUSY;
    }
  else
    {
#ifdef CONFIG_RZV2H_SDIO_DMA
      rzv2h_stopdma(priv);
#endif
      if (!priv->geometry)
        {
          priv->blocklen = nbytes;
          priv->nblocks = 1;
          priv->geometry = true;
        }

      if (priv->blocklen == 0 || priv->blocklen > SDHI_MAX_BLOCK_SIZE ||
          priv->nblocks == 0 || priv->nblocks > SDHI_MAX_BLOCKS ||
          (size_t)priv->blocklen * priv->nblocks != nbytes)
        {
          ret = -EINVAL;
        }
      else
        {
          priv->read = read;
          priv->rxbuffer = rxbuffer;
          priv->txbuffer = txbuffer;
          priv->remaining = nbytes;
          priv->total = nbytes;
          priv->xfrresult = -EINPROGRESS;
          priv->xfrstate = SDHI_XFR_PREPARED;
          priv->datacmd = false;
          rzv2h_write16(priv, RZV2H_SDHI_CC_EXT_MODE,
                        SDHI_CC_EXT_MODE_RESET);
          rzv2h_write16(priv, RZV2H_SDHI_HOST_MODE,
                        SDHI_HOST_MODE_32BIT);
          rzv2h_write16(priv, RZV2H_SDHI_SD_SIZE, priv->blocklen);
          ret = nbytes;
        }
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: rzv2h_recvsetup
 *
 * Description:
 *   Prepare a PIO read using the caller's receive buffer.
 *
 *   Pass the receive buffer and byte count to the shared transfer setup
 *   helper. Success follows the PIO byte-count return convention.
 *
 ****************************************************************************/

static int rzv2h_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                            size_t nbytes)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;

  return rzv2h_setuptransfer(priv, true, buffer, NULL, nbytes);
}

/****************************************************************************
 * Name: rzv2h_sendsetup
 *
 * Description:
 *   Prepare a PIO write using the caller's transmit buffer.
 *
 *   Pass the transmit buffer and byte count to the shared transfer setup
 *   helper. Success follows the PIO byte-count return convention.
 *
 ****************************************************************************/

static int rzv2h_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                            size_t nbytes)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;

  return rzv2h_setuptransfer(priv, false, NULL, buffer, nbytes);
}

#ifdef CONFIG_RZV2H_SDIO_DMA
/****************************************************************************
 * Name: rzv2h_dmasetup
 *
 * Description:
 *   Prepare transfer state and attempt DMA when slot policy permits it.
 *
 *   Wait for an idle host, prepare shared transfer state and ask the DMA
 *   backend to prepare when board policy permits it. Return OK on
 *   success, unlike the PIO byte-count convention.
 *
 ****************************************************************************/

static int rzv2h_dmasetup(struct sdio_dev_s *dev, uint8_t *rxbuffer,
                         const uint8_t *txbuffer, size_t nbytes)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  bool read = rxbuffer != NULL;
  irqstate_t flags;
  int ret;

  ret = rzv2h_idle(priv, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = rzv2h_setuptransfer(priv, read, rxbuffer, txbuffer, nbytes);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->config.allow_dma)
    {
      return OK;
    }

  flags = enter_critical_section();
  ret = rzv2h_sdiodma_prepare(&priv->dma, priv->base,
                             read ? rxbuffer : txbuffer,
                             nbytes, priv->blocklen, read);
  leave_critical_section(flags);

  /* The common DMA caller expects OK, unlike the byte-count PIO setup. */

  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: rzv2h_dmarecvsetup
 *
 * Description:
 *   Prepare a read through the common DMA setup path.
 *
 *   Pass the receive buffer to common DMA setup and return its status.
 *   The shared setup/backend determines whether DMA is prepared or
 *   supported PIO fallback remains in use.
 *
 ****************************************************************************/

static int rzv2h_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                             size_t nbytes)
{
  return rzv2h_dmasetup(dev, buffer, NULL, nbytes);
}

/****************************************************************************
 * Name: rzv2h_dmasendsetup
 *
 * Description:
 *   Prepare a write through the common DMA setup path.
 *
 *   Pass the transmit buffer to common DMA setup and return its status.
 *   The shared setup/backend determines whether DMA is prepared or
 *   supported PIO fallback remains in use.
 *
 ****************************************************************************/

static int rzv2h_dmasendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                             size_t nbytes)
{
  return rzv2h_dmasetup(dev, NULL, buffer, nbytes);
}
#endif

#ifdef CONFIG_SDIO_BLOCKSETUP
/****************************************************************************
 * Name: rzv2h_blocksetup
 *
 * Description:
 *   Record block geometry without replacing an active transfer.
 *
 *   Validate and retain block size/count for the next request. An in-
 *   flight data operation keeps its original geometry; invalid new
 *   geometry records an unsupported-transfer result.
 *
 ****************************************************************************/

static void rzv2h_blocksetup(struct sdio_dev_s *dev, unsigned int blocklen,
                             unsigned int nblocks)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  irqstate_t flags = enter_critical_section();

  if (priv->xfrstate == SDHI_XFR_ACTIVE ||
      (priv->pending && priv->datacmd))
    {
      /* This void callback cannot replace an in-flight request. Leave its
       * geometry, result and buffer ownership intact for completion.
       */

      leave_critical_section(flags);
      return;
    }
  else
    {
      priv->blocklen = blocklen;
      priv->nblocks = nblocks;
      priv->geometry = blocklen > 0 && blocklen <= SDHI_MAX_BLOCK_SIZE &&
                       nblocks > 0 && nblocks <= SDHI_MAX_BLOCKS;
      priv->xfrresult = priv->geometry ? OK : -ENOTSUP;
      priv->xfrstate = priv->geometry ? SDHI_XFR_IDLE : SDHI_XFR_ERROR;
      priv->datacmd = false;
    }

  leave_critical_section(flags);
}
#endif

/****************************************************************************
 * Name: rzv2h_cancel
 *
 * Description:
 *   Reset transfer state and issue CMD12 for an interrupted multiblock
 *   transfer.
 *
 *   Reset the host with recovery preservation and issue a standalone
 *   CMD12 when an interrupted multiblock session still requires
 *   stopping. Retain stop response or recovery failure for subsequent
 *   requests.
 *
 ****************************************************************************/

static int rzv2h_cancel(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  bool stop = priv->multiactive && !priv->mediaremoved;
  int ret;

  rzv2h_reset_host(priv, true);
  if (stop && priv->fault == OK)
    {
      /* A failed memory transfer needs CMD12 even after CMD23 (SD Part 1,
       * 4.15).  Reset the host to leave a stalled FIFO sequence, preserve
       * the negotiated session, then stop with a real standalone CMD12.
       * This runs in task context only.
       */

      ret = rzv2h_sendcmd(dev, MMCSD_CMD12, 0);
      if (ret == OK)
        {
          ret = rzv2h_waitresponse(dev, MMCSD_CMD12);
        }

      if (ret == OK)
        {
          priv->stopresponse = priv->response[0];
          priv->stopvalid = true;
          if ((priv->stopresponse & SDHI_R1_ERRORMASK) != 0)
            {
              ret = -EIO;
            }
        }

      priv->fault = ret;
    }

  return priv->fault;
}

/****************************************************************************
 * Name: rzv2h_waitenable
 *
 * Description:
 *   Arm requested transfer events and their optional timeout watchdog.
 *
 *   Cancel any old watchdog and drain stale semaphore posts before
 *   arming a new wait. Publish already completed/error states
 *   immediately, otherwise start the timeout watchdog when requested.
 *
 ****************************************************************************/

static void rzv2h_waitenable(struct sdio_dev_s *dev,
                             sdio_eventset_t events, uint32_t timeout)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  irqstate_t flags;
  clock_t delay;
  int ret;

  wd_cancel(&priv->waitwdog);
  while (nxsem_trywait(&priv->waitsem) == OK)
    {
    }

  flags = enter_critical_section();
  priv->waitevents = events & SDIOWAIT_ALLEVENTS;
  priv->wkupevent = 0;

  if ((events & ~(SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT |
                  SDIOWAIT_ERROR)) != 0)
    {
      rzv2h_endwait(priv, SDIOWAIT_ERROR);
      leave_critical_section(flags);
      return;
    }

  if (priv->xfrstate == SDHI_XFR_DONE)
    {
      rzv2h_endwait(priv, SDIOWAIT_TRANSFERDONE);
    }
  else if (priv->xfrstate == SDHI_XFR_ERROR)
    {
      rzv2h_endwait(priv, SDIOWAIT_ERROR);
    }
  else if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
    {
      if (timeout == 0)
        {
          rzv2h_eventtimeout((wdparm_t)priv);
        }
      else
        {
          delay = MSEC2TICK(timeout);
          if (delay < 1)
            {
              delay = 1;
            }

          ret = wd_start(&priv->waitwdog, delay, rzv2h_eventtimeout,
                         (wdparm_t)priv);
          if (ret < 0)
            {
              rzv2h_endwait(priv, SDIOWAIT_ERROR);
            }
        }
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rzv2h_eventwait
 *
 * Description:
 *   Wait for a terminal transfer event and recover errors in task
 *   context.
 *
 *   Consume the saved wake event, retire wait/geometry state and recover
 *   error or timeout events through cancellation. If semaphore waiting
 *   fails, retire the wait and cancel before returning an error event.
 *
 ****************************************************************************/

static sdio_eventset_t rzv2h_eventwait(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  sdio_eventset_t event;
  irqstate_t flags;
  int ret;

  for (; ; )
    {
      flags = enter_critical_section();
      event = priv->wkupevent;
      if (event != 0)
        {
          priv->wkupevent = 0;
          priv->waitevents = 0;
          priv->geometry = false;
          priv->datacmd = false;
          if (priv->xfrstate == SDHI_XFR_DONE ||
              priv->xfrstate == SDHI_XFR_ERROR)
            {
              priv->xfrstate = SDHI_XFR_IDLE;
            }

          leave_critical_section(flags);
#ifdef CONFIG_RZV2H_SDIO_XFRDEBUG
          /* Report in task context, before recovery resets transfer state.
           * The upper half serializes requests on this host.
           * ACCESSEND clears multiactive before this log; use the command
           * flags to identify the original transfer mode instead.
           */

          if ((priv->command & MMCSD_CMDIDX_MASK) == 17 ||
              (priv->command & MMCSD_CMDIDX_MASK) == 18 ||
              (priv->command & MMCSD_CMDIDX_MASK) == 24 ||
              (priv->command & MMCSD_CMDIDX_MASK) == 25)
            {
              mcinfo("SDHI%u CMD%u blocks=%u size=%u mode=%s "
                     "remaining=%zu result=%d event=%02x\n",
                     priv->config.channel,
                     (unsigned int)(priv->command & MMCSD_CMDIDX_MASK),
                     priv->nblocks, priv->blocklen,
                     (priv->command & MMCSD_MULTIBLOCK) != 0 ?
                     (priv->autostop ? "auto12" : "count23") : "single",
                     priv->remaining, priv->xfrresult,
                     (unsigned int)event);
            }
#endif

          if ((event & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) != 0)
            {
#ifdef CONFIG_RZV2H_SDIO_UHS_SDR104
              if (!priv->tuning || priv->command != SD_CMD19 ||
                  priv->xfrresult == -ETIMEDOUT || priv->mediaremoved ||
                  priv->fault != OK)
#endif
                {
                  mcerr("ERROR: SDHI%u transfer failed: %d\n",
                        priv->config.channel,
                        priv->xfrresult < 0 ? priv->xfrresult : -EIO);
                }

              /* Single-block reads also return without upper-half cancel.
               * Retire hardware status before any next request can start;
               * cancellation preserves the session or retains a fault.
               */

              rzv2h_cancel(dev);
            }

          return event;
        }

      if (priv->waitevents == 0)
        {
          leave_critical_section(flags);
          return SDIOWAIT_ERROR;
        }

      leave_critical_section(flags);
      ret = nxsem_wait_uninterruptible(&priv->waitsem);
      if (ret < 0)
        {
          /* Retire this wait before cancellation can post an error event.
           * A completion may have raced the semaphore failure.  Stop the
           * transfer before returning its buffer, preserving any recovery
           * fault just as for a consumed ERROR/TIMEOUT event.
           */

          flags = enter_critical_section();
          priv->waitevents = 0;
          priv->wkupevent = 0;
          leave_critical_section(flags);
          rzv2h_cancel(dev);
          return SDIOWAIT_ERROR;
        }
    }
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/****************************************************************************
 * Name: rzv2h_mediaworker
 *
 * Description:
 *   Deliver an armed media notification from work-queue context.
 *
 *   Sample configured media status, consume a matching one-shot
 *   notification mask and invoke the saved callback outside the critical
 *   section.
 *
 ****************************************************************************/

static void rzv2h_mediaworker(void *arg)
{
  struct rzv2h_sdio_s *priv = arg;
  worker_t callback = NULL;
  void *cbarg = NULL;
  sdio_eventset_t event;
  irqstate_t flags = enter_critical_section();

  if (priv->callback != NULL && !priv->cbclosing)
    {
      priv->mediaremoved =
        (rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) == 0;
      event = priv->mediaremoved ? SDIOMEDIA_EJECTED : SDIOMEDIA_INSERTED;
      if ((priv->cbevents & event) != 0)
        {
          priv->cbevents = 0;
          callback = priv->callback;
          cbarg = priv->cbarg;
        }
    }

  leave_critical_section(flags);
  if (callback != NULL)
    {
      /* Report the delivered media event, not initialization success.
       * Keep console output out of the IRQ and critical-section paths.
       */

      syslog(LOG_NOTICE, "SDHI%u: card %s\n", priv->config.channel,
             event == SDIOMEDIA_INSERTED ? "detected" : "removed");
      callback(cbarg);
    }

#ifdef CONFIG_RZV2H_SDIO_UHS
  /* Ejection handling has returned; the upper half must arm insertion
   * before the host prepares the empty slot. Other application
   * users must already be stopped. A too-fast insertion is not recovered.
   */

  if (priv->initialized && !priv->cbclosing && priv->callback != NULL &&
      priv->reinsert_needed && !priv->reinsert_ready &&
      priv->config.cd_source == RZV2H_CD_NATIVE &&
      (priv->cbevents & SDIOMEDIA_INSERTED) != 0 &&
      (rzv2h_status(&priv->dev) & SDIO_STATUS_PRESENT) == 0)
    {
      int ret = rzv2h_prepare_reinsert(priv);

      if (ret == OK)
        {
          syslog(LOG_NOTICE,
                 "SDHI%u: slot restart ready; reinsert card\n",
                 priv->config.channel);
        }
      else
        {
          syslog(LOG_ERR, "SDHI%u: slot preparation failed: %d\n",
                 priv->config.channel, ret);
        }
    }
#endif
}
#endif

/****************************************************************************
 * Name: rzv2h_callbackenable
 *
 * Description:
 *   Arm media notifications and queue a debounced card-presence check.
 *
 *   Store the requested insertion/ejection mask and queue a delayed
 *   media check when a callback is registered. Also report media that
 *   already matches the newly armed event.
 *
 ****************************************************************************/

static void rzv2h_callbackenable(struct sdio_dev_s *dev,
                                 sdio_eventset_t events)
{
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  irqstate_t flags = enter_critical_section();

  if (priv->config.cd_source != RZV2H_CD_FIXED && !priv->cbclosing)
    {
      priv->cbevents = events & (SDIOMEDIA_INSERTED | SDIOMEDIA_EJECTED);
      if (priv->callback != NULL && priv->cbevents != 0)
        {
          /* Also detect media that already matches the newly armed mask. */

          work_queue(HPWORK, &priv->cdwork, rzv2h_mediaworker, priv,
                     MSEC2TICK(priv->config.cd_debounce_ms));
        }
    }

  leave_critical_section(flags);
#endif
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/****************************************************************************
 * Name: rzv2h_registercallback
 *
 * Description:
 *   Replace or unregister a media callback after draining its running
 *   worker.
 *
 *   Require task context, close notification delivery and synchronously
 *   drain old queued/running work before replacing the callback and
 *   argument. Restore masking for the new registration afterward.
 *
 ****************************************************************************/

static int rzv2h_registercallback(struct sdio_dev_s *dev,
                                   worker_t callback, void *arg)
{
  struct rzv2h_sdio_s *priv = (struct rzv2h_sdio_s *)dev;
  irqstate_t flags;

  /* Replacement/unregister may wait for a worker holding the old argument. */

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  flags = enter_critical_section();
  if (priv->cbclosing)
    {
      leave_critical_section(flags);
      return -EBUSY;
    }

  priv->cbclosing = true;
  priv->cbevents = 0;
  rzv2h_updatemask(priv);
  leave_critical_section(flags);
  work_cancel_sync(HPWORK, &priv->cdwork);
  flags = enter_critical_section();
  priv->callback = callback;
  priv->cbarg = callback != NULL ? arg : NULL;
  priv->cbclosing = false;
  rzv2h_updatemask(priv);
  leave_critical_section(flags);
  return OK;
}
#endif

#ifdef CONFIG_SDIO_MUXBUS
/****************************************************************************
 * Name: rzv2h_lock
 *
 * Description:
 *   Reject unsupported multiplexed-bus locking.
 *
 *   Return -ENOSYS because multiplexed-bus locking is not implemented.
 *   Supported configurations reject this interface at configuration
 *   validation.
 *
 ****************************************************************************/

static int rzv2h_lock(struct sdio_dev_s *dev, bool lock)
{
  return -ENOSYS;
}
#endif

/****************************************************************************
 * Name: rzv2h_initialize_config
 *
 * Description:
 *   Validate slot policy and initialize the host after board resource
 *   setup.
 *
 *   Validate the immutable board record before SDHI MMIO and serialize
 *   initialization. Reuse only a matching initialized record, retain
 *   unsafe prior session restrictions and unwind software resources on
 *   reset failure.
 *
 ****************************************************************************/

static int rzv2h_initialize_config(
  const struct rzv2h_sdio_config_s *config, struct sdio_dev_s **dev)
{
  struct rzv2h_sdio_s *priv;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  *dev = NULL;
  if (config == NULL || config->channel >= 3 || config->input_clock == 0 ||
      config->input_clock > SDHI_IDMODE_MAX_HZ *
        SDHI_SD_CLK_CTRL_MAX_DIVIDER ||
      config->max_frequency < SDHI_IDMODE_MAX_HZ ||
      config->max_frequency > SDHI_SDR104_HZ ||
      (config->bus_width != 1 && config->bus_width != 4) ||
      config->intsel < RZV2H_INTSEL_FIRST ||
      config->intsel > RZV2H_INTSEL_LAST || config->priority > 15 ||
      config->wp_source < RZV2H_WP_NONE ||
      config->wp_source > RZV2H_WP_NATIVE ||
      config->cd_source < RZV2H_CD_NATIVE ||
      config->cd_source > RZV2H_CD_FIXED)
    {
      return -EINVAL;
    }

  /* Implemented feature subset, independent of board identity. */

  if (((config->allow_dma || config->allow_uhs) && config->channel > 1) ||
      (config->allow_uhs &&
       (config->bus_width != 4 ||
         config->max_current_ma < SDHI_CURRENT_200MA)))
    {
      return -ENOTSUP;
    }

#ifndef CONFIG_RZV2H_SDIO_DMA
  if (config->allow_dma)
    {
      return -ENOTSUP;
    }

#endif
#ifndef CONFIG_RZV2H_SDIO_UHS
  if (config->allow_uhs)
    {
      return -ENOTSUP;
    }

#endif
#ifndef CONFIG_RZV2H_SDIO_WIDTH4
  if (config->bus_width == 4)
    {
      return -ENOTSUP;
    }
#endif

  ret = nxmutex_lock(&g_initlock);
  if (ret < 0)
    {
      return ret;
    }

  priv = &g_sdio[config->channel];
  if (priv->initialized)
    {
      if (priv->config.input_clock != config->input_clock ||
          priv->config.intsel != config->intsel ||
          priv->config.priority != config->priority ||
          priv->config.cd_source != config->cd_source ||
          priv->config.wp_source != config->wp_source ||
          priv->config.wp_active_low != config->wp_active_low ||
          priv->config.max_frequency != config->max_frequency ||
          priv->config.max_current_ma != config->max_current_ma ||
          priv->config.cd_debounce_ms != config->cd_debounce_ms ||
          priv->config.bus_width != config->bus_width ||
          priv->config.allow_dma != config->allow_dma ||
          priv->config.allow_uhs != config->allow_uhs ||
          priv->config.embedded_card != config->embedded_card)
        {
          ret = -EBUSY;
        }
      else
        {
          *dev = &priv->dev;
          ret = OK;
        }

      goto out;
    }

  /* Software teardown does not reset the external card's electrical or
   * negotiated session. Do not erase its recovery restriction with memset.
   * Board/card recovery and a fresh firmware lifetime remain required.
   */

  if (priv->hsfailed
#ifdef CONFIG_RZV2H_SDIO_UHS
      || priv->uhs_started || priv->uhs_failed || priv->voltage18
#endif
     )
    {
      ret = -EIO;
      goto out;
    }

  memset(priv, 0, sizeof(*priv));
  priv->dev = g_sdiodev;
  priv->config = *config;
  priv->base = RZV2H_SDHI_BASE(config->channel);
  nxmutex_init(&priv->dev.mutex);
  ret = nxsem_init(&priv->waitsem, 0, 0);
  if (ret < 0)
    {
      nxmutex_destroy(&priv->dev.mutex);
      goto out;
    }

  /* No SDHI MMIO is accessed before the validated board handoff above.
   * The caller, not this validation, establishes resource readiness.
   */

  rzv2h_reset(&priv->dev);
  ret = priv->fault;
  if (ret < 0)
    {
      rzv2h_mask(priv);
      nxsem_destroy(&priv->waitsem);
      nxmutex_destroy(&priv->dev.mutex);
      goto out;
    }

  priv->initialized = true;
  *dev = &priv->dev;
out:
  nxmutex_unlock(&g_initlock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rzv2h_sdio_enable_cpg
 *
 * Description:
 *   Enable SDHI0/1 module clocks, release module stop and release CPG reset.
 *   The task-context caller exclusively owns the channel. Write enables
 *   preserve the peer channel. This does not configure pins/card supplies,
 *   negotiate card state or register an MMCSD block device.
 *
 ****************************************************************************/

int rzv2h_sdio_enable_cpg(unsigned int channel)
{
  uint32_t clocks;
  uint32_t mstop;
  uint32_t reset;
  uint32_t resetmon;
  irqstate_t flags;
  int ret;

  if (channel > 1
#ifndef CONFIG_RZV2H_SDIO_CHANNEL0
      || channel == 0
#endif
#ifndef CONFIG_RZV2H_SDIO_CHANNEL1
      || channel == 1
#endif
     )
    {
      return -ENODEV;
    }

  clocks = SDHI_CPG_CLOCK_FIELD_MASK <<
           (SDHI_CPG_CLOCK_BASE_SHIFT + SDHI_CPG_CLOCK_STRIDE * channel);
  mstop = 1u << (SDHI_CPG_MSTOP_BASE_SHIFT + channel);
  reset = 1u << (SDHI_CPG_RESET_BASE_SHIFT + channel);
  resetmon = 1u << (SDHI_CPG_RSTMON_BASE_SHIFT + channel);

  /* CPG fields: SDHI0 clocks 3..6, stop 2, reset 7/monitor 24;
   * SDHI1 clocks 7..10, stop 3, reset 8/monitor 25.  Write enables
   * restrict each write to this controller and preserve its sibling.
   */

  flags = enter_critical_section();
  R_CPG->CPG_CLKON_10 = (clocks << SDHI_CPG_WRITE_ENABLE_SHIFT) | clocks;
  leave_critical_section(flags);

  ret = rzv2h_sdio_waitreg(&R_CPG->CPG_CLKMON_5,
                           clocks,
                           clocks);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SDHI%u clocks did not start: %08" PRIx32
             "\n", channel, R_CPG->CPG_CLKMON_5);
      return ret;
    }

  flags = enter_critical_section();
  R_CPG->CPG_BUS_8_MSTOP = mstop << SDHI_CPG_WRITE_ENABLE_SHIFT;
  (void)R_CPG->CPG_BUS_8_MSTOP;
  leave_critical_section(flags);

  ret = rzv2h_sdio_waitreg(&R_CPG->CPG_BUS_8_MSTOP,
                           mstop, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SDHI%u bus remains stopped: %08" PRIx32
             "\n", channel, R_CPG->CPG_BUS_8_MSTOP);
      return ret;
    }

  flags = enter_critical_section();
  R_CPG->CPG_RST_10 = (reset << SDHI_CPG_WRITE_ENABLE_SHIFT) | reset;
  leave_critical_section(flags);

  ret = rzv2h_sdio_waitreg(&R_CPG->CPG_RSTMON_4,
                           resetmon, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SDHI%u reset did not release: %08" PRIx32
             "\n", channel, R_CPG->CPG_RSTMON_4);
    }

  return ret;
}

/****************************************************************************
 * Name: rzv2h_sdio_initialize
 *
 * Description:
 *   Initialize an enabled controller using its immutable board
 *   configuration.
 *
 *   Validate the channel and task context, obtain its board
 *   configuration and pass it to common initialization. Board resource
 *   readiness is established by the caller.
 *
 ****************************************************************************/

int rzv2h_sdio_initialize(int channel, struct sdio_dev_s **dev)
{
  const struct rzv2h_sdio_config_s *config = NULL;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  *dev = NULL;
  if (channel < 0 || channel >= 3)
    {
      return -EINVAL;
    }

  /* A board cannot enable a channel omitted from this driver build. */

  switch (channel)
    {
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
      case 0:
        break;
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
      case 1:
        break;
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL2
      case 2:
        break;
#endif
      default:
        return -ENODEV;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  ret = rzv2h_sdio_getconfig(channel, &config);
  if (ret < 0)
    {
      return ret;
    }

  if (config == NULL || config->channel != channel)
    {
      return -EINVAL;
    }

  return rzv2h_initialize_config(config, dev);
}

/****************************************************************************
 * Name: rzv2h_sdio_uninitialize
 *
 * Description:
 *   Quiesce host IRQ and clock state, then release software resources if
 *   safe.
 *
 *   Drain media work, reset host state and verify clock shutdown before
 *   detaching IRQ resources and destroying software synchronization
 *   objects. Retain resources on unconfirmed quiescence and preserve
 *   card-session recovery restrictions.
 *
 ****************************************************************************/

int rzv2h_sdio_uninitialize(struct sdio_dev_s *dev)
{
  struct rzv2h_sdio_s *priv = NULL;
  unsigned int i;
  int ret;
  int irq;

  for (i = 0; i < 3; i++)
    {
      if (dev == &g_sdio[i].dev)
        {
          priv = &g_sdio[i];
          break;
        }
    }

  if (priv == NULL)
    {
      return -EINVAL;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  ret = nxmutex_lock(&g_initlock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&g_initlock);
      return -ENODEV;
    }

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  ret = rzv2h_registercallback(dev, NULL, NULL);
  if (ret < 0)
    {
      nxmutex_unlock(&g_initlock);
      return ret;
    }
#endif

  rzv2h_reset(dev);

  /* Reset can retain a card-session fault, especially after CMD11. That
   * fault forbids communication, not verified clock shutdown and software
   * cleanup. Use register-only clock control, retaining the session flags.
   */

  ret = rzv2h_setclock(priv, 0);
  if (ret == OK &&
      (rzv2h_read16(priv, RZV2H_SDHI_SD_CLK_CTRL) &
       SDHI_SD_CLK_CTRL_SCLKEN) != 0)
    {
      priv->clocked = true;
      ret = -EIO;
    }

  if (ret < 0)
    {
      /* Quiescence is unconfirmed. Keep resources for a safe retry, with
       * IRQ delivery masked and commands blocked by the retained fault.
       */

      priv->fault = ret;
      rzv2h_mask(priv);
      if (priv->attached)
        {
          up_disable_irq(RZV2H_FSP_TO_GIC_IRQ(priv->config.intsel));
        }

      nxmutex_unlock(&g_initlock);
      return ret;
    }

  if (priv->attached)
    {
      irq = RZV2H_FSP_TO_GIC_IRQ(priv->config.intsel);
      up_disable_irq(irq);
      irq_detach(irq);
      rzv2h_intsel_disconnect_event((rzv2h_irqn_t)priv->config.intsel);
      priv->attached = false;
    }

  nxmutex_destroy(&priv->dev.mutex);
  nxsem_destroy(&priv->waitsem);
  priv->initialized = false;
  nxmutex_unlock(&g_initlock);
  return OK;
}

/****************************************************************************
 * Name: rzv2h_sdio_getmode
 *
 * Description:
 *   Report the tracked SD mode without changing the controller or card.
 *
 *   Read-only architecture API. The common MMC/SD interface is
 *   unchanged.
 *
 *   Validate the slot in task context and return its retained fault or
 *   tracked mode under protection. The reported value describes software
 *   negotiation state rather than a physical clock measurement.
 *
 ****************************************************************************/

int rzv2h_sdio_getmode(int channel, enum rzv2h_sdio_mode_e *mode)
{
  struct rzv2h_sdio_s *priv;
  enum rzv2h_sdio_mode_e value = RZV2H_SDIO_MODE_UNKNOWN;
  irqstate_t flags;
  int ret = OK;

  if (channel < 0 || channel >= 3 || mode == NULL)
    {
      return -EINVAL;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  flags = enter_critical_section();
  priv = &g_sdio[channel];
  if (!priv->initialized)
    {
      ret = -ENODEV;
    }
  else if (priv->fault < 0)
    {
      ret = priv->fault;
    }
  else if (!priv->mediaremoved)
    {
#ifdef CONFIG_RZV2H_SDIO_UHS
      if (priv->voltage18)
        {
          if (priv->uhs_ready)
            {
              value = priv->uhs_mode ==
                SDHI_SWITCH_SPEED_SDR104 ? RZV2H_SDIO_MODE_UHS_SDR104 :
                                          RZV2H_SDIO_MODE_UHS_SDR50;
            }
          else if (priv->uhs_mode == 0)
            {
              value = RZV2H_SDIO_MODE_UHS_SDR12;
            }
        }
      else
#endif
      if (priv->selected && priv->sdcard)
        {
          value = priv->highspeed ? RZV2H_SDIO_MODE_HIGH_SPEED :
                                   RZV2H_SDIO_MODE_DEFAULT;
        }
    }

  if (ret == OK)
    {
      *mode = value;
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: rzv2h_sdio_getcurrent
 *
 * Description:
 *   Report the verified allowance only after complete UHS preparation.
 *
 *   Return the negotiated allowance in milliamps only after successful
 *   UHS preparation with media present. Report unavailable data or
 *   retained faults otherwise; this API does not measure current
 *   consumption.
 *
 ****************************************************************************/

int rzv2h_sdio_getcurrent(int channel, unsigned int *milliamps)
{
  struct rzv2h_sdio_s *priv;
  irqstate_t flags;
  int ret = -ENODATA;

  if (channel < 0 || channel >= 3 || milliamps == NULL)
    {
      return -EINVAL;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  flags = enter_critical_section();
  priv = &g_sdio[channel];
  if (!priv->initialized)
    {
      ret = -ENODEV;
    }
  else if (priv->fault < 0)
    {
      ret = priv->fault;
    }
#ifdef CONFIG_RZV2H_SDIO_UHS
  else if (!priv->mediaremoved && priv->uhs_ready &&
           priv->current_limit != 0)
    {
      *milliamps = priv->current_limit;
      ret = OK;
    }
#endif

  leave_critical_section(flags);
  return ret;
}
