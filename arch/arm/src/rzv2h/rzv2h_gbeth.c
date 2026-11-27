/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_gbeth.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-queue DWMAC4/5 driver for the RZ/V2H GBETH controller.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <debug.h>

#include <arch/barriers.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/nuttx.h>
#include <nuttx/wqueue.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/ioctl.h>
#include <nuttx/net/ksz9131.h>
#include <nuttx/net/mdio.h>
#include <nuttx/net/mii.h>
#include <nuttx/net/netdev.h>
#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

#include "rzv2h_gbeth.h"
#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GBETH_TX_NDESC       CONFIG_RZV2H_GBETH_NTXDESC
#define GBETH_RX_NDESC       CONFIG_RZV2H_GBETH_NRXDESC
#define GBETH_CACHELINE      64   /* Cortex-R8 L1 D-cache line */

/* Whole cache lines per packet buffer: the cache maintenance of one buffer
 * must not touch its neighbour.
 */

#define GBETH_BUFSIZE        ((CONFIG_NET_ETH_PKTSIZE + 4 + \
                               GBETH_CACHELINE - 1) & ~(GBETH_CACHELINE - 1))
#define GBETH_TXWAIT_US      2000 /* > one full frame on the wire at 10M */

/* With RX zero copy the DMA writes the frame into the IOB at the position
 * of its Ethernet header (CONFIG_NET_LL_GUARDSIZE - ETH_HDRLEN), and may
 * fill a whole RX buffer from there.
 */

#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
#  if CONFIG_NET_LL_GUARDSIZE < 14
#    error RZV2H_GBETH_RX_ZEROCOPY needs NET_LL_GUARDSIZE >= 14
#  endif
#  if CONFIG_IOB_BUFSIZE - (CONFIG_NET_LL_GUARDSIZE - 14) < GBETH_BUFSIZE
#    error RZV2H_GBETH_RX_ZEROCOPY: IOB_BUFSIZE cannot hold an RX buffer
#  endif
#endif

/* Work queue for the interrupt bottom half and the TX poll, unless the
 * dedicated GBETH work queue (CONFIG_RZV2H_GBETH_WQ) is used.  Link polling
 * always runs on LPWORK.
 */

#define GBETH_WORK           LPWORK

#define GBETH0_MACMAR        0x0200
#define GBETH0_MACMDR        0x0204
#define GBETH0_MACCR         0x0000
#define GBETH0_MACPFR        0x0008
#define GBETH0_MACHTR(n)     (0x0010 + 4 * (n))
#define GBETH0_MACRC0        0x00a0
#define GBETH0_MACHF0        0x011c
#define GBETH0_MACHF1        0x0120
#define GBETH0_MACAHR        0x0300
#define GBETH0_MACALR        0x0304
#define GBETH0_MTLRDMR       0x0c30
#define GBETH0_MTLTOMR       0x0d00
#define GBETH0_MTLROMR       0x0d30
#define GBETH0_MTLRMPOC      0x0d34
#define GBETH0_DMR           0x1000
#define GBETH0_DCTCR0        0x1104
#define GBETH0_DCRCR0        0x1108
#define GBETH0_DCTLAR0       0x1114
#define GBETH0_DCRLAR0       0x111c
#define GBETH0_DCTTPR0       0x1120
#define GBETH0_DCRTPR0       0x1128
#define GBETH0_DCTRLR0       0x112c
#define GBETH0_DCRC2R0       0x1130
#define GBETH0_DCIER0        0x1134
#define GBETH0_DCRIWT0       0x1138
#define GBETH0_DCSR0         0x1160
#define GBETH0_MMCRIMR       0x070c
#define GBETH0_MMCTIMR       0x0710
#define GBETH0_MMCIRIMR      0x0800

#define GBETH_OWN           (1u << 31)
#define GBETH_FD            (1u << 29)
#define GBETH_LD            (1u << 28)
#define GBETH_RX_BUF_VALID  (1u << 24)
#define GBETH_RX_IOC        (1u << 30)
#define GBETH_RX_ERROR      (1u << 15)
#define GBETH_TX_IOC        (1u << 31)
#define GBETH_DMA_TI        (1u << 0)
#define GBETH_DMA_RI        (1u << 6)
#define GBETH_DMA_NIS       (1u << 15)
#define GBETH_DMA_AIS       (1u << 14)
#define GBETH_DMA_FBE       (1u << 12)
#define GBETH_DMA_RBU       (1u << 7)
#define GBETH_IRQ_MASK      (GBETH_DMA_TI | GBETH_DMA_RI | \
                             GBETH_DMA_NIS | GBETH_DMA_AIS | \
                             GBETH_DMA_FBE | GBETH_DMA_RBU)
#define GBETH_MAC_TE        (1u << 1)
#define GBETH_MAC_RE        (1u << 0)
#define GBETH_MAC_DM        (1u << 13)
#define GBETH_MAC_FES       (1u << 14)
#define GBETH_MAC_PS        (1u << 15)
#define GBETH_MAC_IPC       (1u << 27)
#define GBETH_MAC_FILTER_PR (1u << 0)
#define GBETH_MAC_FILTER_HMC (1u << 2)
#define GBETH_MAC_FILTER_PM (1u << 4)
#define GBETH_MAC_FILTER_HPF (1u << 10)

/* MAC_Packet_Filter: own unicast and broadcast are always accepted (IPv4
 * needs broadcast for ARP and DHCP).  Multicast memberships are maintained
 * by d_addmac/d_rmmac; promiscuous mode remains a separate option.
 */

#ifdef CONFIG_NET_PROMISCUOUS
#  define GBETH_FILTER_PR   GBETH_MAC_FILTER_PR
#else
#  define GBETH_FILTER_PR   0
#endif

/* DWMAC4/5 MAC_HW_Feature1.HASHTBLSZ: 0 = none, 1/2/3 = 64/128/256
 * hash bits.  The hash registers start at 0x10, with 32 bits per word.
 * See Linux stmmac dwmac4.h and dwmac4_dma.c.
 */

#define GBETH_HWF1_HASH(v)   (((v) >> 24) & 3u)
#define GBETH_MCAST_MAXHASH  256
#define GBETH_MCAST_MAXREGS  (GBETH_MCAST_MAXHASH / 32)

/* Checksum offload (CONFIG_RZV2H_GBETH_CSUM_OFFLOAD).  MAC_HW_Feature0
 * reports TX checksum insertion and RX checksum checking; TDES3.CIC = 3
 * inserts the IPv4 header and TCP/UDP/ICMP checksums with the pseudo-header
 * computed in hardware; the RX write-back RDES1 (valid when RDES3.RS1V is
 * set) reports the payload type and the checksum results.
 */

#define GBETH_HWF0_TXCOE    (1u << 14)
#define GBETH_HWF0_RXCOE    (1u << 16)
#define GBETH_TX_CIC_FULL   (3u << 16)
#define GBETH_RX_RS1V       (1u << 26)
#define GBETH_RDES1_PT(v)   ((v) & 7u)
#define GBETH_RDES1_PT_UDP  1
#define GBETH_RDES1_PT_TCP  2
#define GBETH_RDES1_IPHE    (1u << 3)
#define GBETH_RDES1_IPV4    (1u << 4)
#define GBETH_RDES1_IPCB    (1u << 6)
#define GBETH_RDES1_IPCE    (1u << 7)

#define GBETH_HWF1_RXFIFO(v) ((v) & 0x1fu)
#define GBETH_HWF1_TXFIFO(v) (((v) >> 6) & 0x1fu)
#define GBETH_MTL_TQS_SHIFT 16
#define GBETH_MTL_TQS_MASK  (0x1fu << GBETH_MTL_TQS_SHIFT)
#define GBETH_MTL_RQS_SHIFT 20
#define GBETH_MTL_RQS_MASK  (0x1fu << GBETH_MTL_RQS_SHIFT)

/* MTL_RxQ0_Missed_Packet_Overflow_Cnt (clear on read): frames dropped
 * because the RX FIFO overflowed, and frames dropped because the RX DMA had
 * no free descriptor.
 */

#define GBETH_MISS_OVF(v)   ((v) & 0x7ffu)
#define GBETH_MISS_MIS(v)   (((v) >> 16) & 0x7ffu)

/* DMA_CH0_TX/RX_Control TxPBL/RxPBL: beats per DMA burst.  The reset value
 * 0 is undefined; 8 beats is well below half of the 8 KB queue.
 */

#define GBETH_DMA_PBL_SHIFT 16
#define GBETH_DMA_PBL_MASK  (0x3fu << GBETH_DMA_PBL_SHIFT)
#define GBETH_DMA_PBL       (8u << GBETH_DMA_PBL_SHIFT)

/* RX interrupt coalescing: only every GBETH_RX_COAL-th RX descriptor asks
 * for an interrupt (IOC); a frame in any other descriptor raises RI after
 * the DMA_CH0_RX_Interrupt_Watchdog_Timer count (RIWT, units of 256 clock
 * cycles).  Without the watchdog every descriptor needs IOC, or a frame
 * could wait for the next IOC indefinitely.
 */

#if CONFIG_RZV2H_GBETH_RX_WATCHDOG > 0
#  define GBETH_RX_COAL     CONFIG_RZV2H_GBETH_RX_COAL_FRAMES
#else
#  define GBETH_RX_COAL     1
#endif

