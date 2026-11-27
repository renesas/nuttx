/****************************************************************************
 * arch/arm/src/rzv2h/hardware/rzv2h_sdhi.h
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

#ifndef __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_SDHI_H
#define __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_SDHI_H

/* RZ/V2H SDHI Hardware
 *
 * The register table permits 16, 32 and 64-bit accesses.
 * SD_BUF0 additionally requires the width selected in HOST_MODE
 * (64 bits for internal DMA). ARG1 and the odd response registers are
 * mirrors for narrow accesses, not upper halves at offset +2.
 * Unnamed/reserved bits must be zero except the explicit
 * *_RESERVED_ONE masks below. These masks are write requirements, not
 * functional fields or read/modify/write permission.
 */

/* Register Offsets
 * SDHI_BASE(n) selects channel 0, 1 or 2 in the ACPU/RCPU address map.
 */

#define RZV2H_SDHI_BASE(n)          (0x15c00000ul + (n) * 0x10000ul)
#define RZV2H_SDHI_SD_CMD           0x0000
#define RZV2H_SDHI_SD_ARG           0x0010
#define RZV2H_SDHI_SD_ARG1          0x0018
#define RZV2H_SDHI_SD_STOP          0x0020
#define RZV2H_SDHI_SD_SECCNT        0x0028
#define RZV2H_SDHI_SD_RSP10         0x0030
#define RZV2H_SDHI_SD_RSP1          0x0038
#define RZV2H_SDHI_SD_RSP32         0x0040
#define RZV2H_SDHI_SD_RSP3          0x0048
#define RZV2H_SDHI_SD_RSP54         0x0050
#define RZV2H_SDHI_SD_RSP5          0x0058
#define RZV2H_SDHI_SD_RSP76         0x0060
#define RZV2H_SDHI_SD_RSP7          0x0068
#define RZV2H_SDHI_SD_INFO1         0x0070
#define RZV2H_SDHI_SD_INFO2         0x0078
#define RZV2H_SDHI_SD_INFO1_MASK    0x0080
#define RZV2H_SDHI_SD_INFO2_MASK    0x0088
#define RZV2H_SDHI_SD_CLK_CTRL      0x0090
#define RZV2H_SDHI_SD_SIZE          0x0098
#define RZV2H_SDHI_SD_OPTION        0x00a0
#define RZV2H_SDHI_SD_ERR_STS1      0x00b0
#define RZV2H_SDHI_SD_ERR_STS2      0x00b8
#define RZV2H_SDHI_SD_BUF0          0x00c0
#define RZV2H_SDHI_SDIO_MODE        0x00d0
#define RZV2H_SDHI_SDIO_INFO1       0x00d8
#define RZV2H_SDHI_SDIO_INFO1_MASK  0x00e0
#define RZV2H_SDHI_CC_EXT_MODE      0x0360

/* Internal DMAC: contiguous low 32-bit register accesses, not SD pairs. */

#define RZV2H_SDHI_DM_CM_DTRAN_MODE  0x0820
#define RZV2H_SDHI_DM_CM_DTRAN_CTRL  0x0828
#define RZV2H_SDHI_DM_CM_RST         0x0830
#define RZV2H_SDHI_DM_CM_INFO1       0x0840
#define RZV2H_SDHI_DM_CM_INFO1_MASK  0x0848
#define RZV2H_SDHI_DM_CM_INFO2       0x0850
#define RZV2H_SDHI_DM_CM_INFO2_MASK  0x0858
#define RZV2H_SDHI_DM_DTRAN_ADDR     0x0880

#define SDHI_DM_MODE_64BIT          (3u << 4)
#define SDHI_DM_MODE_RX             (1u << 16)
#define SDHI_DM_START               (1u << 0)
#define SDHI_DM_RST_CHANNELS        ((1u << 8) | (1u << 9))
#define SDHI_DM_END_TX              (1u << 16)
#define SDHI_DM_END_RX              (1u << 20)
#define SDHI_DM_ERROR_TX            (1u << 16)
#define SDHI_DM_ERROR_RX            (1u << 17)
#define RZV2H_SDHI_SOFT_RST         0x0380
#define RZV2H_SDHI_VERSION          0x0388
#define RZV2H_SDHI_HOST_MODE        0x0390
#define RZV2H_SDHI_SDIF_MODE        0x0398
#define RZV2H_SDHI_SD_STATUS        0x03c8

