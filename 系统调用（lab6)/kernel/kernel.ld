OUTPUT_ARCH( "riscv" )
ENTRY( _entry )
/*指定程序的入口点为_entry
  这主要是给调试器GDB看的，告诉它要运行的话，从这里开始
  真正的硬件跳转地址是由物理地址0x80000000开始的
*/
SECTIONS
{
    /*
   *  OUTPUT_ARCH( "riscv" )  ENTRY( _entry )  告诉链接器我们要生成的是risc-v架构的可执行文件，内核被加载后，应该从_entry标签执行第一条指令
   * ensure that entry.S / _entry is at 0x80000000,
   * where qemu's -kernel jumps.
   *  kernel/entry.o(_entry)  确保这段启动汇编形成的目标文件的代码段放在最前面
   *  . = ALIGN(0x1000);  位置对齐到 4KB 页边界（操作系统通常以页为单位管理内存),方便后续分页管理
   *  trampoline 几句话设置了一种“跳板”机制，用来在不同特权级（比如内核态/用户态）之间切换，如果大小超过一页报错
   *  *(trampsec)  去所有的输入文件(.o)里面找，凡是名字叫做 trampsec 的段，都把内容拷贝到这里来
   *  ALIGN  0x1000的是为了保证分页整齐    16的是为了保证访问的高效、不浪费内存
   */
  . = 0x80000000;

  .text : {
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(etext = .);
    /* etext 标记代码段结束位置，C语言引用 &etext就能知道代码段到哪里结束了，方便后续使用 */
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    /* Small Read-Only Data，RISC-V 特有的优化段 */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
  /* end 告诉 OS，内核使用的所有静态内存到此为止，从end这个地址往后，就是可以自由使用的内存了 */
  /* 可分配物理内存区域 - 从内核结束位置到128MB */
  . = ALIGN(0x1000);
  PROVIDE(ALLOC_BEGIN = .);
  PROVIDE(ALLOC_END = 0x88000000);
}