#if GBETH_RX_COAL > CONFIG_RZV2H_GBETH_NRXDESC
#  error RZV2H_GBETH_RX_COAL_FRAMES must not exceed RZV2H_GBETH_NRXDESC
#endif
#if (CONFIG_RZV2H_GBETH_NRXDESC % GBETH_RX_COAL) != 0
#  error RZV2H_GBETH_NRXDESC must be a multiple of RZV2H_GBETH_RX_COAL_FRAMES
#endif
#define GBETH_RX_IOC_AT(i)  ((i) % GBETH_RX_COAL == GBETH_RX_COAL - 1 ? \
                             GBETH_RX_IOC : 0)

/* Frames gbeth_receive() handles per work run (CONFIG_RZV2H_GBETH_RX_BUDGET,
 * 0 = until the ring is empty).
 */

#if CONFIG_RZV2H_GBETH_RX_BUDGET > 0
#  define GBETH_RX_BUDGET   CONFIG_RZV2H_GBETH_RX_BUDGET
#else
#  define GBETH_RX_BUDGET   UINT_MAX
#endif

/* TX completion coalescing: only every GBETH_TX_COAL-th TX descriptor asks
 * for the TX interrupt.  The other descriptors are reclaimed by the next
 * work run, gbeth_txpoll() or gbeth_txready(); a full ring always contains
 * a descriptor with IOC, so the interrupt still restarts a stalled TX poll.
 */

#define GBETH_TX_COAL       CONFIG_RZV2H_GBETH_TX_COAL_FRAMES
#if GBETH_TX_COAL > CONFIG_RZV2H_GBETH_NTXDESC
#  error RZV2H_GBETH_TX_COAL_FRAMES must not exceed RZV2H_GBETH_NTXDESC
#endif
#define GBETH_TX_IOC_AT(i)  ((i) % GBETH_TX_COAL == GBETH_TX_COAL - 1 ? \
                             GBETH_TX_IOC : 0)

/* The CR8 DTCM, at its core address and as other bus masters (the GBETH
 * DMA) reach it through the AXI slave port [FSP: rzv2h/cr/
 * bsp_slave_address.h, used by r_dmac_b.c for its own DMA addresses].
 */

#define GBETH_DTCM_BASE     0x00020000
#define GBETH_DTCM_SIZE     0x00020000
#ifdef CONFIG_RZV2H_CORTEXR8_1
#  define GBETH_DTCM_AXI    0x120a0000
#else
#  define GBETH_DTCM_AXI    0x12060000
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gbeth_desc_s
{
  volatile uint32_t des0;
  volatile uint32_t des1;
  volatile uint32_t des2;
  volatile uint32_t des3;
};

/* The descriptor rings, which the GBETH DMA and the CPU both poll.  They are
 * placed in the .dma linker section, which the MPU maps Normal
 * non-cacheable, so they need no cache maintenance with the D-cache on.
 * .dma is NOLOAD (not zeroed at boot); gbeth_ifup() initializes them.  The
 * packet buffers are ordinary cacheable memory (g_gbeth_txbuf/rxbuf) that
 * the driver cleans (TX) and invalidates (RX) around each DMA transfer.
 * With CONFIG_RZV2H_GBETH_RX_ZEROCOPY the RX buffers are IOBs of the stack
 * (priv->rxiob), which must be in DTCM, which is not cached.  With
 * CONFIG_RZV2H_GBETH_TXBUF_DTCM the TX buffers and netbuf are in DTCM too,
 * and the TX buffers need no clean.
 */

struct gbeth_dma_s
{
  struct gbeth_desc_s txdesc[GBETH_TX_NDESC] __attribute__((aligned(64)));
  struct gbeth_desc_s rxdesc[GBETH_RX_NDESC] __attribute__((aligned(64)));
};

/* Driver state.  txdesc and rxdesc point into g_gbeth_dma, txbuf and rxbuf
 * into g_gbeth_txbuf/rxbuf (SRAM, cacheable; txbuf in DTCM with
 * CONFIG_RZV2H_GBETH_TXBUF_DTCM), or rxiob holds the IOB of each RX
 * descriptor (CONFIG_RZV2H_GBETH_RX_ZEROCOPY); netbuf points to
 * g_gbeth_netbuf, which is only used by the CPU (the stack's frame buffer).
 */

struct gbeth_priv_s
{
  struct net_driver_s dev;
  struct mdio_lowerhalf_s mdio;
  struct mdio_bus_s *bus;
  const struct rzv2h_gbeth_config_s *cfg;
  struct work_s irqwork;
  struct work_s txwork;
  struct work_s linkwork;
#ifdef CONFIG_RZV2H_GBETH_WQ
  struct kwork_wqueue_s *wq;     /* Dedicated RX/TX work queue or NULL */
#endif
  struct gbeth_desc_s *txdesc;
  struct gbeth_desc_s *rxdesc;
  uint8_t (*txbuf)[GBETH_BUFSIZE];
#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
  struct iob_s *rxiob[GBETH_RX_NDESC]; /* IOB each RX descriptor fills */
#else
  uint8_t (*rxbuf)[GBETH_BUFSIZE];
#endif
  uint8_t *netbuf;
  unsigned int txhead;
  unsigned int txtail;
  unsigned int txused;
  unsigned int rxhead;
  unsigned int speed;
  unsigned int rxarp_logged;     /* ARP frames seen, limits the log lines */
#ifdef CONFIG_NET_MCASTGROUP
  /* Memberships per hash bit; index width is 0 without hardware hashing. */