/* SD_CMD. Writing starts the command. MD[2:0]=0
 * uses automatic response/transfer selection and ignores bits 15:11.
 * Extended mode uses an explicit response type.  C[1:0]=2/3, MD[2:0]=1/2
 * and MD[7:6]=2/3 are prohibited.  Encoders mask fields; they do not
 * validate values.
 */

#define SDHI_SD_CMD_INDEX_SHIFT 0
#define SDHI_SD_CMD_INDEX_MASK  (0x3fu << SDHI_SD_CMD_INDEX_SHIFT)
#define SDHI_SD_CMD_INDEX(n)    (((n) & 0x3fu) << SDHI_SD_CMD_INDEX_SHIFT)
#define SDHI_SD_CMD_C_SHIFT     6
#define SDHI_SD_CMD_C_MASK      (0x3u << SDHI_SD_CMD_C_SHIFT)
#define SDHI_SD_CMD_C(n)        (((n) & 0x3u) << SDHI_SD_CMD_C_SHIFT)
#define SDHI_SD_CMD_C_CMD       (0u << SDHI_SD_CMD_C_SHIFT)
#define SDHI_SD_CMD_C_ACMD      (1u << SDHI_SD_CMD_C_SHIFT)
#define SDHI_SD_CMD_MD_SHIFT    8
#define SDHI_SD_CMD_MD_MASK     (0x7u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD(n)       (((n) & 0x7u) << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_NORMAL   (0u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_NORESP   (3u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_R1       (4u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_R1B      (5u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_R2       (6u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_R3       (7u << SDHI_SD_CMD_MD_SHIFT)
#define SDHI_SD_CMD_MD_R4       SDHI_SD_CMD_MD_R3
#define SDHI_SD_CMD_MD_R5       SDHI_SD_CMD_MD_R1
#define SDHI_SD_CMD_MD_R6       SDHI_SD_CMD_MD_R1
#define SDHI_SD_CMD_MD_R7       SDHI_SD_CMD_MD_R1

/* Data present */

#define SDHI_SD_CMD_MD3 (1u << 11)

/* Read; zero selects write */

#define SDHI_SD_CMD_MD4 (1u << 12)

/* Multiple blocks */

#define SDHI_SD_CMD_MD5          (1u << 13)
#define SDHI_SD_CMD_STOP_SHIFT   14
#define SDHI_SD_CMD_STOP_MASK    (0x3u << SDHI_SD_CMD_STOP_SHIFT)
#define SDHI_SD_CMD_STOP(n)      (((n) & 0x3u) << SDHI_SD_CMD_STOP_SHIFT)
#define SDHI_SD_CMD_STOP_AUTO    (0u << SDHI_SD_CMD_STOP_SHIFT)
#define SDHI_SD_CMD_STOP_NO_AUTO (1u << SDHI_SD_CMD_STOP_SHIFT)
#define SDHI_SD_CMD_RESET        0x00000000u

/* SD_ARG / SD_ARG1.  ARG1[15:0] mirrors ARG[31:16].
 * Set the argument before SD_CMD.
 */

#define SDHI_SD_ARG_CF_SHIFT  0
#define SDHI_SD_ARG_CF_MASK   (0xffffffffu << SDHI_SD_ARG_CF_SHIFT)
#define SDHI_SD_ARG_CF(n)     (((n) & 0xffffffffu) << SDHI_SD_ARG_CF_SHIFT)
#define SDHI_SD_ARG1_CF_SHIFT 0
#define SDHI_SD_ARG1_CF_MASK  (0xffffu << SDHI_SD_ARG1_CF_SHIFT)
#define SDHI_SD_ARG1_CF(n)    (((n) & 0xffffu) << SDHI_SD_ARG1_CF_SHIFT)
#define SDHI_SD_ARG_RESET     0x00000000u
#define SDHI_SD_ARG1_RESET    0x00000000u

/* SD_STOP / SD_SECCNT. Do not change SEC or CNT
 * while CBSY=1. Set STP after response end; HPICMD is a self-clearing
 * request, writable as 1 only while CBSY=1. CNT is a 32-bit block count;
 * zero is prohibited for a multiple-block transfer.
 */

