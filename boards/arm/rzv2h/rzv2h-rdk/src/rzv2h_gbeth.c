/****************************************************************************
 * boards/arm/rzv2h/rzv2h-rdk/src/rzv2h_gbeth.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>
#include <stdint.h>

#include <nuttx/arch.h>

#include "bsp_api.h"
#include "rzv2h_gbeth.h"
#include "rzv2h_irq.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* GBETH0's six CPG gates and reset are from r9a09g057-cpg.c.  The SYS
 * register and PHY address/skew are from the RDK Linux device tree.
 * SEL numbers are NuttX software allocations, not fixed SoC IRQ IDs.
 */

/****************************************************************************
 * Name: rdk_gbeth_speed
 *
 * Description:
 *   set_speed callback for the GBETH driver.  Set the PLLETH_GBE0 divider
 *   that generates the RGMII TX clock (/2, /10 or /100 for 1000, 100 or
 *   10 Mbps) and print the clock and pin configuration (bring-up
 *   diagnostic).
 *
 * Input Parameters:
 *   speed - Link speed in Mbps: 10, 100 or 1000
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL for an unsupported speed, -EIO if the
 *   BSP rejects the divider.
 *
 * Assumptions:
 *   Called from gbeth_linkwork() on the LPWORK thread.
 *
 ****************************************************************************/

