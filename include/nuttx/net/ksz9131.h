/****************************************************************************
 * include/nuttx/net/ksz9131.h
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

#ifndef __INCLUDE_NUTTX_NET_KSZ9131_H
#define __INCLUDE_NUTTX_NET_KSZ9131_H

#ifdef CONFIG_NET_KSZ9131

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/net/mdio.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board-specific KSZ9131 configuration.  These values correspond to the
 * Linux device-tree PHY node properties (reg, rxc-skew-psec,
 * txc-skew-psec and the per-pad data skews).
 */

struct ksz9131_config_s
{
  uint8_t  phyaddr;        /* PHY address on the MDIO bus (0-31) */
  uint16_t rxc_skew_ps;    /* RXC pad skew, 0-2400 ps in 100 ps steps */
  uint16_t txc_skew_ps;    /* TXC pad skew, 0-2400 ps in 100 ps steps */
  bool     zero_data_skew; /* Program 0 ps for RXDV, RXD0-3 and TXD0-3 */
  uint16_t max_speed;      /* Highest advertised speed in Mbps: 10, 100
                            * or 1000.  Zero means 1000 (no limit), like
                            * an absent Linux "max-speed" property.
                            */
};

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ksz9131_init
 *
 * Description:
 *   Verify the PHY ID, soft-reset the PHY, apply the RGMII clock and pad
 *   skews, enable the internal RXC/TXC delay DLLs and restart
 *   auto-negotiation.  Called by the Ethernet MAC driver, typically from
 *   its ifup handler, after its MDIO bus has been registered.
 *
 * Input Parameters:
 *   bus    - MDIO bus handle returned by mdio_register()
 *   config - Board-specific PHY configuration
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *   -ENODEV if no KSZ9131 answers at the configured address, -EINVAL for
 *   unsupported skew or max_speed values, -ETIMEDOUT if the soft reset
 *   does not complete.
 *
 ****************************************************************************/

int ksz9131_init(FAR struct mdio_bus_s *bus,
                 FAR const struct ksz9131_config_s *config);

/****************************************************************************
 * Name: ksz9131_link_status
 *
 * Description:
 *   Report the link resolved by auto-negotiation.  The MAC driver uses the
 *   result to program its own speed and duplex settings.
 *
 * Input Parameters:
 *   bus     - MDIO bus handle returned by mdio_register()
 *   phyaddr - PHY address on the MDIO bus
 *   speed   - Location to return the speed in Mbps (10, 100 or 1000)
 *   full    - Location to return true for full duplex
 *
 * Returned Value:
 *   Zero (OK) when the link is up; -ENETDOWN when the link is down or
 *   auto-negotiation has not completed; another negated errno value on
 *   MDIO failure.
 *
 ****************************************************************************/

int ksz9131_link_status(FAR struct mdio_bus_s *bus, uint8_t phyaddr,
                        FAR unsigned int *speed, FAR bool *full);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_NET_KSZ9131 */
#endif /* __INCLUDE_NUTTX_NET_KSZ9131_H */
