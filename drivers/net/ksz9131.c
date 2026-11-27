/****************************************************************************
 * drivers/net/ksz9131.c
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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include <nuttx/net/gmii.h>
#include <nuttx/net/ksz9131.h>
#include <nuttx/net/mdio.h>
#include <nuttx/net/mii.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* KSZ9131 PHY ID and MMD2 RGMII registers, from Linux micrel.c and the
 * Renesas FSP KSZ9131 target.  The PHY address and delay are board data.
 */

#define KSZ9131_ID            0x00221640
#define KSZ9131_ID_MASK       0x00fffff0
#define KSZ9131_MMD           2
#define KSZ9131_CLK_SKEW      8
#define KSZ9131_CTRL_SKEW     4
#define KSZ9131_RX_SKEW       5
#define KSZ9131_TX_SKEW       6
#define KSZ9131_RX_DLL        76
#define KSZ9131_TX_DLL        77
#define KSZ9131_DLL_DISABLE   (1 << 12)
#define KSZ9131_SKEW_MASK     0x03ff

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ksz9131_mmdselect(FAR struct mdio_bus_s *bus, uint8_t addr,
                             uint16_t reg)
{
  int ret;

  ret = mdio_write(bus, addr, MII_MMDCONTROL,
                   MII_MMDCONTROL_FUNC_ADDR | KSZ9131_MMD);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_write(bus, addr, MII_MMDADDRDATA, reg);
  if (ret < 0)
    {
      return ret;
    }

  return mdio_write(bus, addr, MII_MMDCONTROL,
                    MII_MMDCONTROL_FUNC_NOINCR | KSZ9131_MMD);
}

static int ksz9131_mmdread(FAR struct mdio_bus_s *bus, uint8_t addr,
                           uint16_t reg, FAR uint16_t *value)
{
  int ret = ksz9131_mmdselect(bus, addr, reg);

  return ret < 0 ? ret : mdio_read(bus, addr, MII_MMDADDRDATA, value);
}

