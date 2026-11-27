/****************************************************************************
 * boards/arm/rzv2h/rzv2h-rdk/scripts/rzv2h-rdk_cr8_0.ld
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

#include <nuttx/config.h>
#include <arch/board/rzv2h-rptun.h>

/*
 * RZ/V2H Cortex-R8_0 linker script for NuttX.
 *
 */

/* Core-local TCM map of the RZ/V2H CR8, from the Renesas HAL
 * [FSP: bsp_slave_address.h:19-25]:
 *
 *   ITCM  core 0x00000000, 128 KB, AXI alias 0x12040000
 *   DTCM  core 0x00020000, 128 KB, AXI alias 0x12060000
 *
 * Both build types keep a load copy of the vector table at the start of
 * RAM, which arm_boot() copies to ITCM.  CR8_0 takes its reset vector from
 * ITCM, so before releasing CR8_0 from reset U-Boot writes a fixed two-word
 * trampoline there through the AXI alias:
 *
 *   0x0: 0xe51ff004  ldr pc, [pc, #-4]
 *   0x4: 0x08180000  ORIGIN(RAM), the load copy of the table
 *
 * The reset slot of the load copy then branches to __start.  A
 * debugger-loaded image is entered at __start directly; ITCM is not mapped
 * until arm_boot() programs the ATCM region register.
 */

MEMORY
{
    ITCM (rwx)     : ORIGIN = 0x00000000, LENGTH = 128K
    DTCM (rwx)     : ORIGIN = 0x00020000, LENGTH = 128K
    RAM  (rwx)     : ORIGIN = 0x08180000, LENGTH = 512K
#ifdef CONFIG_RZV2H_RPTUN
    RSCTBL (rw)    : ORIGIN = BOARD_RPTUN_RESOURCE_ADDR,
                     LENGTH = BOARD_RPTUN_RESOURCE_SIZE
#endif
}

OUTPUT_ARCH(arm)
EXTERN(_vector_start)
ENTRY(__start)

#ifdef CONFIG_RZV2H_RPTUN
/* The generated reference MHU_B_NS instance leaves p_shared_memory NULL.
 * FSP uses this base symbol and adds eight bytes per logical MHU channel.
 */

PROVIDE(__mhu_shmem_start = BOARD_RPTUN_MHU_SHMEM_ADDR);
#endif

/* Start of the read-only SRAM part of the image.  Everything from here up to
 * __mpu_code_end (the vector load image, .text, .rodata, .init_section and
 * .ARM.exidx) is covered by one MPU region programmed
 * RO + executable; see g_mpu_region_table[] in
 * arch/arm/src/rzv2h/rzv2h_mpu_region.c.
 */

__mpu_code_start = ABSOLUTE(ORIGIN(RAM));

SECTIONS
{

    /* The table always has a load copy at the start of RAM, at
     * _vector_load, which arm_boot() copies to VECTOR_ADDRESS (ITCM).
     *
     * The U-Boot image links the table at its ITCM run address and stores
     * it in RAM (AT> RAM), so nuttx_uboot.bin starts with it.  CR8_0
     * reaches it at reset through the U-Boot trampoline described at the
     * top of this file, so the load copy has to stay at ORIGIN(RAM).
     *
     * The debugger image links the table in RAM.  That works because the
     * table is position independent: its branch slots are PC-relative and
     * its handler entries contain absolute addresses.
     */

    .vectors :
    {
        . = ALIGN(32);
        _vector_start = ABSOLUTE(.);
        KEEP(*(.vectors))
        _vector_end = ABSOLUTE(.);
#ifdef CONFIG_RZV2H_UBOOT_IMAGE
    } > ITCM AT> RAM
#else
    } > RAM
#endif

    /* Where the table is stored in the image, as opposed to _vector_start,
     * where it runs.  The two are equal in debugger builds.
     */

    _vector_load = LOADADDR(.vectors);

