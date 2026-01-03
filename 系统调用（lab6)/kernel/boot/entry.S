# kernel.ld 将_entry作为整个OS的起点放置到0x80000000处
# qemu会自动跳转到0x80000000处并开始执行
# 注意: 此时是M-mode
# qemu -kernel loads the kernel at 0x80000000(qemu把编译后的内核文件放置在这个地址)
# and causes each hart (i.e. CPU) to jump there.
# kernel.ld causes the following code to
# be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # CPU_stack 定义于start.c中
        # sp = CPU_stack + ((hartid + 1) * 4096)
        # 将sp置于当前CPU的内核栈的栈顶
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        #la指令把stack0的地址加载到sp  li设置立即数  mhartid是RISC-V架构里一个特殊寄存器的名字,它里面就存放着当前正在执行这段代码的CPU核心的ID号。
        #csrr (Control and Status Register Read) 指令：专门用来读取这种系统控制寄存器
        #多核心cpu 一般每个cpu都有一套自己的registers
        la sp, CPU_stack
        li a0, 4096
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # 跳转到start.c 中的start函数
        call start
        #安全防护措施，万一错误的跳回来，就让其一直循环
spin:
        j spin
