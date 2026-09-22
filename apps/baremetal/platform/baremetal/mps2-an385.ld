/* Linker script for QEMU's mps2-an385 (Cortex-M3), for the minimum-footprint
 * decoder probe (roadmap PF7).
 *
 * ONE memory region, and everything - code, initialised data, bss, heap and
 * stack - lives in it starting at address 0. That is not a simplification of
 * a real board's flash/RAM split so much as a match for how QEMU boots this
 * one: `-kernel prog.elf` loads the ELF's program headers straight into
 * memory, so a .data section whose load address equals its virtual address
 * arrives already initialised and needs no flash-to-RAM copy loop. On the
 * mps2-an385 that region is 4 MB of ZBT SRAM, writable, which is what makes
 * the arrangement legal rather than merely convenient.
 *
 * A Cortex-M does not have a reset vector in the usual sense: it reads the
 * initial stack pointer from address 0 and the reset handler's address from
 * address 4. KEEP(*(.vectors)) first in .text is what puts
 * platform/baremetal/vectors.cpp's table there.
 *
 * The symbol names below are newlib's, not chosen here: crt0 zeroes
 * [__bss_start__, __bss_end__) and _sbrk grows the heap from `end` toward
 * the stack. Renaming any of them silently breaks startup.
 */

MEMORY
{
    MEM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M
}

ENTRY(_start)

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > MEM

    .init : { KEEP(*(SORT_NONE(.init))) } > MEM
    .fini : { KEEP(*(SORT_NONE(.fini))) } > MEM

    /* Present even under -fno-exceptions: the C library's own objects carry
     * unwind tables whether or not this build can throw. Unreferenced, but
     * they must land somewhere or the link leaves them as orphans. */
    .ARM.extab : { *(.ARM.extab*) } > MEM
    .ARM.exidx : { *(.ARM.exidx*) } > MEM

    /* C++ static initialisation. NOT optional and NOT boilerplate: newlib's
     * __libc_init_array walks [__init_array_start, __init_array_end) before
     * calling main, and if this script does not define those bounds it walks
     * whatever the symbols happened to resolve to and calls it. That failure
     * mode is a data abort somewhere in startup with nothing printed - which
     * is exactly how it presented the first time this probe was linked
     * without these three sections.
     *
     * KEEP() because --gc-sections would otherwise be quite right that
     * nothing references them. */
    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > MEM

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtend.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > MEM

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE(*crtbegin.o *crtend.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > MEM

    /* Thread-local storage. This target has one thread and no thread-pointer
     * runtime, so platform/baremetal/tls.cpp supplies __aeabi_read_tp over a
     * single static block - see that file. These two sections exist here only
     * so the asserts at the end of this script can check the two assumptions
     * that makes: that the whole TLS image fits in that block, and that none
     * of it needs a non-zero initial value copied in.
     */
    .tdata : { *(.tdata .tdata.*) } > MEM
    .tbss : { *(.tbss .tbss.*) *(.tcommon) } > MEM

    .data :
    {
        *(.data*)
        . = ALIGN(4);
    } > MEM

    .bss :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } > MEM

    . = ALIGN(8);
    end = .;
    __end__ = .;
    PROVIDE(_end = .);

    __stack = ORIGIN(MEM) + LENGTH(MEM);
}

/* The two contracts platform/baremetal/tls.cpp's single-block thread pointer
 * depends on. Both are link-time, so a change that breaks either fails here
 * with a sentence rather than at run time with a corrupted spectrum. */
ASSERT(SIZEOF(.tdata) == 0,
    "the bare-metal TLS stub only supports zero-initialised thread_local storage; something in this build has a non-zero initialiser whose image nothing copies in")
ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) + 8 <= 64 * 1024,
    "the thread_local image no longer fits apps/baremetal/platform/baremetal/tls.cpp's 64 KiB block - grow kTlsBlockBytes and this assert together")