#ifdef CONFIG_RZV2H_UBOOT_IMAGE
    ASSERT(_vector_load == ORIGIN(RAM),
           "U-Boot reset trampoline expects .vectors at ORIGIN(RAM)")
#endif
#ifdef CONFIG_RZV2H_ITCM_HOTCODE
    /* Hot network code that runs from ITCM (CONFIG_RZV2H_ITCM_HOTCODE): the
     * GBETH driver, the IPv4/UDP/TCP receive and send paths, the socket
     * calls, I/O buffers, memcpy, semaphores and the atomics, watchdogs and
     * system time they use, the scheduler's context switch and the
     * interrupt entry.
     * It is stored in RAM and copied to ITCM by arm_boot(), after the
     * exception table at the start of ITCM.  It comes before .text so that
     * these input sections are not taken by the *(.text .text.*) below.
     */

    .itcm ORIGIN(ITCM) + 0x100 :
    {
        _sitcm = ABSOLUTE(.);
        *libarch.a:rzv2h_gbeth.o(.text .text.*)
        *libnet.a:ipv4_input.o(.text .text.*)
        *libnet.a:udp_input.o(.text .text.*)
        *libnet.a:udp_callback.o(.text .text.*)
        *libnet.a:udp_recvfrom.o(.text .text.*)
        *libnet.a:devif_callback.o(.text .text.*)
        *libnet.a:netdev_iob.o(.text .text.*)
        *libnet.a:net_lock.o(.text .text.*)
        *libnet.a:net_iob_concat.o(.text .text.*)
        *libnet.a:tcp_input.o(.text .text.*)
        *libnet.a:tcp_callback.o(.text .text.*)
        *libmm.a:iob_*.o(.text .text.*)
        *libc.a:arch_memcpy.o(.text .text.*)
        *libc.a:sem_wait.o(.text .text.*)
        *libc.a:sem_post.o(.text .text.*)
        *libc.a:lib_mutex.o(.text .text.*)
        *libsched.a:sem_wait.o(.text .text.*)
        *libsched.a:sem_post.o(.text .text.*)
        *libsched.a:sem_timedwait.o(.text .text.*)
        *libsched.a:kwork_thread.o(.text .text.*)
        *libsched.a:kwork_queue.o(.text .text.*)
        *libsched.a:sched_yield.o(.text .text.*)
        *libsched.a:sched_addreadytorun.o(.text .text.*)
        *libsched.a:sched_removereadytorun.o(.text .text.*)
        *libsched.a:sched_addblocked.o(.text .text.*)
        *libsched.a:sched_removeblocked.o(.text .text.*)
        *libsched.a:sched_switchcontext.o(.text .text.*)
        *libsched.a:sched_mergepending.o(.text .text.*)
        *libsched.a:sched_lock.o(.text .text.*)
        *libsched.a:sched_unlock.o(.text .text.*)
        *libsched.a:sched_gettcb.o(.text .text.*)
        *libsched.a:sem_waitirq.o(.text .text.*)
        *libsched.a:irq_dispatch.o(.text .text.*)
        *libarch.a:arm_vectors.o(.text .text.*)
        *libarch.a:arm_doirq.o(.text .text.*)
        *libarch.a:arm_syscall.o(.text .text.*)
        *libarch.a:arm_gicv2.o(.text .text.*)
        *libfs.a:fs_files.o(.text .text.*)
        *libfs.a:socket.o(.text .text.*)
        *libnet.a:recvfrom.o(.text .text.*)
        *libnet.a:recvmsg.o(.text .text.*)
        *libnet.a:recv.o(.text .text.*)
        *libnet.a:send.o(.text .text.*)
        *libnet.a:sendto.o(.text .text.*)
        *libnet.a:sendmsg.o(.text .text.*)
        *libnet.a:udp_sendto_buffered.o(.text .text.*)
        *libnet.a:udp_send.o(.text .text.*)
        *libnet.a:udp_wrbuffer.o(.text .text.*)
        *libnet.a:udp_conn.o(.text .text.*)
        *libnet.a:udp_finddev.o(.text .text.*)
        *libnet.a:devif_poll.o(.text .text.*)
        *libnet.a:devif_send.o(.text .text.*)
        *libnet.a:devif_iobsend.o(.text .text.*)
        *libnet.a:netdev_txnotify.o(.text .text.*)
        *libnet.a:arp_out.o(.text .text.*)
        *libnet.a:arp_table.o(.text .text.*)
        *libnet.a:arp_send.o(.text .text.*)
        *libnet.a:tcp_recvfrom.o(.text .text.*)
        *libnet.a:tcp_send_buffered.o(.text .text.*)
        *libnet.a:tcp_send.o(.text .text.*)
        *libnet.a:tcp_appsend.o(.text .text.*)
        *libnet.a:tcp_devpoll.o(.text .text.*)
        *libnet.a:tcp_wrbuffer.o(.text .text.*)
        *libnet.a:tcp_recvwindow.o(.text .text.*)
        *libnet.a:netdev_lock.o(.text .text.*)
        *libnet.a:net_bufpool.o(.text .text.*)
        *libnet.a:udp_devpoll.o(.text .text.*)
        *libnet.a:ipv4_build_header.o(.text .text.*)
        *libc.a:arch_atomic.o(.text .text.*)
        *libc.a:tls_getinfo.o(.text .text.*)
        *libc.a:lib_errno.o(.text .text.*)
        *libc.a:sem_getvalue.o(.text .text.*)
        *libc.a:sig_isemptyset.o(.text .text.*)
        *libsched.a:sem_tickwait.o(.text .text.*)
        *libsched.a:sem_clockwait.o(.text .text.*)
        *libsched.a:clock_systime_ticks.o(.text .text.*)
        *libsched.a:wd_start.o(.text .text.*)
        *libsched.a:wd_cancel.o(.text .text.*)
        *libsched.a:task_gettid.o(.text .text.*)
        *libsched.a:sig_pending.o(.text .text.*)
        *libc.a:lib_memset.o(.text .text.*)
        *libc.a:sem_init.o(.text .text.*)
        *libc.a:sem_trywait.o(.text .text.*)
        *libc.a:sig_nandset.o(.text .text.*)
        *libsched.a:sem_destroy.o(.text .text.*)
        *libsched.a:sched_idletask.o(.text .text.*)
        . = ALIGN(4);
        _eitcm = ABSOLUTE(.);
    } AT> RAM

    _litcm = LOADADDR(.itcm);
    ASSERT(_eitcm <= ORIGIN(ITCM) + LENGTH(ITCM), ".itcm overflows ITCM")
