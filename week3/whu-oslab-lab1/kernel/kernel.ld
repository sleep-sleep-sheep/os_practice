OUTPUT_ARCH("riscv")
ENTRY(_entry)  
KERNEL_BASE = 0x80000000; 
PGSIZE = 4096; 
SECTIONS
{
  . = KERNEL_BASE;  

  /* 1. .text段：先放入口代码（.text.entry），结束后页对齐（原脚本这里是对的，不用改） */
  .text : {
    *(.text.entry)  /* 补充：优先放入口函数，避免地址错乱（可选但推荐） */
    *(.text .text.*)
    . = ALIGN(PGSIZE);
    PROVIDE(etext = .);
  }

  /* 2. .rodata段：结束后加页对齐（核心补充！） */
  .rodata : {
    . = ALIGN(16);
    *(.rodata .rodata.*)
    . = ALIGN(PGSIZE);  /* 加这行：让只读数据段结束在页边界 */
  }

  /* 3. .data段：结束后加页对齐（核心补充！） */
  .data : {
    . = ALIGN(16);
    *(.data .data.*)
    . = ALIGN(PGSIZE);  /* 加这行：让已初始化数据段结束在页边界 */
  }

  /* 4. .bss段：原脚本是对的，不用改 */
  .bss : {
  . = ALIGN(PGSIZE);          /* 起始对齐*/
  *(.bss)                     /* 原.bss段数据  */
  *(.bss.*)
  *(.sbss)                    /*新增：包含小未初始化数据段（.sbss） */
  *(.sbss.*)
  *(COMMON)                   /* 包含COMMON段  */
  . = ALIGN(PGSIZE);          /* 结束对齐 */
  PROVIDE(end = .);           /* end 现在 = .sbss 段结束后的地址 */
}

  /* 后面的符号导出，原脚本不用改 */
  PROVIDE(KERNEL_BASE = KERNEL_BASE);
  PROVIDE(PHYS_BASE = KERNEL_BASE);
  PROVIDE(PHYS_TOP = 0x880008000);  
  PROVIDE(DEVBASE = 0x10000000);   
  PROVIDE(DEVSIZE = 0x08000000);   
}