#define SDHI_SD_STOP_STP         (1u << 0)
#define SDHI_SD_STOP_SEC         (1u << 8)
#define SDHI_SD_STOP_HPICMD      (1u << 16)
#define SDHI_SD_STOP_HPIMODE     (1u << 17)
#define SDHI_SD_STOP_RESET       0x00000000u
#define SDHI_SD_SECCNT_CNT_SHIFT 0
#define SDHI_SD_SECCNT_CNT_MASK  (0xffffffffu << SDHI_SD_SECCNT_CNT_SHIFT)
#define SDHI_SD_SECCNT_CNT(n) \
  (((n) & 0xffffffffu) << SDHI_SD_SECCNT_CNT_SHIFT)
#define SDHI_SD_SECCNT_RESET     0x00000000u

/* Response windows (read-only).
 */

#define SDHI_SD_RSP10_DATA_SHIFT 0
#define SDHI_SD_RSP10_DATA_MASK  0xffffffffffffffffull
#define SDHI_SD_RSP10_RESET      0
#define SDHI_SD_RSP1_DATA_SHIFT  0
#define SDHI_SD_RSP1_DATA_MASK   0x0000ffffu
#define SDHI_SD_RSP1_RESET       0
#define SDHI_SD_RSP32_DATA_SHIFT 0
#define SDHI_SD_RSP32_DATA_MASK  0xffffffffu
#define SDHI_SD_RSP32_RESET      0
#define SDHI_SD_RSP3_DATA_SHIFT  0
#define SDHI_SD_RSP3_DATA_MASK   0x0000ffffu
#define SDHI_SD_RSP3_RESET       0
#define SDHI_SD_RSP54_DATA_SHIFT 0
#define SDHI_SD_RSP54_DATA_MASK  0x00ffffffffffffffull
#define SDHI_SD_RSP54_RESET      0
#define SDHI_SD_RSP5_DATA_SHIFT  0
#define SDHI_SD_RSP5_DATA_MASK   0x0000ffffu
#define SDHI_SD_RSP5_RESET       0
#define SDHI_SD_RSP76_DATA_SHIFT 0
#define SDHI_SD_RSP76_DATA_MASK  0x00ffffffu
#define SDHI_SD_RSP76_RESET      0
#define SDHI_SD_RSP7_DATA_SHIFT  0
#define SDHI_SD_RSP7_DATA_MASK   0x000000ffu
#define SDHI_SD_RSP7_RESET       0

/* SD_INFO1. Event flags are write-zero-to-clear (W0C);
 * preserve other events with ones and keep reserved bits zero.
 * INFO5/7/10 are read-only pin states, not interrupt events.
 * INFO2 is access end and INFO3 is card removal.
 * INFO5=1 means card present (inverted ISDCD); INFO7 is inverted ISDWP.
 */

/* Response end */

#define SDHI_SD_INFO1_INFO0 (1u << 0)
#define SDHI_SD_INFO1_INFO2 (1u << 2)
#define SDHI_SD_INFO1_INFO3 (1u << 3)

/* CD insertion */

#define SDHI_SD_INFO1_INFO4 (1u << 4)
#define SDHI_SD_INFO1_INFO5 (1u << 5)
#define SDHI_SD_INFO1_INFO7 (1u << 7)

/* DAT3 removal */

#define SDHI_SD_INFO1_INFO8 (1u << 8)

/* DAT3 insertion */

#define SDHI_SD_INFO1_INFO9 (1u << 9)

/* DAT3 state */

#define SDHI_SD_INFO1_INFO10 (1u << 10)

/* HPI response end */

#define SDHI_SD_INFO1_HPIRES     (1u << 16)
#define SDHI_SD_INFO1_W0C_MASK   0x0001031du
#define SDHI_SD_INFO1_STATE_MASK 0x000004a0u

/* SD_INFO2. Errors, BRE/BWE and ILA are W0C.
 * DAT0, SCLKDIVEN and CBSY are read-only. Clear BRE/BWE before servicing
 * SD_SIZE bytes through the FIFO.
 */

/* Command index error */

#define SDHI_SD_INFO2_ERR0 (1u << 0)

/* CRC error */

#define SDHI_SD_INFO2_ERR1 (1u << 1)

/* End-bit error */

#define SDHI_SD_INFO2_ERR2 (1u << 2)

