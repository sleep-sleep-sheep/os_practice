kernel.ld  定义的是关于内核部分包含东西的代码
不可分配区域：这些段是被所有进程共享的，有且只有一份，这些段在物理内存中是连续排列的


可分配区域：  
用于实现不同进程的存放位置
end ~ ALLOC_END
end 由链接器定义（脚本中 PROVIDE(end = .);），是可分配区域的起始地址；



查看地址布局：
 make build
 riscv64-linux-gnu-objdump -h kernel-qemu



外设中断：
只有一个PLIC
中断分给一个核，其他核就不用处理了