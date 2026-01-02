# 项目简介

本项目为操作系统实践的项目源代码，其参考了xv6的项目结构。

# 代码架构：

WHU-OSLAB  
├── include  
│   │   └── uart.h  
│   ├── lib  
│   │   ├── print.h  
│   │   └── lock.h  
│   ├── proc  
│   │   └── cpu.h  
│   ├── common.h  
│   ├── memlayout.h  
│   └── riscv.h  
├── kernel  
│   ├── boot  
│   │   ├── main.c   
│   │   ├── start.c   
│   │   ├── entry.S  
│   │   └── Makefile  
│   ├── dev  
│   │   ├── uart.c  
│   │   └── Makefile  
│   ├── lib  
│   │   ├── print.c   
│   │   ├── spinlock.c 
│   │   └── Makefile    
│   ├── proc  
│   │   ├── proc.c 
│   │   └── Makefile  
│   ├── Makefile  
│   └── kernel.ld  
├── Makefile  
└── common.mk  

# 使用方法

清除构建Makefile：

```bash
make clean
```

构建项目并执行：

```bash
make qemu
```