  uint16_t mcast_refs[GBETH_MCAST_MAXHASH];
  uint8_t mcast_bits;
#endif
#ifdef CONFIG_RZV2H_GBETH_CSUM_OFFLOAD
  bool txcsum;                   /* GBETH inserts TX checksums */
  bool rxcsum;                   /* GBETH checks RX checksums */
#endif
  bool full;
  bool txdefer;                  /* Batch TX doorbells (gbeth_txflush()) */
  bool txkick;                   /* TX descriptors queued, no doorbell yet */
  bool up;
  bool link;
  bool fault;
  bool link_reported;
  bool speed_reported;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct gbeth_priv_s g_gbeth;
static struct gbeth_dma_s g_gbeth_dma locate_data(".dma");
#ifdef CONFIG_RZV2H_GBETH_TXBUF_DTCM
static uint8_t g_gbeth_txbuf[GBETH_TX_NDESC][GBETH_BUFSIZE]
  __attribute__((aligned(GBETH_CACHELINE))) locate_data(".dtcm.gbeth");
static uint8_t g_gbeth_netbuf[GBETH_BUFSIZE]
  __attribute__((aligned(GBETH_CACHELINE))) locate_data(".dtcm.gbeth");
#else
static uint8_t g_gbeth_txbuf[GBETH_TX_NDESC][GBETH_BUFSIZE]
  __attribute__((aligned(GBETH_CACHELINE)));
static uint8_t g_gbeth_netbuf[GBETH_BUFSIZE]
  __attribute__((aligned(GBETH_CACHELINE)));
#endif
#ifndef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
static uint8_t g_gbeth_rxbuf[GBETH_RX_NDESC][GBETH_BUFSIZE]
  __attribute__((aligned(GBETH_CACHELINE)));
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gbeth_get
 *
 * Description:
 *   Read a 32-bit GBETH register.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   offset - Register offset from the GBETH base address
 *
 * Returned Value:
 *   The register value.
 *
 ****************************************************************************/

static inline uint32_t gbeth_get(struct gbeth_priv_s *priv,
                                 unsigned int offset)
{
  return getreg32(priv->cfg->base + offset);
}

/****************************************************************************
 * Name: gbeth_put
 *
 * Description:
 *   Write a 32-bit GBETH register.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   offset - Register offset from the GBETH base address
 *   value  - Value to write
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void gbeth_put(struct gbeth_priv_s *priv, unsigned int offset,
                             uint32_t value)
{
  putreg32(value, priv->cfg->base + offset);
}

/****************************************************************************
 * Name: gbeth_dmaaddr
 *
 * Description:
 *   Return the address the GBETH DMA uses for a buffer: the AXI alias for
 *   a buffer in DTCM, the address itself otherwise.
 *
 * Input Parameters:
 *   buf - Buffer at its CPU address
 *
 * Returned Value:
 *   The bus address of the buffer.
 *
 ****************************************************************************/

static uint32_t gbeth_dmaaddr(const void *buf)
{
  uintptr_t addr = (uintptr_t)buf;

  if (addr >= GBETH_DTCM_BASE && addr - GBETH_DTCM_BASE < GBETH_DTCM_SIZE)
    {
      return addr - GBETH_DTCM_BASE + GBETH_DTCM_AXI;
    }

  return addr;
}

/****************************************************************************
 * Name: gbeth_rxframe
 *
 * Description:
 *   Return where RX descriptor 'index' receives its frame: the RX buffer,
 *   or, with RX zero copy, the Ethernet header position in its IOB.
 *
 * Input Parameters:
 *   priv  - Driver state
 *   index - RX descriptor index
 *
 * Returned Value:
 *   The CPU address of the frame buffer.
 *
 ****************************************************************************/

static uint8_t *gbeth_rxframe(struct gbeth_priv_s *priv, unsigned int index)
{
#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
  return &priv->rxiob[index]->io_data[CONFIG_NET_LL_GUARDSIZE - ETH_HDRLEN];
#else
  return priv->rxbuf[index];
#endif
}

/****************************************************************************
 * Name: gbeth_rxarm
 *
 * Description:
 *   Hand RX descriptor 'index' with its buffer to the DMA, OWN last.  The
 *   RX tail pointer is moved by the caller.
 *
 * Input Parameters:
 *   priv  - Driver state
 *   index - RX descriptor index
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gbeth_rxarm(struct gbeth_priv_s *priv, unsigned int index)
{
  struct gbeth_desc_s *desc = &priv->rxdesc[index];

  desc->des0 = gbeth_dmaaddr(gbeth_rxframe(priv, index));
  desc->des1 = 0;
  desc->des2 = 0;
  UP_DMB();
  desc->des3 = GBETH_RX_BUF_VALID | GBETH_RX_IOC_AT(index) | GBETH_OWN;
}

/****************************************************************************
 * Name: gbeth_mdio_wait
 *
 * Description:
 *   Wait until the MDIO busy bit (MDIO_Address.GB) clears, polling every
 *   10 us for up to 10 ms.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   Zero (OK) when the MDIO interface is idle; -ETIMEDOUT otherwise.
 *
 ****************************************************************************/

static int gbeth_mdio_wait(struct gbeth_priv_s *priv)
{
  int i;

  for (i = 0; i < 1000; i++)
    {
      if ((gbeth_get(priv, GBETH0_MACMAR) & 1) == 0)
        {
          return 0;
        }

      up_udelay(10);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: gbeth_mdio_read
 *
 * Description:
 *   MDIO lower-half read operation: read a Clause 22 PHY register through
 *   the GBETH station management interface.  Used by the KSZ9131 driver
 *   through the MDIO bus registered in rzv2h_gbeth_initialize().
 *
 * Input Parameters:
 *   lower - MDIO lower half embedded in the driver state
 *   phy   - PHY address (0-31)
 *   reg   - PHY register number (0-31)
 *   value - Location to return the 16-bit register value
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the MDIO interface stays busy.
 *
 ****************************************************************************/

static int gbeth_mdio_read(struct mdio_lowerhalf_s *lower, uint8_t phy,
                           uint8_t reg, uint16_t *value)
{
  struct gbeth_priv_s *priv =
    container_of(lower, struct gbeth_priv_s, mdio);
  int ret = gbeth_mdio_wait(priv);

  if (ret < 0)
    {
      return ret;
    }

  gbeth_put(priv, GBETH0_MACMAR, ((uint32_t)phy << 21) |
            ((uint32_t)reg << 16) | ((uint32_t)priv->cfg->mdio_csr << 8) |
            (3u << 2) | 1u);
  ret = gbeth_mdio_wait(priv);
  if (ret >= 0)
    {
      *value = gbeth_get(priv, GBETH0_MACMDR) & 0xffff;
    }

  return ret;
}

/****************************************************************************
 * Name: gbeth_mdio_write
 *
 * Description:
 *   MDIO lower-half write operation: write a Clause 22 PHY register
 *   through the GBETH station management interface.
 *
 * Input Parameters:
 *   lower - MDIO lower half embedded in the driver state
 *   phy   - PHY address (0-31)
 *   reg   - PHY register number (0-31)
 *   value - 16-bit value to write
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the MDIO interface stays busy.
 *
 ****************************************************************************/

static int gbeth_mdio_write(struct mdio_lowerhalf_s *lower, uint8_t phy,
                            uint8_t reg, uint16_t value)
{
  struct gbeth_priv_s *priv =
    container_of(lower, struct gbeth_priv_s, mdio);
  int ret = gbeth_mdio_wait(priv);

  if (ret < 0)
    {
      return ret;
    }

  gbeth_put(priv, GBETH0_MACMDR, value);
  gbeth_put(priv, GBETH0_MACMAR, ((uint32_t)phy << 21) |
            ((uint32_t)reg << 16) | ((uint32_t)priv->cfg->mdio_csr << 8) |
            (1u << 2) | 1u);
  return gbeth_mdio_wait(priv);
}

static const struct mdio_ops_s g_gbeth_mdio_ops =
{
  .read = gbeth_mdio_read,
  .write = gbeth_mdio_write
};

/****************************************************************************
 * Name: gbeth_txflush
 *
 * Description:
 *   Ring the TX doorbell for the descriptors gbeth_transmit() queued while
 *   doorbells were batched (priv->txdefer): one DSB and one TX tail pointer
 *   write for all of them.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static void gbeth_txflush(struct gbeth_priv_s *priv)
{
  if (priv->txkick)
    {
      priv->txkick = false;

      /* The descriptors must be in SRAM before the tail pointer wakes the
       * DMA (Linux stmmac: wmb()).  A DMB only orders the writes; the DMA
       * can still fetch an old OWN=0 word, suspend, and send the frames
       * only on the next doorbell.
       */

      UP_DSB();
      gbeth_put(priv, GBETH0_DCTTPR0,
                (uintptr_t)&priv->txdesc[priv->txhead]);
    }
}

/****************************************************************************
 * Name: gbeth_transmit
 *
 * Description:
 *   Queue the frame in dev->d_buf (d_len bytes) for transmission: copy it
 *   into the next TX buffer, fill the descriptor and hand it to the DMA by
 *   setting OWN last.  The TX tail pointer (doorbell) is written at once,
 *   or, while doorbells are batched, by the next gbeth_txflush().  Clears
 *   d_len on success.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY if there is no link, all TX descriptors
 *   are in use, or the frame length is zero or larger than a TX buffer.
 *   The frame is not queued on failure.
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static int gbeth_transmit(struct gbeth_priv_s *priv)
{
  struct gbeth_desc_s *desc;
  unsigned int index;
  size_t len = priv->dev.d_len;

  if (!priv->link || priv->txused == GBETH_TX_NDESC ||
      len == 0 || len > GBETH_BUFSIZE)
    {
      NETDEV_TXERRORS(&priv->dev);
      return -EBUSY;
    }

  index = priv->txhead;
  desc = &priv->txdesc[index];
  memcpy(priv->txbuf[index], priv->dev.d_buf, len);

#ifndef CONFIG_RZV2H_GBETH_TXBUF_DTCM
  up_clean_dcache((uintptr_t)priv->txbuf[index],
                  (uintptr_t)priv->txbuf[index] + len);
#endif
  desc->des0 = gbeth_dmaaddr(priv->txbuf[index]);
  desc->des1 = 0;
  desc->des2 = (uint32_t)len | GBETH_TX_IOC_AT(index);
  UP_DMB();
  desc->des3 = (uint32_t)len | GBETH_FD | GBETH_LD | GBETH_OWN
#ifdef CONFIG_RZV2H_GBETH_CSUM_OFFLOAD
                | (priv->txcsum ? GBETH_TX_CIC_FULL : 0)
#endif
                ;

  priv->txhead = (index + 1) % GBETH_TX_NDESC;
  priv->txused++;
  NETDEV_TXPACKETS(&priv->dev);
  priv->txkick = true;
  if (!priv->txdefer)
    {
      gbeth_txflush(priv);
    }

  priv->dev.d_len = 0;
  return 0;
}

/****************************************************************************
 * Name: gbeth_reclaim
 *
 * Description:
 *   Release TX descriptors that the DMA has finished (OWN cleared), oldest
 *   first, so they can be reused.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static void gbeth_reclaim(struct gbeth_priv_s *priv)
{
  while (priv->txused > 0 &&
         !(priv->txdesc[priv->txtail].des3 & GBETH_OWN))
    {
      priv->txtail = (priv->txtail + 1) % GBETH_TX_NDESC;
      priv->txused--;
      NETDEV_TXDONE(&priv->dev);
    }
}

/****************************************************************************
 * Name: gbeth_txpoll
 *
 * Description:
 *   devif_poll() callback.  Transmit the frame the network stack has just
 *   built, and stop the poll once the TX ring is full so that the stack
 *   does not build a frame that cannot be queued.  Ring the TX doorbell
 *   every half ring, so that the DMA can finish descriptors during the
 *   poll, and reclaim them before giving up on a full ring.
 *
 * Input Parameters:
 *   dev - Network device
 *
 * Returned Value:
 *   Zero to continue polling, -EBUSY to stop it (transmit failed or the TX
 *   ring is still full).
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static int gbeth_txpoll(struct net_driver_s *dev)
{
  struct gbeth_priv_s *priv = dev->d_private;

  if (dev->d_len > 0)
    {
      if (gbeth_transmit(priv) < 0)
        {
          return -EBUSY;
        }

      if (priv->txhead % (GBETH_TX_NDESC / 2) == 0)
        {
          gbeth_txflush(priv);
        }
    }

  if (priv->txused == GBETH_TX_NDESC)
    {
      gbeth_reclaim(priv);
    }

  return priv->txused == GBETH_TX_NDESC ? -EBUSY : 0;
}

/****************************************************************************
 * Name: gbeth_txready
 *
 * Description:
 *   Make sure one TX descriptor is free.  Reclaim finished descriptors and,
 *   if the ring is still full, busy-wait up to GBETH_TXWAIT_US for the DMA
 *   to finish one.  Used before sending a reply from the receive path,
 *   where the frame cannot be kept for later.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   True if a TX descriptor is free; false if the ring stayed full.
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static bool gbeth_txready(struct gbeth_priv_s *priv)
{
  unsigned int i;

  /* A full ring drains at wire speed, so wait briefly for one descriptor
   * rather than drop a frame the stack already counts as sent.  Batched
   * frames are handed to the DMA first, or the ring could never drain.
   */