/* Data timeout */

#define SDHI_SD_INFO2_ERR3 (1u << 3)

/* Illegal buffer write */

#define SDHI_SD_INFO2_ERR4 (1u << 4)

/* Illegal buffer read */

#define SDHI_SD_INFO2_ERR5 (1u << 5)

/* Response timeout */

#define SDHI_SD_INFO2_ERR6         (1u << 6)
#define SDHI_SD_INFO2_DAT0         (1u << 7)
#define SDHI_SD_INFO2_BRE          (1u << 8)
#define SDHI_SD_INFO2_BWE          (1u << 9)
#define SDHI_SD_INFO2_SCLKDIVEN    (1u << 13)
#define SDHI_SD_INFO2_CBSY         (1u << 14)
#define SDHI_SD_INFO2_ILA          (1u << 15)
#define SDHI_SD_INFO2_ERROR_MASK   0x0000807fu
#define SDHI_SD_INFO2_W0C_MASK     0x0000837fu
#define SDHI_SD_INFO2_STATE_MASK   0x00006080u
#define SDHI_SD_INFO2_RESERVED_ONE 0x00000800u

/* SD_INFO1_MASK / SD_INFO2_MASK.
 * One masks an interrupt; zero enables it.
 */

#define SDHI_SD_INFO1_MASK_IMASK0       (1u << 0)
#define SDHI_SD_INFO1_MASK_IMASK2       (1u << 2)
#define SDHI_SD_INFO1_MASK_IMASK3       (1u << 3)
#define SDHI_SD_INFO1_MASK_IMASK4       (1u << 4)
#define SDHI_SD_INFO1_MASK_IMASK8       (1u << 8)
#define SDHI_SD_INFO1_MASK_IMASK9       (1u << 9)
#define SDHI_SD_INFO1_MASK_IMASK16      (1u << 16)
#define SDHI_SD_INFO1_MASK_ALL          0x0001031du
#define SDHI_SD_INFO1_MASK_RESET        SDHI_SD_INFO1_MASK_ALL
#define SDHI_SD_INFO2_MASK_EMASK0       (1u << 0)
#define SDHI_SD_INFO2_MASK_EMASK1       (1u << 1)
#define SDHI_SD_INFO2_MASK_EMASK2       (1u << 2)
#define SDHI_SD_INFO2_MASK_EMASK3       (1u << 3)
#define SDHI_SD_INFO2_MASK_EMASK4       (1u << 4)
#define SDHI_SD_INFO2_MASK_EMASK5       (1u << 5)
#define SDHI_SD_INFO2_MASK_EMASK6       (1u << 6)
#define SDHI_SD_INFO2_MASK_BMASK0       (1u << 8)
#define SDHI_SD_INFO2_MASK_BMASK1       (1u << 9)
#define SDHI_SD_INFO2_MASK_IMASK        (1u << 15)
#define SDHI_SD_INFO2_MASK_RESERVED_ONE 0x00000800u
#define SDHI_SD_INFO2_MASK_ALL          0x00008b7fu
#define SDHI_SD_INFO2_MASK_RESET        SDHI_SD_INFO2_MASK_ALL

/* SD_CLK_CTRL. The manual specifies DIV1 for SD0/1;
 * DIV1 is prohibited in DDR mode.
 */

#define SDHI_SD_CLK_CTRL_MAX_DIVIDER   512
#define SDHI_SD_CLK_CTRL_DIV_SHIFT     0
#define SDHI_SD_CLK_CTRL_DIV_MASK      (0xffu << SDHI_SD_CLK_CTRL_DIV_SHIFT)
#define SDHI_SD_CLK_CTRL_DIV(n) \
  (((n) & 0xffu) << SDHI_SD_CLK_CTRL_DIV_SHIFT)
#define SDHI_SD_CLK_CTRL_DIV1         0xffu
#define SDHI_SD_CLK_CTRL_DIV2         0x00u
#define SDHI_SD_CLK_CTRL_DIV4         0x01u
#define SDHI_SD_CLK_CTRL_DIV8         0x02u
#define SDHI_SD_CLK_CTRL_DIV16        0x04u
#define SDHI_SD_CLK_CTRL_DIV32        0x08u
#define SDHI_SD_CLK_CTRL_DIV64        0x10u
#define SDHI_SD_CLK_CTRL_DIV128       0x20u
#define SDHI_SD_CLK_CTRL_DIV256       0x40u
#define SDHI_SD_CLK_CTRL_DIV512       0x80u
#define SDHI_SD_CLK_CTRL_SCLKEN       (1u << 8)
#define SDHI_SD_CLK_CTRL_SDCLKOFFEN   (1u << 9)
#define SDHI_SD_CLK_CTRL_RESET        0x00000020u

