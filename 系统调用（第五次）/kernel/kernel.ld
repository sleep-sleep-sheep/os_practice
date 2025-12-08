OUTPUT_ARCH("riscv")
ENTRY(_entry)  
KERNEL_BASE = 0x80000000; 
PGSIZE = 4096; 
SECTIONS
{
  . = KERNEL_BASE;  

  .text : {
    *(.text.entry) 
    *(.text .text.*)
    . = ALIGN(PGSIZE);
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.rodata .rodata.*)
    . = ALIGN(PGSIZE); 
  }

  .data : {
    . = ALIGN(16);
    *(.data .data.*)
    . = ALIGN(PGSIZE); 
  }


  .bss : {
  . = ALIGN(PGSIZE);         
  *(.bss)                   
  *(.bss.*)
  *(.sbss)                   
  *(.sbss.*)
  *(COMMON)                 
  . = ALIGN(PGSIZE);         
  PROVIDE(end = .);          
}


  PROVIDE(KERNEL_BASE = KERNEL_BASE);
  PROVIDE(PHYS_BASE = KERNEL_BASE);
  PROVIDE(PHYS_TOP = 0x88000000);  
  PROVIDE(DEVBASE = 0x10000000);   
  PROVIDE(DEVSIZE = 0x08000000);   
}