  gbeth_txflush(priv);
  gbeth_reclaim(priv);
  for (i = 0; priv->txused == GBETH_TX_NDESC && i < GBETH_TXWAIT_US; i++)
    {
      up_udelay(1);
      gbeth_reclaim(priv);
    }

  return priv->txused < GBETH_TX_NDESC;
}

#ifdef CONFIG_RZV2H_GBETH_CSUM_OFFLOAD
/****************************************************************************
 * Name: gbeth_rxcsum
 *
 * Description:
 *   Tell the stack, for the frame about to be passed up, whether the GBETH
 *   has already verified its IPv4 header and TCP/UDP checksums.  Only a
 *   frame the hardware checked and found good gets NETDEV_RX_CSUM; every
 *   other frame (checksum error, bypassed, ICMP, ARP, no RDES1) is checked
 *   by the stack in software, as Linux stmmac does.
 *
 * Input Parameters:
 *   priv - Driver state
 *   desc - Written-back RX descriptor of the frame
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked; the flag is only
 *   read by the input call that follows.
 *
 ****************************************************************************/

static void gbeth_rxcsum(struct gbeth_priv_s *priv,
                         const struct gbeth_desc_s *desc)
{
  uint32_t rdes1 = desc->des1;
  bool good = priv->rxcsum && (desc->des3 & GBETH_RX_RS1V) != 0 &&
              (rdes1 & (GBETH_RDES1_IPV4 | GBETH_RDES1_IPHE |
                        GBETH_RDES1_IPCB | GBETH_RDES1_IPCE)) ==
              GBETH_RDES1_IPV4 &&
              (GBETH_RDES1_PT(rdes1) == GBETH_RDES1_PT_TCP ||
               GBETH_RDES1_PT(rdes1) == GBETH_RDES1_PT_UDP);

  if (good)
    {
      priv->dev.d_features |= NETDEV_RX_CSUM;
    }
  else
    {
      priv->dev.d_features &= ~NETDEV_RX_CSUM;
    }
}
#endif

/****************************************************************************
 * Name: gbeth_rxiob
 *
 * Description:
 *   Make the frame received by RX descriptor 'index' the device's current
 *   packet in an IOB (d_iob, d_buf at its Ethernet header), the way
 *   netdev_input() does from netbuf: copy it from its RX buffer into a
 *   fresh IOB or, with RX zero copy, take the IOB the DMA filled and give
 *   the descriptor a fresh one instead.
 *
 * Input Parameters:
 *   priv  - Driver state
 *   index - RX descriptor index; d_len is the frame length
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value if no IOB is free or the
 *   frame does not fit.  netbuf stays the device buffer, and the
 *   descriptor keeps its buffer, on failure.
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static int gbeth_rxiob(struct gbeth_priv_s *priv, unsigned int index)
{
  struct net_driver_s *dev = &priv->dev;
#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
  struct iob_s *iob = priv->rxiob[index];
  struct iob_s *fresh;

  /* Without a free IOB the frame is dropped and the descriptor keeps its
   * IOB, so the ring never runs dry.
   */

  fresh = iob_tryalloc(false);
  if (fresh == NULL)
    {
      return -ENOMEM;
    }

  priv->rxiob[index] = fresh;

  /* The frame starts at the Ethernet header position, so the IOB looks as
   * after netdev_iob_prepare() + iob_trycopyin(..., -NET_LL_HDRLEN(dev)).
   */

  iob->io_flink  = NULL;
  iob->io_offset = CONFIG_NET_LL_GUARDSIZE;
  iob->io_len    = dev->d_len - NET_LL_HDRLEN(dev);
  iob->io_pktlen = iob->io_len;
  dev->d_iob     = iob;
  dev->d_buf     = NETLLBUF;
  return OK;
#else
  int ret;

  ret = netdev_iob_prepare(dev, false, 0);
  if (ret == OK)
    {
      ret = iob_trycopyin(dev->d_iob, priv->rxbuf[index], dev->d_len,
                          -NET_LL_HDRLEN(dev), false);
      if (ret == dev->d_len)
        {
          return OK;
        }

      netdev_iob_release(dev);
      ret = -ENOMEM;
    }

  dev->d_buf = priv->netbuf;
  return ret;
#endif
}

#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
/****************************************************************************
 * Name: gbeth_rxiob_init
 *
 * Description:
 *   Give every RX descriptor an IOB.  The IOBs stay with the driver across
 *   ifdown/ifup; only missing ones are allocated.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   Zero (OK) on success; -ENOMEM if the IOB pool is too small, -EINVAL if
 *   it is not in DTCM.
 *
 ****************************************************************************/

static int gbeth_rxiob_init(struct gbeth_priv_s *priv)
{
  unsigned int i;

  for (i = 0; i < GBETH_RX_NDESC; i++)
    {
      if (priv->rxiob[i] == NULL)
        {
          priv->rxiob[i] = iob_tryalloc(false);
          if (priv->rxiob[i] == NULL)
            {
              nerr("ERROR: no IOB for RX descriptor %u\n", i);
              return -ENOMEM;
            }
        }

      /* The DMA writes the IOBs and the CPU reads them without any cache
       * maintenance, which only DTCM allows.
       */

      if (gbeth_dmaaddr(gbeth_rxframe(priv, i)) ==
          (uintptr_t)gbeth_rxframe(priv, i))
        {
          nerr("ERROR: RX zero copy needs the IOB pool in DTCM "
               "(CONFIG_IOB_SECTION)\n");
          return -EINVAL;
        }
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: gbeth_rxiob_done
 *
 * Description:
 *   Finish a frame passed in through gbeth_rxiob(): copy a reply the stack
 *   left in d_iob (TCP ACK, ICMP echo reply) to netbuf for
 *   gbeth_transmit(), release whatever IOB the stack did not keep, and make
 *   netbuf the device buffer again.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static void gbeth_rxiob_done(struct gbeth_priv_s *priv)
{
  struct net_driver_s *dev = &priv->dev;

  if (dev->d_iob != NULL && dev->d_len > 0)
    {
      iob_copyout(priv->netbuf, dev->d_iob, dev->d_len,
                  -NET_LL_HDRLEN(dev));
    }
  else
    {
      dev->d_len = 0;
    }

  netdev_iob_release(dev);
  dev->d_buf = priv->netbuf;
}

/****************************************************************************
 * Name: gbeth_rxnetbuf
 *
 * Description:
 *   Pass a frame that is in netbuf (d_buf) to the network stack by its
 *   EtherType: IPv4 (when it does not take the IOB path), IPv6 and ARP.
 *   Other EtherTypes are dropped.
 *
 * Input Parameters:
 *   priv - Driver state
 *   eth  - Ethernet header of the frame in netbuf
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static void gbeth_rxnetbuf(struct gbeth_priv_s *priv,
                           const struct eth_hdr_s *eth)
{
  struct net_driver_s *dev = &priv->dev;

#ifdef CONFIG_NET_PKT
  pkt_input(dev);
#endif
#ifdef CONFIG_NET_IPv4
  if (eth->type == HTONS(ETHTYPE_IP))
    {
      NETDEV_RXIPV4(dev);
      ipv4_input(dev);
    }
  else
#endif
#ifdef CONFIG_NET_IPv6
  if (eth->type == HTONS(ETHTYPE_IP6))
    {
      NETDEV_RXIPV6(dev);
      ipv6_input(dev);
    }
  else
#endif
#ifdef CONFIG_NET_ARP
  if (eth->type == HTONS(ETHTYPE_ARP))
    {
      FAR const uint8_t *a = dev->d_buf;

      NETDEV_RXARP(dev);

      /* Bring-up diagnostic: show who is ARPing on the wire.  Offsets are
       * from the Ethernet header: opcode at 20, sender MAC at 22, sender IP
       * at 28, target IP at 38.
       */

      if (++priv->rxarp_logged <= 8 && dev->d_len >= 42)
        {
          syslog(LOG_INFO,
                 "GBETH0 rx ARP op=%u from %02x:%02x:%02x:%02x:"
                 "%02x:%02x %u.%u.%u.%u -> %u.%u.%u.%u\n",
                 a[21], a[22], a[23], a[24], a[25], a[26], a[27],
                 a[28], a[29], a[30], a[31],
                 a[38], a[39], a[40], a[41]);
        }

      arp_input(dev);
    }
  else
#endif
    {
      NETDEV_RXDROPPED(dev);
      dev->d_len = 0;
    }
}