/* SD_SIZE. LEN is bytes per block, not a block count.
 * Zero and values >512 are invalid.
 * Do not change LEN while CBSY=1.
 */

#define SDHI_SD_SIZE_LEN_SHIFT 0
#define SDHI_SD_SIZE_LEN_MASK  (0x3ffu << SDHI_SD_SIZE_LEN_SHIFT)
#define SDHI_SD_SIZE_LEN(n)    (((n) & 0x3ffu) << SDHI_SD_SIZE_LEN_SHIFT)
#define SDHI_SD_SIZE_MAX       512u
#define SDHI_SD_SIZE_RESET     0x00000200u

/* SD_OPTION.
 */

#define SDHI_SD_OPTION_CTOP_SHIFT   0
#define SDHI_SD_OPTION_CTOP_MASK    (0xfu << SDHI_SD_OPTION_CTOP_SHIFT)
#define SDHI_SD_OPTION_CTOP(n) \
  (((n) & 0xfu) << SDHI_SD_OPTION_CTOP_SHIFT)
#define SDHI_SD_OPTION_TOP_SHIFT    4
#define SDHI_SD_OPTION_TOP_MASK     (0xfu << SDHI_SD_OPTION_TOP_SHIFT)
#define SDHI_SD_OPTION_TOP(n)       (((n) & 0xfu) << SDHI_SD_OPTION_TOP_SHIFT)
#define SDHI_SD_OPTION_TOUTMASK     (1u << 8)
#define SDHI_SD_OPTION_EXTOP        (1u << 9)
#define SDHI_SD_OPTION_WIDTH8       (1u << 13)
#define SDHI_SD_OPTION_WIDTH        (1u << 15)
#define SDHI_SD_OPTION_RESERVED_ONE 0x00004000u
#define SDHI_SD_OPTION_WIDTH_MASK   0x0000a000u
#define SDHI_SD_OPTION_WIDTH_1BIT   SDHI_SD_OPTION_WIDTH
#define SDHI_SD_OPTION_WIDTH_4BIT   0u
#define SDHI_SD_OPTION_WIDTH_8BIT   SDHI_SD_OPTION_WIDTH8
#define SDHI_SD_OPTION_RESET        0x000040eeu

/* SD_ERR_STS1 / SD_ERR_STS2 (read-only).
 */

/* Command index */

#define SDHI_SD_ERR_STS1_E0 (1u << 0)

/* Sequence command index */

#define SDHI_SD_ERR_STS1_E1 (1u << 1)

/* Response end bit */

#define SDHI_SD_ERR_STS1_E2 (1u << 2)

/* Sequence response end bit */

#define SDHI_SD_ERR_STS1_E3 (1u << 3)

/* Read data end bit */

#define SDHI_SD_ERR_STS1_E4 (1u << 4)

/* CRC status length */

#define SDHI_SD_ERR_STS1_E5 (1u << 5)

/* Response CRC */

#define SDHI_SD_ERR_STS1_E8 (1u << 8)

/* Sequence response CRC */

#define SDHI_SD_ERR_STS1_E9 (1u << 9)

/* Read data CRC */

#define SDHI_SD_ERR_STS1_E10 (1u << 10)

/* CRC status error */

#define SDHI_SD_ERR_STS1_E11              (1u << 11)
#define SDHI_SD_ERR_STS1_CRC_STATUS_SHIFT 12
#define SDHI_SD_ERR_STS1_CRC_STATUS_MASK \
  (0x7u << SDHI_SD_ERR_STS1_CRC_STATUS_SHIFT)
#define SDHI_SD_ERR_STS1_CRC_STATUS(n) \
  (((n) & 0x7u) << SDHI_SD_ERR_STS1_CRC_STATUS_SHIFT)