#endif

    .text :
    {
        . = ALIGN(4);
        _stext = ABSOLUTE(.);
        *(.text .text.*)
        *(.fixup)
        *(.gnu.warning)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7)
        *(.glue_7t)
        *(.got)
        *(.gcc_except_table)
        *(.gnu.linkonce.r.*)
        _etext = ABSOLUTE(.);
    } > RAM

    .init_section :
    {
        . = ALIGN(4);
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
    } > RAM

    .dtcm (NOLOAD) :
    {
        KEEP(*(.stack))
        KEEP(*(.stack.*))
        . = ALIGN(64);
        *(.dtcm.gbeth)
    } > DTCM

    .ARM.extab :
    {
        *(.ARM.extab*)
    } > RAM

    PROVIDE_HIDDEN (__exidx_start = .);
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > RAM
    PROVIDE_HIDDEN (__exidx_end = .);

    /* Round the read-only part of the image up to a power of two.
     *
     * The MPU region that makes this range executable and read-only has to
     * obey PMSAv7: the size must be 2^n and the base must be a multiple of
     * that size.  Padding to 2^n here means mpu_modify_region() finds an
     * exact fit and needs no sub-region disable mask, so the region cannot
     * silently spill over into .data - which would make every write to a
     * global fault.  The base needs no work: ORIGIN(RAM) is 512 KB aligned,
     * so it is aligned to any 2^n <= 512 KB.
     *
     * The padding has to happen inside an output section attached to RAM.
     * Assignments to "." outside of an output section move the location
     * counter but do NOT move the allocation pointer of the memory region,
     * so the next "> RAM" section would be placed back at the unpadded
     * address and only the symbols would move.
     *
     * (NOLOAD) keeps the padding out of the ELF file itself; it still
     * occupies address space, which is all that is needed here.
     *
     * LOG2CEIL() is a GNU ld builtin (binutils >= 2.29).
     */

    .rom_region_pad (NOLOAD) :
    {
        . = ALIGN(32);
        . = ALIGN(1 << LOG2CEIL(ABSOLUTE(.) - __mpu_code_start));
    } > RAM

    __mpu_code_end = ABSOLUTE(.);

    /* Where arm_head.S copies the .data initial values from.  .data is
     * loaded in place, so this equals _sdata and the copy is a no-op; it
     * must not be "." here, because .dma may sit between the two.
     */

    _eronly = LOADADDR(.data);