/****************************************************************************
 * Name: gbeth_receive
 *
 * Description:
 *   Process every frame the RX DMA has completed.  Valid single-descriptor
 *   IPv4 frames are copied into an IOB (with RX zero copy, passed in the
 *   IOB the DMA filled) and passed to ipv4_input(); other
 *   frames are copied to netbuf and passed on by gbeth_rxnetbuf().  A
 *   reply left by the stack (d_len > 0) is transmitted at once.  Each
 *   descriptor is then returned to the DMA, and the RX tail pointer is
 *   moved once at the end.  Frames with errors, split frames and unknown
 *   EtherTypes are dropped.  At most GBETH_RX_BUDGET frames are handled.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   The number of RX descriptors handled.
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static unsigned int gbeth_receive(struct gbeth_priv_s *priv)
{
  struct net_driver_s *dev = &priv->dev;
  struct gbeth_desc_s *desc;
  struct eth_hdr_s *eth;
  uint8_t *frame;
  unsigned int index;
  unsigned int len;
  unsigned int count = 0;

  while (count < GBETH_RX_BUDGET &&
         !(priv->rxdesc[priv->rxhead].des3 & GBETH_OWN))
    {
      index = priv->rxhead;
      desc = &priv->rxdesc[index];
      len = desc->des3 & 0x7fff;

      /* The MAC includes the Ethernet FCS in the DMA length. */

      if (!(desc->des3 & GBETH_RX_ERROR) &&
          (desc->des3 & (GBETH_FD | GBETH_LD)) ==
          (GBETH_FD | GBETH_LD) &&
          len >= 4 + sizeof(struct eth_hdr_s) &&
          len - 4 <= CONFIG_NET_ETH_PKTSIZE)
        {
          dev->d_len = len - 4;
          NETDEV_RXPACKETS(dev);
          frame = gbeth_rxframe(priv, index);
#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
          UP_DMB(); /* Read the frame only after its descriptor */
#else
          up_invalidate_dcache((uintptr_t)frame,
                               (uintptr_t)frame + GBETH_BUFSIZE);
#endif
          eth = (struct eth_hdr_s *)frame;
#ifdef CONFIG_RZV2H_GBETH_CSUM_OFFLOAD
          gbeth_rxcsum(priv, desc);
#endif

#if defined(CONFIG_NET_IPv4) && !defined(CONFIG_NET_PKT)
          /* IPv4 goes straight from the RX buffer into an IOB (with zero
           * copy it already is in one), which ipv4_input() then uses as is.
           * Through netbuf, netdev_input() would copy every frame a second
           * time.
           */

          if (eth->type == HTONS(ETHTYPE_IP))
            {
              if (gbeth_rxiob(priv, index) == OK)
                {
                  NETDEV_RXIPV4(dev);
                  ipv4_input(dev);
                  gbeth_rxiob_done(priv);
                }
              else
                {
                  NETDEV_RXDROPPED(dev);
                  dev->d_len = 0;
                }
            }
          else
#endif
            {
              memcpy(priv->netbuf, frame, dev->d_len);
              eth = (struct eth_hdr_s *)dev->d_buf;
              gbeth_rxnetbuf(priv, eth);
            }

          /* The reply (ARP/ICMP reply, TCP ACK or next TCP segment) lives
           * in netbuf, which the next received frame overwrites, so it
           * must be queued now.
           */

          if (dev->d_len > 0)
            {
              if (gbeth_txready(priv))
                {
                  gbeth_transmit(priv);
                }
              else
                {
                  NETDEV_TXERRORS(dev);
                }
            }
        }
      else
        {
          NETDEV_RXERRORS(dev);
        }

      dev->d_len = 0;
      gbeth_rxarm(priv, index);
      priv->rxhead = (index + 1) % GBETH_RX_NDESC;
      count++;
    }

  /* One doorbell for all re-armed descriptors; they must be in SRAM before
   * it, see gbeth_txflush().
   */

  if (count > 0)
    {
      UP_DSB();
      gbeth_put(priv, GBETH0_DCRTPR0,
                (uintptr_t)&priv->rxdesc[priv->rxhead]);
    }

  return count;
}

/****************************************************************************
 * Name: gbeth_rxmissed
 *
 * Description:
 *   Add the frames the hardware dropped before they reached a descriptor
 *   (RX FIFO overflow, or no free RX descriptor) to the RX error count, so
 *   that ring overruns show up in /proc/net/eth0.  Reading the counter
 *   clears it.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread with the network locked.
 *
 ****************************************************************************/

static void gbeth_rxmissed(struct gbeth_priv_s *priv)
{
#ifdef CONFIG_NETDEV_STATISTICS
  uint32_t value = gbeth_get(priv, GBETH0_MTLRMPOC);
  uint32_t count = GBETH_MISS_OVF(value) + GBETH_MISS_MIS(value);

  priv->dev.d_statistics.rx_errors += count;
  priv->dev.d_statistics.errors += count;
#endif
}

/****************************************************************************
 * Name: gbeth_queue
 *
 * Description:
 *   Queue RX/TX bottom-half work at once: on the dedicated GBETH work queue
 *   when it exists, else on GBETH_WORK.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   work   - priv->irqwork or priv->txwork
 *   worker - The work function
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   May be called from the GBETH interrupt handler.
 *
 ****************************************************************************/

static void gbeth_queue(struct gbeth_priv_s *priv, struct work_s *work,
                        worker_t worker)
{
#ifdef CONFIG_RZV2H_GBETH_WQ
  if (priv->wq != NULL)
    {
      work_queue_wq(priv->wq, work, worker, priv, 0);
      return;
    }
#endif

  work_queue(GBETH_WORK, work, worker, priv, 0);
}

/****************************************************************************
 * Name: gbeth_irqwork
 *
 * Description:
 *   Interrupt bottom half.  Clear the DMA status; on a fatal bus error
 *   stop the interface (carrier off, no recovery until ifdown/ifup);
 *   otherwise reclaim TX descriptors, receive frames, count the frames the
 *   hardware dropped and poll the stack for more TX data, with a TX
 *   doorbell per half ring for the frames queued.  Finally unmask the GBETH
 *   interrupt, or, if the RX budget ran out with frames still waiting,
 *   queue itself again.  After a run that received frames, yield, so that
 *   threads of the same priority (the socket reader) run before the next
 *   run.
 *
 * Input Parameters:
 *   arg - Driver state (struct gbeth_priv_s *)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on the GBETH work queue or GBETH_WORK; takes the network lock.
 *   The interrupt stays masked from gbeth_irq() until a run ends with the
 *   RX ring empty.
 *
 ****************************************************************************/

static void gbeth_irqwork(void *arg)
{
  struct gbeth_priv_s *priv = arg;
  uint32_t status;
  unsigned int count = 0;
  bool more = false;

  net_lock();
  priv->txdefer = true;
  if (priv->up)
    {
      status = gbeth_get(priv, GBETH0_DCSR0);
      gbeth_put(priv, GBETH0_DCSR0, status);
      if (status & GBETH_DMA_FBE)
        {
          nerr("GBETH fatal DMA error: %08" PRIx32 "\n", status);
          gbeth_put(priv, GBETH0_DCIER0, 0);
          netdev_carrier_off(&priv->dev);
          priv->link = false;
          priv->fault = true;
        }
      else
        {
          gbeth_reclaim(priv);
          count = gbeth_receive(priv);
          more = count >= GBETH_RX_BUDGET &&
                 !(priv->rxdesc[priv->rxhead].des3 & GBETH_OWN);
          gbeth_rxmissed(priv);
          gbeth_reclaim(priv);
          if (priv->link && priv->txused < GBETH_TX_NDESC)
            {
              devif_poll(&priv->dev, gbeth_txpoll);
            }
        }

      gbeth_txflush(priv);

      /* RX budget used up with frames still waiting: keep polling with the
       * interrupt masked.
       */

      if (more)
        {
          gbeth_queue(priv, &priv->irqwork, gbeth_irqwork);
        }
      else
        {
          up_enable_irq(priv->cfg->irq);
        }
    }

  priv->txdefer = false;
  net_unlock();

  /* Let ready threads of the same priority (the socket reader) empty the
   * socket queue before the next run.  Without this, the interrupt that
   * is unmasked with frames already waiting queues the next run at once,
   * and this work thread would never give up the CPU.
   */

  if (count > 0)
    {
      sched_yield();
    }
}

/****************************************************************************
 * Name: gbeth_irq
 *
 * Description:
 *   GBETH sbd_intr_o interrupt handler (level-triggered).  Mask the
 *   interrupt and defer all processing to gbeth_irqwork().
 *
 * Input Parameters:
 *   irq     - IRQ number
 *   context - Interrupted register context (unused)
 *   arg     - Driver state (struct gbeth_priv_s *)
 *
 * Returned Value:
 *   Always zero (OK).
 *
 * Assumptions:
 *   Runs in interrupt context.
 *
 ****************************************************************************/

static int gbeth_irq(int irq, void *context, void *arg)
{
  struct gbeth_priv_s *priv = arg;

  up_disable_irq(priv->cfg->irq);
  gbeth_queue(priv, &priv->irqwork, gbeth_irqwork);
  return 0;
}

/****************************************************************************
 * Name: gbeth_txwork
 *
 * Description:
 *   TX work queued by gbeth_txavail().  If the link is up, reclaim TX
 *   descriptors and, when one is free, poll the stack for frames.
 *
 * Input Parameters:
 *   arg - Driver state (struct gbeth_priv_s *)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on the GBETH work queue or GBETH_WORK; takes the network lock
 *   itself.
 *
 ****************************************************************************/