#define SDHI_SD_ERR_STS1_CRC_STATUS_OK \
  (2u << SDHI_SD_ERR_STS1_CRC_STATUS_SHIFT)

/* Response timeout */

#define SDHI_SD_ERR_STS2_E0 (1u << 0)

/* Sequence response timeout */

#define SDHI_SD_ERR_STS2_E1 (1u << 1)

/* R1b busy timeout */

#define SDHI_SD_ERR_STS2_E2 (1u << 2)

/* Sequence CMD12 busy timeout */

#define SDHI_SD_ERR_STS2_E3 (1u << 3)

/* Read data timeout */

#define SDHI_SD_ERR_STS2_E4 (1u << 4)

/* CRC status timeout */

#define SDHI_SD_ERR_STS2_E5 (1u << 5)

/* CRC status busy timeout */

#define SDHI_SD_ERR_STS2_E6    (1u << 6)
#define SDHI_SD_ERR_STS2_RESET 0u

/* SD_BUF0. FIFO port backed by two 512-byte buffers;
 * access width must match HOST_MODE. Contents at reset are undefined.
 */

#define SDHI_SD_BUF0_DATA_SHIFT   0
#define SDHI_SD_BUF0_DATA_MASK    0xffffffffffffffffull
#define SDHI_SD_BUF0_BUFFER_SIZE  512u
#define SDHI_SD_BUF0_BUFFER_COUNT 2u

/* SDIO_MODE.
 */

#define SDHI_SDIO_MODE_IOMOD  (1u << 0)
#define SDHI_SDIO_MODE_RWREQ  (1u << 2)
#define SDHI_SDIO_MODE_IOABT  (1u << 8)
#define SDHI_SDIO_MODE_C52PUB (1u << 9)
#define SDHI_SDIO_MODE_RESET  0u

/* SDIO_INFO1 / SDIO_INFO1_MASK.
 */

#define SDHI_SDIO_INFO1_IOIRQ             (1u << 0)
#define SDHI_SDIO_INFO1_EXPUB52           (1u << 14)
#define SDHI_SDIO_INFO1_EXWT              (1u << 15)
#define SDHI_SDIO_INFO1_W0C_MASK          0x0000c001u
#define SDHI_SDIO_INFO1_RESERVED_ONE      0x00000006u
#define SDHI_SDIO_INFO1_RESET             0u
#define SDHI_SDIO_INFO1_MASK_IOMSK        (1u << 0)
#define SDHI_SDIO_INFO1_MASK_MEXPUB52     (1u << 14)
#define SDHI_SDIO_INFO1_MASK_MEXWT        (1u << 15)
#define SDHI_SDIO_INFO1_MASK_RESERVED_ONE 0x00000006u
#define SDHI_SDIO_INFO1_MASK_ALL          0x0000c007u
#define SDHI_SDIO_INFO1_MASK_RESET        SDHI_SDIO_INFO1_MASK_ALL

/* CC_EXT_MODE / SOFT_RST.
 */

#define SDHI_CC_EXT_MODE_DMASDRW      (1u << 1)
#define SDHI_CC_EXT_MODE_RESERVED_ONE 0x00001010u
#define SDHI_CC_EXT_MODE_RESET        0x00001010u
#define SDHI_SOFT_RST_SDRST           (1u << 0)
#define SDHI_SOFT_RST_RESERVED_ONE    0x00000006u
#define SDHI_SOFT_RST_ASSERT          0x00000006u
#define SDHI_SOFT_RST_RELEASE         0x00000007u
#define SDHI_SOFT_RST_RESET           0x00000007u

/* VERSION (read-only).
 */

#define SDHI_VERSION_IP_SHIFT     0
#define SDHI_VERSION_IP_MASK      (0xffu << SDHI_VERSION_IP_SHIFT)
#define SDHI_VERSION_IP(n)        (((n) & 0xffu) << SDHI_VERSION_IP_SHIFT)
#define SDHI_VERSION_UR_SHIFT     8
#define SDHI_VERSION_UR_MASK      (0xfu << SDHI_VERSION_UR_SHIFT)
#define SDHI_VERSION_UR(n)        (((n) & 0xfu) << SDHI_VERSION_UR_SHIFT)
#define SDHI_VERSION_RESERVED_ONE 0x0000c000u
#define SDHI_VERSION_RESET        0x0000cc10u

