/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_gbeth.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RZV2H_RZV2H_GBETH_H
#define __ARCH_ARM_SRC_RZV2H_RZV2H_GBETH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>
#include <nuttx/net/ksz9131.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board-specific GBETH configuration passed to rzv2h_gbeth_initialize() */

struct rzv2h_gbeth_config_s
{
  uintptr_t base;                       /* GBETH register base address */
  int irq;                              /* GIC IRQ of sbd_intr_o (level) */
  uint8_t mdio_csr;                     /* MDIO_Address.CR clock range */
  struct ksz9131_config_s phy;          /* PHY address, skews, max speed */

  /* Set the board TX clock for 10, 100 or 1000 Mbps.  Called on every
   * link-up; returns zero (OK) or a negated errno value.
   */

  int (*set_speed)(unsigned int speed);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rzv2h_gbeth_initialize
 *
 * Description:
 *   Register the GBETH0 MDIO bus and the 'eth0' network device and attach
 *   the interrupt handler.  The controller is started later, at ifup.
 *
 * Input Parameters:
 *   config - Board configuration.  Must remain valid for the life of the
 *            driver.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 * Assumptions:
 *   Called once from board bring-up, after the GBETH0 clocks and reset
 *   have been released and its interrupt has been routed.
 *
 ****************************************************************************/

int rzv2h_gbeth_initialize(
  const struct rzv2h_gbeth_config_s *config);

#endif