static void gbeth_txwork(void *arg)
{
  struct gbeth_priv_s *priv = arg;

  net_lock();
  if (priv->up && priv->link)
    {
      /* With the ring full, the stack would build a frame and lose it in
       * gbeth_transmit().  The TX-complete interrupt polls again.  The
       * frames of this poll share a TX doorbell per half ring, see
       * gbeth_txpoll().
       */

      priv->txdefer = true;
      gbeth_reclaim(priv);
      if (priv->txused < GBETH_TX_NDESC)
        {
          devif_poll(&priv->dev, gbeth_txpoll);
        }

      gbeth_txflush(priv);
      priv->txdefer = false;
    }

  net_unlock();
}

/****************************************************************************
 * Name: gbeth_txavail
 *
 * Description:
 *   d_txavail() hook.  Called by the network stack when it has data to
 *   send; schedules gbeth_txwork() unless it is already pending.
 *
 * Input Parameters:
 *   dev - Network device
 *
 * Returned Value:
 *   Always zero (OK).
 *
 * Assumptions:
 *   Called from the network stack, possibly from an application thread.
 *
 ****************************************************************************/

static int gbeth_txavail(struct net_driver_s *dev)
{
  struct gbeth_priv_s *priv = dev->d_private;

  if (priv->up && work_available(&priv->txwork))
    {
      gbeth_queue(priv, &priv->txwork, gbeth_txwork);
    }

  return 0;
}

/****************************************************************************
 * Name: gbeth_queue_size
 *
 * Description:
 *   Convert a MAC_HW_Feature1 FIFO size code (FIFO bytes = 128 << code)
 *   into an MTL TQS/RQS value (queue bytes = (value + 1) * 256), so that
 *   queue 0 can be given the whole FIFO.
 *
 * Input Parameters:
 *   fifosize - TXFIFOSIZE or RXFIFOSIZE field of MAC_HW_Feature1
 *
 * Returned Value:
 *   The TQS/RQS field value, from 0 (256 bytes) to 31 (8 KB).
 *
 ****************************************************************************/

static uint32_t gbeth_queue_size(uint32_t fifosize)
{
  /* HW_Feature1 encodes the FIFO as 128 << fifosize bytes; the TQS/RQS
   * fields encode a queue as (n + 1) * 256 bytes, at most 8 KB.
   */

  if (fifosize < 1)
    {
      return 0;
    }

  if (fifosize > 6)
    {
      return 31;
    }

  return (1u << (fifosize - 1)) - 1;
}