/* HOST_MODE. These widths apply to the host FIFO port,
 * not SD DAT lines. Do not change width while CBSY=1.  Internal DMA
 * requires 64-bit mode. ENDIAN controls FIFO data swapping.
 */

#define SDHI_HOST_MODE_WMODE      (1u << 0)
#define SDHI_HOST_MODE_ENDIAN     (1u << 1)
#define SDHI_HOST_MODE_BUSWIDTH   (1u << 8)
#define SDHI_HOST_MODE_WIDTH_MASK 0x00000101u
#define SDHI_HOST_MODE_64BIT      0u
#define SDHI_HOST_MODE_16BIT      SDHI_HOST_MODE_WMODE
#define SDHI_HOST_MODE_32BIT \
  (SDHI_HOST_MODE_WMODE | SDHI_HOST_MODE_BUSWIDTH)
#define SDHI_HOST_MODE_RESET      0u

/* SDIF_MODE / SD_STATUS. Change DDR only while
 * CBSY=0 and do not combine with DIV1. NOCHKCR bypasses CRC16/status
 * checks for MMC test commands. SD_RST controls SD0RSTN on SD0 only;
 * SD_IOVS selects the IOVS output (0=3.3 V, 1=1.8 V). Board wiring and
 * power/voltage sequencing must be handled before using these outputs.
 */

#define SDHI_SDIF_MODE_DDR     (1u << 0)
#define SDHI_SDIF_MODE_NOCHKCR (1u << 8)
#define SDHI_SDIF_MODE_RESET   0u
#define SDHI_SD_STATUS_SD_PWEN (1u << 0)
#define SDHI_SD_STATUS_SD_RST  (1u << 1)
#define SDHI_SD_STATUS_SD_IOVS (1u << 16)
#define SDHI_SD_STATUS_RESET   1u

/* Sampling clock control, 32-bit registers (Rev.1.40 section 6.2.7).
 * Offsets include the Gen3/RZ bus shift. Never use TMIO halfword accessors.
 */

#define RZV2H_SDHI_SCC_DTCNTL  0x1000
#define RZV2H_SDHI_SCC_TAPSET  0x1008
#define RZV2H_SDHI_SCC_DT2FF   0x1010
#define RZV2H_SDHI_SCC_CKSEL   0x1018
#define RZV2H_SDHI_SCC_RVSCNTL 0x1020
#define RZV2H_SDHI_SCC_RVSREQ  0x1028
#define RZV2H_SDHI_SCC_SMPCMP  0x1030

/* DTCNTL [23:16] = 8 for SD_CLK_CTRL.DIV = 0xff (1:1); bit 0 TAPEN
 * enables SCC sampling. The readback mask selects only these fields;
 * the initialization value writes reserved bits as zero (6.2.7.2.1).
 */

#define SDHI_SCC_DTCNTL_TAPNUM_SHIFT 16
#define SDHI_SCC_DTCNTL_TAPNUM_MASK  0xff
#define SDHI_SCC_DTCNTL_8TAPS        0x00080001
#define SDHI_SCC_DTCNTL_CONFIG_MASK  0x00ff0001

/* Rev.1.40 section 6.2.7.2.6: DT2NS=0x07 and DT2NE=0x02 for tuning.
 * Program this value with DTSEL clear before enabling sampling selection.
 * The register reset value (0x0300) is not the tuning prescription.
 */

#define SDHI_SCC_DT2FF_TUNING        0x0207

#define SDHI_SCC_TAPEN        (1u << 0)

/* CKSEL bit 0 selects the SCC clock for SDR104; change with SDCLK stopped
 * (6.2.7.2.3). TAPEN and DTSEL control different registers.
 */

#define SDHI_SCC_DTSEL        (1u << 0)
#define SDHI_SCC_RVSEN        (1u << 0)
#define SDHI_SCC_RVSERR       (1u << 2)
#define SDHI_SCC_CMD_ERROR    ((1u << 8) | (1u << 24))

/* DM_CM_DTRAN_MODE / DM_CM_DTRAN_CTRL.
 */

#define SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH_SHIFT   4
#define SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH_MASK \
  (0x3u << SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH_SHIFT)
#define SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH(n) \
  (((n) & 0x3u) << SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH_SHIFT)