static int rdk_gbeth_speed(unsigned int speed)
{
  uint32_t divider;

  if (speed == 1000)
    {
      divider = BSP_CLOCKS_PLLETH_GBE0_DIV_2;
    }
  else if (speed == 100)
    {
      divider = BSP_CLOCKS_PLLETH_GBE0_DIV_10;
    }
  else if (speed == 10)
    {
      divider = BSP_CLOCKS_PLLETH_GBE0_DIV_100;
    }
  else
    {
      return -EINVAL;
    }

  if (R_BSP_ClockDividerSet(FSP_PRIV_CLOCK_DIVIDER_PLLETH_GBE0,
                            divider) != FSP_SUCCESS)
    {
      return -EIO;
    }

  return 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rzv2h_gbeth_config_s g_rdk_gbeth =
{
  .base = 0x15c30000,
  .irq = RZV2H_FSP_TO_GIC_IRQ(CONFIG_RZV2H_GBETH_INTSEL),
  .mdio_csr = 4,
  .set_speed = rdk_gbeth_speed,
  .phy =
    {
      .phyaddr = 0,
      .rxc_skew_ps = 1400,
      .txc_skew_ps = 1400,
      .zero_data_skew = true,
      .max_speed = CONFIG_RZV2H_GBETH_MAX_SPEED
    }
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_gbeth_initialize
 *
 * Description:
 *   Bring up GBETH0 on the RZ/V2H RDK: enable the TXC output pin, start
 *   PLLETH if it is stopped, select RGMII mode, release the GBETH0 reset,
 *   enable its clock gates, release the bus module stop, route the
 *   sbd_intr_o event to INTSEL slot CONFIG_RZV2H_GBETH_INTSEL as a
 *   level interrupt, and register the driver with
 *   rzv2h_gbeth_initialize().  Works when CR8_0 boots without the CA55.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if PLLETH, the reset or the clock
 *   monitors do not reach the expected state; or the error from the
 *   interrupt routing or rzv2h_gbeth_initialize().
 *
 * Assumptions:
 *   Called once from rzv2h_bringup().
 *
 ****************************************************************************/

int board_gbeth_initialize(void)
{
  const uint32_t gates = (0x3fu << 8); /* CLK8..CLK13 */
  const uint32_t plleth_ready =
    R_CPG_CPG_PLLETH_MON_PLLn_RESETB_Msk |
    R_CPG_CPG_PLLETH_MON_PLLn_LOCK_Msk;
  unsigned int i;
  uint32_t value;
  int ret;

  /* Enable ET0_TXC_TXCLK output like the EVK pinctrl node.  OEN0 is
   * active-low, so clear it before starting the MAC.  The BSP helpers
   * handle the PWPR write protection around the OEN register.
   */

  R_BSP_OENAccessEnable();
  R_GPIO->PFC_OEN &= ~R_GPIO_PFC_OEN_OEN0_Msk;
  R_BSP_OENAccessDisable();

  /* The BSP configures the Ethernet PLL divider and TX selector, but it
   * restores PLLETH to its previous stopped state afterward.  GBETH0's
   * internal TX clocks require the PLL to be running.
   */

  if ((R_CPG->CPG_PLLETH_MON & plleth_ready) != plleth_ready)
    {
      R_CPG->CPG_PLLETH_STBY =
        R_CPG_CPG_PLLETH_STBY_RESETB_WEN_Msk |
        R_CPG_CPG_PLLETH_STBY_RESETB_Msk;

      for (i = 0; i < 1000; i++)
        {
          if ((R_CPG->CPG_PLLETH_MON & plleth_ready) == plleth_ready)
            {
              break;
            }

          up_udelay(10);
        }

      if (i == 1000)
        {
          syslog(LOG_ERR, "GBETH0 PLLETH lock timeout: MON=%08x\n",
                 (unsigned int)R_CPG->CPG_PLLETH_MON);
          return -ETIMEDOUT;
        }
    }

  /* Preserve the read-only MAC speed field; RGMII is encoding 1. */

  value = R_SYSC->SYS_GBETH0_CFG & 3u;
  R_SYSC->SYS_GBETH0_CFG = value | (1u << 16);

  R_CPG->CPG_RST_11 = R_CPG_CPG_RST_11_RSTB0_WEN_Msk |
                      R_CPG_CPG_RST_11_RSTB0_Msk;
  for (i = 0; i < 100000; i++)
    {
      if (!(R_CPG->CPG_RSTMON_5 &
            R_CPG_CPG_RSTMON_5_RST1_MON_Msk))
        {
          break;
        }
    }

  if (i == 100000)
    {
      syslog(LOG_ERR, "GBETH0 reset release timeout: RSTMON_5=%08x "
             "RST_11=%08x\n",
             (unsigned int)R_CPG->CPG_RSTMON_5,
             (unsigned int)R_CPG->CPG_RST_11);
      return -ETIMEDOUT;
    }

  R_CPG->CPG_CLKON_11 = gates | (gates << 16);

  /* All six GBETH0 clocks share bus stop 8.5.  The CPG clock enable
   * path releases this stop before checking the clock monitors.
   */

  R_CPG->CPG_BUS_8_MSTOP =
    R_CPG_CPG_BUS_8_MSTOP_MSTOP5_ON_WEN_Msk;

  for (i = 0; i < 100000; i++)
    {
      if ((R_CPG->CPG_CLKMON_5 & (0x3fu << 24)) == (0x3fu << 24))
        {
          break;
        }
    }

  if (i == 100000)
    {
      syslog(LOG_ERR, "GBETH0 clock timeout: CLKMON_5=%08x "
             "CLKON_11=%08x BUS_8_MSTOP=%08x\n",
             (unsigned int)R_CPG->CPG_CLKMON_5,
             (unsigned int)R_CPG->CPG_CLKON_11,
             (unsigned int)R_CPG->CPG_BUS_8_MSTOP);
      syslog(LOG_ERR, "GBETH0 TX source: PLLETH_MON=%08x "
             "SSEL0=%08x CSDIV0=%08x OEN=%08x\n",
             (unsigned int)R_CPG->CPG_PLLETH_MON,
             (unsigned int)R_CPG->CPG_SSEL0,
             (unsigned int)R_CPG->CPG_CSDIV0,
             (unsigned int)R_GPIO->PFC_OEN);
      return -ETIMEDOUT;
    }

  /* Like Linux rz-6.1 (single "macirq"), use the sbd_intr_o summary
   * interrupt.  It is a level signal while DMA_CH0_Status NIS/AIS is set.
   */

  ret = rzv2h_intsel_connect_event(CONFIG_RZV2H_GBETH_INTSEL,
                                    RZV2H_IRQSEL_GBETH0_SBD,
                                    BSP_GIC_SPI_DETECT_LEVEL);
  if (ret < 0)
    {
      return ret;
    }

  /* This direct driver does not call R_BSP_IrqCfg(), unlike FSP drivers.
   * Program the GIC trigger mode explicitly for the level IRQ.
   */

  ret = up_set_irq_type(g_rdk_gbeth.irq, IRQ_HIGH_LEVEL);
  if (ret >= 0)
    {
      ret = rzv2h_gbeth_initialize(&g_rdk_gbeth);
    }

  if (ret < 0)
    {
      rzv2h_intsel_disconnect_event(CONFIG_RZV2H_GBETH_INTSEL);
    }

  return ret;
}
