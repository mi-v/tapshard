/* SPDX-License-Identifier: MIT */
ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0xf4000, LENGTH = 0xa000
 RAM (rwx) : ORIGIN = 0x20000008, LENGTH = 0x3ffe8
}
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > FLASH
 .boot_id 0xf4100 : { KEEP(*(.boot_id)) } > FLASH
 .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
 .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
 __data_load = LOADADDR(.data);
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
 /DISCARD/ : { *(.comment*) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
 ASSERT(SIZEOF(.bss) == 0, "Bootloader must not rely on BSS initialization")
 ASSERT(SIZEOF(.data) == 0, "Bootloader mutable state must be on the erased stack")
 ASSERT(ORIGIN(RAM) + LENGTH(RAM) == 0x2003fff0, "Reserve top 16 bytes for reset marker")
 ASSERT(ADDR(.text) == 0xf4110, "Code must follow the bootloader vectors and ID")
 ASSERT(__data_end < 0x2003e000, "Reserve at least 8 KiB for boot stack")
}