#define SDHI_DM_CM_DTRAN_MODE_BUS_WIDTH_64BIT   (3u << 4)
#define SDHI_DM_CM_DTRAN_MODE_CH_NUM_SHIFT      16
#define SDHI_DM_CM_DTRAN_MODE_CH_NUM_MASK \
  (0x3u << SDHI_DM_CM_DTRAN_MODE_CH_NUM_SHIFT)
#define SDHI_DM_CM_DTRAN_MODE_CH_NUM(n) \
  (((n) & 0x3u) << SDHI_DM_CM_DTRAN_MODE_CH_NUM_SHIFT)
#define SDHI_DM_CM_DTRAN_MODE_CH_NUM_DOWNSTREAM (0u << 16)
#define SDHI_DM_CM_DTRAN_MODE_CH_NUM_UPSTREAM   (1u << 16)
#define SDHI_DM_CM_DTRAN_MODE_RESET             0x00000030u
#define SDHI_DM_CM_DTRAN_CTRL_DM_START          (1u << 0)
#define SDHI_DM_CM_DTRAN_CTRL_RESET             0u

/* DM_CM_RST. Reset bits are active low. Apply software
 * reset only when there is no communication with the card.
 */

#define SDHI_DM_CM_RST_SEQRST       (1u << 0)
#define SDHI_DM_CM_RST_DTRANRST0    (1u << 8)
#define SDHI_DM_CM_RST_DTRANRST1    (1u << 9)
#define SDHI_DM_CM_RST_RESERVED_ONE 0xfffffcfeu
#define SDHI_DM_CM_RST_RESET        0xffffffffu

/* DM_CM_INFO1/2 and masks.
 */

#define SDHI_DM_CM_INFO1_SEQEND              (1u << 0)
#define SDHI_DM_CM_INFO1_DTRANEND0           (1u << 16)
#define SDHI_DM_CM_INFO1_DTRANEND1           (1u << 20)
#define SDHI_DM_CM_INFO1_W0C_MASK            0x00110001u
#define SDHI_DM_CM_INFO1_RESET               0u
#define SDHI_DM_CM_INFO1_MASK_SEQEND_MASK    (1u << 0)
#define SDHI_DM_CM_INFO1_MASK_DTRANEND0_MASK (1u << 16)
#define SDHI_DM_CM_INFO1_MASK_DTRANEND1_MASK (1u << 20)
#define SDHI_DM_CM_INFO1_MASK_RESERVED_ONE   0xffeefffeu
#define SDHI_DM_CM_INFO1_MASK_ALL            0xffffffffu
#define SDHI_DM_CM_INFO1_MASK_RESET          0xffffffffu
#define SDHI_DM_CM_INFO2_SEQERR              (1u << 0)
#define SDHI_DM_CM_INFO2_DTRANERR0           (1u << 16)
#define SDHI_DM_CM_INFO2_DTRANERR1           (1u << 17)
#define SDHI_DM_CM_INFO2_W0C_MASK            0x00030001u
#define SDHI_DM_CM_INFO2_RESET               0u
#define SDHI_DM_CM_INFO2_MASK_SEQERR_MASK    (1u << 0)
#define SDHI_DM_CM_INFO2_MASK_DTRANERR0_MASK (1u << 16)
#define SDHI_DM_CM_INFO2_MASK_DTRANERR1_MASK (1u << 17)
#define SDHI_DM_CM_INFO2_MASK_RESERVED_ONE   0xfffcfffeu
#define SDHI_DM_CM_INFO2_MASK_ALL            0xffffffffu
#define SDHI_DM_CM_INFO2_MASK_RESET          0xffffffffu

/* DM_DTRAN_ADDR.
 */

#define SDHI_DM_DTRAN_ADDR_DADDR_SHIFT 3
#define SDHI_DM_DTRAN_ADDR_DADDR_MASK \
  (0x1fffffffu << SDHI_DM_DTRAN_ADDR_DADDR_SHIFT)
#define SDHI_DM_DTRAN_ADDR_DADDR(n) \
  (((n) & 0x1fffffffu) << SDHI_DM_DTRAN_ADDR_DADDR_SHIFT)
#define SDHI_DM_DTRAN_ADDR_ALIGNMENT   8u
#define SDHI_DM_DTRAN_ADDR_RESET       0u

#endif /* __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_SDHI_H */