static int ksz9131_mmdwrite(FAR struct mdio_bus_s *bus, uint8_t addr,
                            uint16_t reg, uint16_t value)
{
  int ret = ksz9131_mmdselect(bus, addr, reg);

  return ret < 0 ? ret : mdio_write(bus, addr, MII_MMDADDRDATA, value);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ksz9131_init
 *
 * Description:
 *   Verify, reset and configure a KSZ9131 PHY and restart
 *   auto-negotiation.  See include/nuttx/net/ksz9131.h.
 *
 ****************************************************************************/

int ksz9131_init(FAR struct mdio_bus_s *bus,
                 FAR const struct ksz9131_config_s *config)
{
  uint8_t addr;
  uint16_t val;
  uint16_t id1;
  uint16_t id2;
  int ret;
  int i;

  if (bus == NULL || config == NULL || config->phyaddr > 31 ||
      (config->max_speed != 0 && config->max_speed != 10 &&
       config->max_speed != 100 && config->max_speed != 1000))
    {
      return -EINVAL;
    }

  addr = config->phyaddr;

  ret = mdio_read(bus, addr, MII_PHYID1, &id1);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_read(bus, addr, MII_PHYID2, &id2);
  if (ret < 0)
    {
      return ret;
    }

  if (((((uint32_t)id1 << 16) | id2) & KSZ9131_ID_MASK) !=
      (KSZ9131_ID & KSZ9131_ID_MASK))
    {
      return -ENODEV;
    }

  ret = mdio_write(bus, addr, MII_MCR, MII_MCR_RESET);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < 1000; i++)
    {
      usleep(1000);
      ret = mdio_read(bus, addr, MII_MCR, &val);
      if (ret < 0 || !(val & MII_MCR_RESET))
        {
          break;
        }
    }

  if (ret < 0 || i == 1000)
    {
      return ret < 0 ? ret : -ETIMEDOUT;
    }

  /* Linux micrel.c maps skew picoseconds to the 5-bit field by adding
   * 700 ps and dividing by 100.  The board supplies the desired values.
   */

  if (config->rxc_skew_ps > 2400 || config->txc_skew_ps > 2400 ||
      (config->rxc_skew_ps % 100) != 0 || (config->txc_skew_ps % 100) != 0)
    {
      return -EINVAL;
    }

  ret = ksz9131_mmdread(bus, addr, KSZ9131_CLK_SKEW, &val);
  if (ret < 0)
    {
      return ret;
    }

  ret = ksz9131_mmdwrite(bus, addr, KSZ9131_CLK_SKEW,
                         (val & ~KSZ9131_SKEW_MASK) |
                         (((config->txc_skew_ps + 700) / 100) << 5) |
                         ((config->rxc_skew_ps + 700) / 100));
  if (ret < 0)
    {
      return ret;
    }

  if (config->zero_data_skew)
    {
      /* 0 ps is encoded as 7.  The RDK and EVK DTS files supply zero for
       * RXDV and all eight data pads.  Their "txdv" property is not the
       * Linux driver's "txen" property, so preserve TXEN's reset setting.
       */

      ret = ksz9131_mmdread(bus, addr, KSZ9131_CTRL_SKEW, &val);
      if (ret < 0)
        {
          return ret;
        }

      ret = ksz9131_mmdwrite(bus, addr, KSZ9131_CTRL_SKEW,
                             (val & ~0xf0u) | 0x70u);
      if (ret < 0)
        {
          return ret;
        }

      ret = ksz9131_mmdwrite(bus, addr, KSZ9131_RX_SKEW, 0x7777);
      if (ret < 0)
        {
          return ret;
        }

      ret = ksz9131_mmdwrite(bus, addr, KSZ9131_TX_SKEW, 0x7777);
      if (ret < 0)
        {
          return ret;
        }
    }

  for (i = KSZ9131_RX_DLL; i <= KSZ9131_TX_DLL; i++)
    {
      ret = ksz9131_mmdread(bus, addr, i, &val);
      if (ret < 0)
        {
          return ret;
        }

      ret = ksz9131_mmdwrite(bus, addr, i,
                             val & ~KSZ9131_DLL_DISABLE);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* FSP's 10/100/1000 auto-negotiation path advertises both duplex
   * modes.  Do not advertise pause until the MAC flow-control registers
   * are configured to honor it.  max_speed trims the advertisement like
   * the Linux "max-speed" property.
   */

  val = MII_ADVERTISE_CSMA |
        MII_ADVERTISE_10BASETXHALF |
        MII_ADVERTISE_10BASETXFULL;
  if (config->max_speed != 10)
    {
      val |= MII_ADVERTISE_100BASETXHALF |
             MII_ADVERTISE_100BASETXFULL;
    }

  ret = mdio_write(bus, addr, MII_ADVERTISE, val);
  if (ret < 0)
    {
      return ret;
    }

  val = 0;
  if (config->max_speed == 0 || config->max_speed == 1000)
    {
      val = GMII_1000BTCR_1000BASETHALF |
            GMII_1000BTCR_1000BASETFULL;
    }

  ret = mdio_write(bus, addr, GMII_1000BTCR, val);
  if (ret < 0)
    {
      return ret;
    }

  return mdio_write(bus, addr, MII_MCR,
                    MII_MCR_ANENABLE | MII_MCR_ANRESTART);
}

/****************************************************************************
 * Name: ksz9131_link_status
 *
 * Description:
 *   Report the speed and duplex resolved by auto-negotiation.  See
 *   include/nuttx/net/ksz9131.h.
 *
 ****************************************************************************/

int ksz9131_link_status(FAR struct mdio_bus_s *bus, uint8_t phyaddr,
                        FAR unsigned int *speed, FAR bool *full)
{
  uint16_t bmsr;
  uint16_t adv;
  uint16_t lpa;
  uint16_t ctrl1000;
  uint16_t stat1000;
  uint16_t common;
  int ret;

  /* Link status is latched low, so read twice. */

  ret = mdio_read(bus, phyaddr, MII_MSR, &bmsr);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_read(bus, phyaddr, MII_MSR, &bmsr);
  if (ret < 0 || !(bmsr & MII_MSR_LINKSTATUS) ||
      !(bmsr & MII_MSR_ANEGCOMPLETE))
    {
      return ret < 0 ? ret : -ENETDOWN;
    }

  ret = mdio_read(bus, phyaddr, MII_ADVERTISE, &adv);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_read(bus, phyaddr, MII_LPA, &lpa);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_read(bus, phyaddr, GMII_1000BTCR, &ctrl1000);
  if (ret < 0)
    {
      return ret;
    }

  ret = mdio_read(bus, phyaddr, GMII_1000BTSR, &stat1000);
  if (ret < 0)
    {
      return ret;
    }

  if ((ctrl1000 & GMII_1000BTCR_1000BASETFULL) &&
      (stat1000 & GMII_1000BTSR_LP1000BASETFULL))
    {
      *speed = 1000;
      *full = true;
    }
  else if ((ctrl1000 & GMII_1000BTCR_1000BASETHALF) &&
           (stat1000 & GMII_1000BTSR_LP1000BASETHALF))
    {
      *speed = 1000;
      *full = false;
    }
  else
    {
      common = adv & lpa;
      if (common & MII_ADVERTISE_100BASETXFULL)
        {
          *speed = 100;
          *full = true;
        }
      else if (common & MII_ADVERTISE_100BASETXHALF)
        {
          *speed = 100;
          *full = false;
        }
      else if (common & MII_ADVERTISE_10BASETXFULL)
        {
          *speed = 10;
          *full = true;
        }
      else if (common & MII_ADVERTISE_10BASETXHALF)
        {
          *speed = 10;
          *full = false;
        }
      else
        {
          return -ENETDOWN;
        }
    }

  return 0;
}