#ifdef CONFIG_RZV2H_RPTUN
    /* Multi-OS sample maps the resource table directly at 0x42f00000 through
     * UIO.  Like the Renesas reference linker script, keep the live table
     * NOLOAD and initialize it from a template during board bring-up.
     */

    .resource_table (NOLOAD) :
    {
        . = ALIGN(8);
        KEEP(*(.resource_table))
        . = ALIGN(4);
    } > RSCTBL

    ASSERT(SIZEOF(.resource_table) <= BOARD_RPTUN_RESOURCE_SIZE,
           "RZ/V2H resource table exceeds the reserved region")
#endif

#ifdef CONFIG_RZV2H_GBETH
    /* GBETH DMA memory: descriptors and packet buffers, mapped Normal
     * non-cacheable by one MPU region (see rzv2h_mpu_region.c), which needs
     * a 2^n size and a base that is a multiple of it.  The section follows
     * the code window, whose end is aligned to its own 2^n size, and is
     * padded up to 2^n the same way as .rom_region_pad.  (NOLOAD): it is not
     * zeroed at boot; the driver initializes it in ifup.
     */

    .dma (NOLOAD) :
    {
        __dma_start = ABSOLUTE(.);
        KEEP(*(.dma .dma.*))
        . = ALIGN(1 << LOG2CEIL(ABSOLUTE(.) - __dma_start));
        __dma_end = ABSOLUTE(.);
    } > RAM

    ASSERT(__dma_start % (__dma_end - __dma_start) == 0,
           "GBETH .dma region is not aligned to its size")
#endif

    .data :
    {
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS
        . = ALIGN(4);
        _edata = ABSOLUTE(.);
    } > RAM

    /* The FSP BSP heap (g_heap, BSP_CFG_HEAP_BYTES).  NuttX has its own heap
     * and nothing calls the FSP's sbrk, so it stays out of DTCM, which holds
     * the GBETH I/O buffers instead.  Its input section is PROGBITS: (NOLOAD)
     * keeps the 32 KB of zeros out of the image, and placing it before .bss
     * keeps it below _ebss, where the NuttX heap starts.
     */

    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        __HeapBase = .;
        KEEP(*(.heap))
        __HeapLimit = .;
    } > RAM

    .bss :
    {
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = ABSOLUTE(.);
    } > RAM

    /* The padding above is not linear: crossing 128 KB of code costs another
     * 128 KB of SRAM, taken straight out of the heap.  Fail the link instead
     * of losing a quarter of the RAM silently.
     */

    /* Stabs debugging sections. */
    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .stab.excl 0 : { *(.stab.excl) }
    .stab.exclstr 0 : { *(.stab.exclstr) }
    .stab.index 0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment 0 : { *(.comment) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
}