/****************************************************************************
 * Name: gbeth_set_macaddr
 *
 * Description:
 *   Program MAC_Address0 from dev->d_mac.  Called from gbeth_ifup() and
 *   again on every link-up, when the PHY receive clock is known to run.
 *
 * Input Parameters:
 *   priv - Driver state
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gbeth_set_macaddr(struct gbeth_priv_s *priv)
{
  const uint8_t *mac = priv->dev.d_mac.ether.ether_addr_octet;

  /* Write High before Low, as Linux stmmac_set_mac_addr() does: the Low
   * write latches the whole address into the RX clock domain, so the
   * reverse order leaves the unicast filter with a stale upper half and
   * drops frames addressed to this interface.  The latch needs a running
   * RXC from the PHY; right after the first PHY setup at power-on it may
   * not be, so the address is programmed again on link up.  Bit 31 (AE)
   * is read-only for address 0 but is set for consistency with Linux.
   */

  gbeth_put(priv, GBETH0_MACAHR,
            (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));
  gbeth_put(priv, GBETH0_MACALR,
            (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
            ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
}

/****************************************************************************
 * Name: gbeth_set_filter
 *
 * Description:
 *   Restore the multicast hash table and packet filter.  The feature
 *   register determines how many hash words exist.  Without a hash table,
 *   accept all multicast while any membership is present.  Keep unicast,
 *   broadcast and configured promiscuous reception unchanged.
 *
 * Assumptions:
 *   Network access is serialized by the caller.  Called during ifup,
 *   on link-up, or after a membership change while the interface is up.
 *
 ****************************************************************************/

static void gbeth_set_filter(struct gbeth_priv_s *priv)
{
  uint32_t value;
#ifdef CONFIG_NET_MCASTGROUP
  uint32_t hash[GBETH_MCAST_MAXREGS];
  unsigned int entries = 1u << priv->mcast_bits;
  unsigned int nregs = priv->mcast_bits == 0 ? 0 : entries / 32;
  unsigned int i;
  bool active = false;
#endif

  value = gbeth_get(priv, GBETH0_MACPFR);
  value &= ~(GBETH_MAC_FILTER_HMC | GBETH_MAC_FILTER_HPF |
             GBETH_MAC_FILTER_PM);
  value |= GBETH_FILTER_PR;

#ifdef CONFIG_NET_MCASTGROUP
  memset(hash, 0, sizeof(hash));
  for (i = 0; i < entries; i++)
    {
      if (priv->mcast_refs[i] != 0)
        {
          hash[i / 32] |= 1u << (i % 32);
          active = true;
        }
    }

  /* Program the table before selecting hash filtering.  HPF combines
   * perfect address matching with the multicast hash result.
   */

  for (i = 0; i < nregs; i++)
    {
      gbeth_put(priv, GBETH0_MACHTR(i), hash[i]);
    }

  if (active)
    {
      value |= nregs == 0 ? GBETH_MAC_FILTER_PM :
               GBETH_MAC_FILTER_HMC | GBETH_MAC_FILTER_HPF;
    }
#endif

  gbeth_put(priv, GBETH0_MACPFR, value);
}

#ifdef CONFIG_NET_MCASTGROUP
/****************************************************************************
 * Name: gbeth_mcast_index
 *
 * Description:
 *   Validate a multicast address and calculate its DWMAC hash index.  This
 *   is the high N bits of the bit-reversed, complemented Ethernet CRC32,
 *   equivalent to bitrev32(~crc32_le(~0, mac, 6)) in Linux dwmac4_core.c.
 *   Feed each byte least-significant bit first into a left-shifting CRC
 *   to obtain the reversed result directly.  No hashing uses bin zero.
 *
 * Returned Value:
 *   Hash index, or -EINVAL for NULL, unicast or broadcast addresses.
 *
 ****************************************************************************/

static int gbeth_mcast_index(struct gbeth_priv_s *priv, const uint8_t *mac)
{
  uint32_t crc = UINT32_MAX;
  uint32_t bit;
  unsigned int i;
  unsigned int j;
  uint8_t all = 0xff;
  uint8_t data;

  if (mac == NULL || (mac[0] & 1u) == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < ETHER_ADDR_LEN; i++)
    {
      all &= mac[i];
      data = mac[i];
      for (j = 0; j < 8; j++)
        {
          bit = (crc >> 31) ^ (data & 1u);
          crc <<= 1;
          if (bit != 0)
            {
              crc ^= 0x04c11db7u;
            }

          data >>= 1;
        }
    }

  if (all == 0xff)
    {
      return -EINVAL;
    }

  return priv->mcast_bits == 0 ? 0 : (~crc >> (32 - priv->mcast_bits));
}

/****************************************************************************
 * Name: gbeth_addmac
 *
 * Description:
 *   d_addmac() hook.  Count memberships per hash bin so duplicate adds and
 *   colliding addresses survive until the corresponding removes.  Keep
 *   counts while down, including memberships added by netdev_register(),
 *   and restore them after every DMA reset in gbeth_ifup().
 *
 * Returned Value:
 *   Zero on success; -EINVAL for an invalid address, or -EOVERFLOW if the
 *   hash-bin reference count cannot be incremented.
 *
 * Assumptions:
 *   The caller serializes network access.  Adds and removes are paired.
 *
 ****************************************************************************/

static int gbeth_addmac(struct net_driver_s *dev, const uint8_t *mac)
{
  struct gbeth_priv_s *priv = dev->d_private;
  int index = gbeth_mcast_index(priv, mac);

  if (index < 0)
    {
      return index;
    }

  if (priv->mcast_refs[index] == UINT16_MAX)
    {
      return -EOVERFLOW;
    }

  priv->mcast_refs[index]++;
  if (priv->up)
    {
      gbeth_set_filter(priv);
    }

  return OK;
}

/****************************************************************************
 * Name: gbeth_rmmac
 *
 * Description:
 *   d_rmmac() hook.  Drop a membership reference without clearing hash
 *   bits still needed by other memberships.  No MMIO is needed while
 *   down; the next ifup restores the remaining filter state.
 *
 * Returned Value:
 *   Zero on success; -EINVAL for an invalid address, or -ENOENT if the
 *   hash bin has no membership reference.
 *
 * Assumptions:
 *   The caller serializes network access.  Adds and removes are paired.
 *
 ****************************************************************************/

static int gbeth_rmmac(struct net_driver_s *dev, const uint8_t *mac)
{
  struct gbeth_priv_s *priv = dev->d_private;
  int index = gbeth_mcast_index(priv, mac);

  if (index < 0)
    {
      return index;
    }

  if (priv->mcast_refs[index] == 0)
    {
      return -ENOENT;
    }

  priv->mcast_refs[index]--;
  if (priv->up)
    {
      gbeth_set_filter(priv);
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: gbeth_linkwork
 *
 * Description:
 *   Periodic link supervision, rescheduled every
 *   CONFIG_RZV2H_GBETH_LINK_POLL_MS.  Read the PHY link state; on link up
 *   or a speed/duplex change set the board TX clock, the MAC speed and
 *   duplex, reprogram the MAC address, enable TX/RX and report carrier
 *   on; on link loss disable TX/RX and report carrier off.
 *
 * Input Parameters:
 *   arg - Driver state (struct gbeth_priv_s *)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on the LPWORK thread; takes the network lock itself.  Stops
 *   rescheduling after ifdown or a fatal DMA error.
 *
 ****************************************************************************/

static void gbeth_linkwork(void *arg)
{
  struct gbeth_priv_s *priv = arg;
  uint16_t bmsr = 0;
  uint32_t value;
  unsigned int speed;
  bool full;
  int mdio_ret;
  int ret;

  net_lock();
  if (!priv->up || priv->fault)
    {
      net_unlock();
      return;
    }

  ret = ksz9131_link_status(priv->bus, priv->cfg->phy.phyaddr,
                            &speed, &full);
  if (ret == 0 && (!priv->link || speed != priv->speed ||
                   full != priv->full))
    {
      if (!priv->speed_reported)
        {
          syslog(LOG_INFO, "GBETH0 PHY negotiated: %u Mbps %s duplex\n",
                 speed, full ? "full" : "half");
          priv->speed_reported = true;
        }

      value = gbeth_get(priv, GBETH0_MACCR);
      gbeth_put(priv, GBETH0_MACCR,
                value & ~(GBETH_MAC_TE | GBETH_MAC_RE));
      ret = priv->cfg->set_speed(speed);
      if (ret == 0)
        {
          value &= ~(GBETH_MAC_PS | GBETH_MAC_FES | GBETH_MAC_DM);
          if (speed != 1000)
            {
              value |= GBETH_MAC_PS;
            }

          if (speed == 100)
            {
              value |= GBETH_MAC_FES;
            }

          if (full)
            {
              value |= GBETH_MAC_DM;
            }

          gbeth_set_macaddr(priv);
          gbeth_set_filter(priv);
          gbeth_put(priv, GBETH0_MACCR,
                    value | GBETH_MAC_TE | GBETH_MAC_RE);
          priv->speed = speed;
          priv->full = full;
          priv->link = true;
          priv->link_reported = false;
          netdev_carrier_on(&priv->dev);
          syslog(LOG_INFO, "GBETH0 link up: %u Mbps %s duplex\n",
                 speed, full ? "full" : "half");
          devif_poll(&priv->dev, gbeth_txpoll);
        }
      else if (!priv->link_reported)
        {
          syslog(LOG_ERR, "GBETH0 speed setup failed: %u Mbps, %d\n",
                 speed, ret);
          priv->link_reported = true;
        }
    }
  else if (ret < 0 && priv->link)
    {
      priv->speed_reported = false;
      value = gbeth_get(priv, GBETH0_MACCR);
      gbeth_put(priv, GBETH0_MACCR,
                value & ~(GBETH_MAC_TE | GBETH_MAC_RE));
      priv->link = false;
      netdev_carrier_off(&priv->dev);
      syslog(LOG_WARNING, "GBETH0 link down: PHY status %d\n", ret);
      priv->link_reported = true;
    }
  else if (ret < 0 && !priv->link_reported)
    {
      priv->speed_reported = false;
      mdio_ret = mdio_read(priv->bus, priv->cfg->phy.phyaddr,
                           MII_MSR, &bmsr);
      syslog(LOG_WARNING,
             "GBETH0 link not ready: PHY status %d, BMSR=%04x, "
             "MDIO status %d\n", ret, bmsr, mdio_ret);
      priv->link_reported = true;
    }
  else if (ret < 0)
    {
      priv->speed_reported = false;
    }

  net_unlock();

  work_queue(LPWORK, &priv->linkwork, gbeth_linkwork,
             priv, MSEC2TICK(CONFIG_RZV2H_GBETH_LINK_POLL_MS));
}

/****************************************************************************
 * Name: gbeth_ifdown
 *
 * Description:
 *   d_ifdown() hook ('ifdown eth0').  Mask the interrupt, stop the TX/RX
 *   DMA and the MAC transmitter/receiver, cancel link supervision and
 *   report carrier off.  The PHY is left running.
 *
 * Input Parameters:
 *   dev - Network device
 *
 * Returned Value:
 *   Always zero (OK).
 *
 * Assumptions:
 *   Called by the network stack with the device locked.
 *
 ****************************************************************************/

static int gbeth_ifdown(struct net_driver_s *dev)
{
  struct gbeth_priv_s *priv = dev->d_private;

  priv->up = false;
  up_disable_irq(priv->cfg->irq);
  gbeth_put(priv, GBETH0_DCIER0, 0);
  gbeth_put(priv, GBETH0_DCTCR0,
            gbeth_get(priv, GBETH0_DCTCR0) & ~1u);
  gbeth_put(priv, GBETH0_DCRCR0,
            gbeth_get(priv, GBETH0_DCRCR0) & ~1u);
  gbeth_put(priv, GBETH0_MACCR,
            gbeth_get(priv, GBETH0_MACCR) &
            ~(GBETH_MAC_TE | GBETH_MAC_RE));
  work_cancel(LPWORK, &priv->linkwork);
  priv->link = false;
  priv->fault = false;
  priv->link_reported = false;
  priv->speed_reported = false;
  netdev_carrier_off(dev);
  return 0;
}

/****************************************************************************
 * Name: gbeth_ifup
 *
 * Description:
 *   d_ifup() hook ('ifup eth0').  Reset the DMA, initialize the PHY,
 *   program the MAC address, packet filter, checksum offload (if
 *   configured and present), MTL queue 0, DMA rings and burst length,
 *   start the TX/RX DMA, enable interrupts and start link supervision.
 *   The MAC transmitter/receiver are enabled later, by gbeth_linkwork(),
 *   once the PHY reports a link.
 *
 * Input Parameters:
 *   dev - Network device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *   -EADDRNOTAVAIL if no unicast MAC address has been set,
 *   -ETIMEDOUT if the DMA software reset does not complete, or the error
 *   returned by ksz9131_init().
 *
 * Assumptions:
 *   Called by the network stack with the device locked.
 *
 ****************************************************************************/

static int gbeth_ifup(struct net_driver_s *dev)
{
  struct gbeth_priv_s *priv = dev->d_private;
  uint8_t *mac = dev->d_mac.ether.ether_addr_octet;
  unsigned int i;
  uint32_t value;
  int ret;

  /* An interface without a unique unicast address cannot be started.
   * Board firmware may supply one; otherwise users set it via ifconfig.
   */

  if ((mac[0] & 1) ||
      ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0))
    {
      return -EADDRNOTAVAIL;
    }

  /* DMA software reset: set DMA_MODE.SWR, wait for it to clear.
   */

  gbeth_put(priv, GBETH0_DMR, 1);
  for (i = 0; i < 100000; i++)
    {
      if (!(gbeth_get(priv, GBETH0_DMR) & 1))
        {
          break;
        }

      up_udelay(1);
    }

  if (i == 100000)
    {
      return -ETIMEDOUT;
    }

  ret = ksz9131_init(priv->bus, &priv->cfg->phy);
  if (ret < 0)
    {
      return ret;
    }

  gbeth_set_macaddr(priv);
  gbeth_set_filter(priv);
  gbeth_put(priv, GBETH0_MACRC0,
            gbeth_get(priv, GBETH0_MACRC0) | 2u);
  gbeth_put(priv, GBETH0_MTLRDMR,
            gbeth_get(priv, GBETH0_MTLRDMR) & ~0xfu);

#ifdef CONFIG_RZV2H_GBETH_CSUM_OFFLOAD
  /* Use checksum offload only where MAC_HW_Feature0 reports it.  The DMA
   * reset above cleared MAC_Configuration, and gbeth_linkwork() only
   * read-modify-writes it, so IPC stays set.  TX insertion needs
   * store-and-forward, which MTL queue 0 uses (TSF below).
   */

  value = gbeth_get(priv, GBETH0_MACHF0);
  priv->txcsum = (value & GBETH_HWF0_TXCOE) != 0;
  priv->rxcsum = (value & GBETH_HWF0_RXCOE) != 0;
  dev->d_features &= ~(NETDEV_TX_CSUM | NETDEV_RX_CSUM);
  if (priv->txcsum)
    {
      dev->d_features |= NETDEV_TX_CSUM;
    }

  if (priv->rxcsum)
    {
      gbeth_put(priv, GBETH0_MACCR,
                gbeth_get(priv, GBETH0_MACCR) | GBETH_MAC_IPC);
    }
#endif

  /* GBETH has four queues, so TQS/RQS reset to 256 bytes and a 1514-byte
   * frame can never be stored in store-and-forward mode.  Only queue 0 is
   * used: give it the whole TX and RX FIFO, as Linux stmmac does.
   */

  value = gbeth_get(priv, GBETH0_MACHF1);
  gbeth_put(priv, GBETH0_MTLTOMR,
            (gbeth_get(priv, GBETH0_MTLTOMR) & ~GBETH_MTL_TQS_MASK) |
            (gbeth_queue_size(GBETH_HWF1_TXFIFO(value)) <<
             GBETH_MTL_TQS_SHIFT) | 0x0au);
  gbeth_put(priv, GBETH0_MTLROMR,
            (gbeth_get(priv, GBETH0_MTLROMR) & ~GBETH_MTL_RQS_MASK) |
            (gbeth_queue_size(GBETH_HWF1_RXFIFO(value)) <<
             GBETH_MTL_RQS_SHIFT) | 0x20u);

  priv->txhead = 0;
  priv->txtail = 0;
  priv->txused = 0;
  priv->rxhead = 0;
  priv->rxarp_logged = 0;
  memset(priv->txdesc, 0, sizeof(g_gbeth_dma.txdesc));

#ifdef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
  ret = gbeth_rxiob_init(priv);
  if (ret < 0)
    {
      return ret;
    }
#else
  /* Drop any dirty lines left in the RX buffers, so that they cannot be
   * written back over frames the DMA stores there.
   */

  up_invalidate_dcache((uintptr_t)g_gbeth_rxbuf,
                       (uintptr_t)g_gbeth_rxbuf + sizeof(g_gbeth_rxbuf));
#endif
  for (i = 0; i < GBETH_RX_NDESC; i++)
    {
      gbeth_rxarm(priv, i);
    }

  UP_DSB(); /* Rings in SRAM before the DMA is pointed at them */
  gbeth_put(priv, GBETH0_DCTLAR0, (uintptr_t)priv->txdesc);
  gbeth_put(priv, GBETH0_DCRLAR0, (uintptr_t)priv->rxdesc);
  gbeth_put(priv, GBETH0_DCTRLR0, GBETH_TX_NDESC - 1);
  gbeth_put(priv, GBETH0_DCRC2R0, GBETH_RX_NDESC - 1);
  gbeth_put(priv, GBETH0_DCTTPR0, (uintptr_t)priv->txdesc);
  gbeth_put(priv, GBETH0_DCRTPR0,
            (uintptr_t)priv->rxdesc + sizeof(g_gbeth_dma.rxdesc));
  gbeth_put(priv, GBETH0_DCRIWT0, CONFIG_RZV2H_GBETH_RX_WATCHDOG);
  value = gbeth_get(priv, GBETH0_DCRCR0);
  value = (value & ~(0x7ffeu | GBETH_DMA_PBL_MASK)) |
          (GBETH_BUFSIZE << 1) | GBETH_DMA_PBL;
  gbeth_put(priv, GBETH0_DCRCR0, value | 1u);
  value = gbeth_get(priv, GBETH0_DCTCR0);
  value = (value & ~GBETH_DMA_PBL_MASK) | GBETH_DMA_PBL;
  gbeth_put(priv, GBETH0_DCTCR0, value | 1u);
#ifdef CONFIG_RZV2H_GBETH_WQ
  /* The dedicated RX/TX work queue, created on the first ifup (task
   * context) and kept afterwards.
   */

  if (priv->wq == NULL)
    {
      priv->wq = work_queue_create("gbeth", CONFIG_RZV2H_GBETH_WQ_PRIORITY,
                                   NULL, CONFIG_RZV2H_GBETH_WQ_STACKSIZE,
                                   1);
      if (priv->wq == NULL)
        {
          nwarn("WARNING: no GBETH work queue, using the system one\n");
        }
    }

#endif

  /* Mask the MMC counter interrupts (as Linux dwmac_mmc_intr_all_mask()).
   * They are on after reset and reach the CPU through sbd_intr_o, but
   * only DMA_STATUS is cleared here: once the RX octet counter passes half
   * its range (~2 GB) the interrupt would stay asserted and fire again on
   * every up_enable_irq().  The counters are only read for diagnostics.
   */

  gbeth_put(priv, GBETH0_MMCRIMR, 0xffffffff);
  gbeth_put(priv, GBETH0_MMCTIMR, 0xffffffff);
  gbeth_put(priv, GBETH0_MMCIRIMR, 0xffffffff);
  gbeth_put(priv, GBETH0_DCSR0, 0xffffffff);
  gbeth_put(priv, GBETH0_DCIER0, GBETH_IRQ_MASK);
  priv->link = false;
  priv->fault = false;
  priv->link_reported = false;
  priv->speed_reported = false;
  priv->up = true;
  up_enable_irq(priv->cfg->irq);
  work_queue(LPWORK, &priv->linkwork, gbeth_linkwork, priv, 0);
  return 0;
}

#ifdef CONFIG_NETDEV_PHY_IOCTL
/****************************************************************************
 * Name: gbeth_ioctl
 *
 * Description:
 *   Handle application requests for the PHY address and Clause 22 register
 *   access.  Reuse the MDIO bus so transactions are serialized with the
 *   common PHY driver.  Register writes do not reconfigure the MAC or
 *   restart auto-negotiation automatically.
 *
 * Input Parameters:
 *   dev - Network device
 *   cmd - Network IOCTL command
 *   arg - Pointer to struct mii_ioctl_data_s, supplied by the network stack
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

static int gbeth_ioctl(struct net_driver_s *dev, int cmd, unsigned long arg)
{
  struct gbeth_priv_s *priv = dev->d_private;
  struct mii_ioctl_data_s *req =
    (struct mii_ioctl_data_s *)(uintptr_t)arg;
  uint16_t value;
  int ret;

  if (req == NULL)
    {
      return -EINVAL;
    }

  switch (cmd)
    {
      case SIOCGMIIPHY:
        req->phy_id = priv->cfg->phy.phyaddr;
        return OK;

      case SIOCGMIIREG:
      case SIOCSMIIREG:
        if (mdio_phy_id_is_c45(req->phy_id))
          {
            return -EOPNOTSUPP;
          }

        if (req->phy_id > 31 || req->reg_num > 31)
          {
            return -EINVAL;
          }

        if (cmd == SIOCGMIIREG)
          {
            ret = mdio_read(priv->bus, req->phy_id, req->reg_num, &value);
            if (ret == OK)
              {
                req->val_out = value;
              }

            return ret;
          }

        return mdio_write(priv->bus, req->phy_id, req->reg_num, req->val_in);

      default:
        return -ENOTTY;
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rzv2h_gbeth_initialize
 *
 * Description:
 *   Point the driver at its descriptor rings in the non-cacheable .dma
 *   section and its cacheable packet buffers,
 *   register the GBETH0 MDIO bus and the 'eth0' network device and attach
 *   the interrupt handler.  The MAC address is read from MAC_Address0;
 *   if it is not a valid unicast address, one must be set before ifup.
 *   The hardware itself is started later, by gbeth_ifup().
 *
 * Input Parameters:
 *   cfg - Board configuration.  Must remain valid for the life of the
 *         driver.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure: -EINVAL for
 *   an invalid configuration, -ENOMEM if the MDIO bus cannot be
 *   registered, or the error from irq_attach() or netdev_register().
 *
 * Assumptions:
 *   Called once from board bring-up, after the board has enabled the
 *   GBETH0 clocks, released its reset and routed its interrupt.
 *
 ****************************************************************************/

int rzv2h_gbeth_initialize(const struct rzv2h_gbeth_config_s *cfg)
{
  struct gbeth_priv_s *priv = &g_gbeth;
  uint32_t maclo;
  uint32_t machi;
  uint8_t *mac;
  int ret;

  if (cfg == NULL || cfg->set_speed == NULL || cfg->phy.phyaddr > 31)
    {
      return -EINVAL;
    }

  priv->cfg = cfg;

#ifdef CONFIG_NET_MCASTGROUP
  /* Discover the filter width before netdev_register() can add multicast
   * memberships.  Counts remain in software until the first ifup.
   */

  maclo = GBETH_HWF1_HASH(gbeth_get(priv, GBETH0_MACHF1));
  priv->mcast_bits = maclo == 0 ? 0 : maclo + 5;
#endif

  priv->txdesc = g_gbeth_dma.txdesc;
  priv->rxdesc = g_gbeth_dma.rxdesc;
  priv->txbuf = g_gbeth_txbuf;
  priv->netbuf = g_gbeth_netbuf;
#ifndef CONFIG_RZV2H_GBETH_RX_ZEROCOPY
  priv->rxbuf = g_gbeth_rxbuf;
#endif
  priv->mdio.ops = &g_gbeth_mdio_ops;
  priv->bus = mdio_register(&priv->mdio);
  if (priv->bus == NULL)
    {
      return -ENOMEM;
    }

  priv->dev.d_ifup = gbeth_ifup;
  priv->dev.d_ifdown = gbeth_ifdown;
  priv->dev.d_txavail = gbeth_txavail;
#ifdef CONFIG_NETDEV_PHY_IOCTL
  priv->dev.d_ioctl = gbeth_ioctl;
#endif
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_addmac = gbeth_addmac;
  priv->dev.d_rmmac = gbeth_rmmac;
#endif
  priv->dev.d_private = priv;
  priv->dev.d_buf = priv->netbuf;
  maclo = gbeth_get(priv, GBETH0_MACALR);
  machi = gbeth_get(priv, GBETH0_MACAHR);
  mac = priv->dev.d_mac.ether.ether_addr_octet;
  mac[0] = maclo;
  mac[1] = maclo >> 8;
  mac[2] = maclo >> 16;
  mac[3] = maclo >> 24;
  mac[4] = machi;
  mac[5] = machi >> 8;

  ret = irq_attach(cfg->irq, gbeth_irq, priv);
  if (ret >= 0)
    {
      ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
      if (ret < 0)
        {
          irq_detach(cfg->irq);
        }
    }

  if (ret < 0)
    {
      mdio_unregister(priv->bus);
    }

  return ret;
}
