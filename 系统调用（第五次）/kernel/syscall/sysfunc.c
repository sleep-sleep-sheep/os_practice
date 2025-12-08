#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "memlayout.h"

// 堆伸缩(sys_brk)
uint64 sys_brk() {
    proc_t* p = myproc();
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);

    // 初始化堆顶（如果未初始化）
    if (p->heap_top == 0) {
        p->heap_top = USER_HEAP + PGSIZE;  // 匹配proc_make_first的初始化值
        printf("sys_brk: PID %d init heap_top = 0x%lx\n", p->pid, p->heap_top);
    }

    // 场景1：查询当前堆顶（参数为0）
    if (new_heap_top == 0) {
        printf("sys_brk: PID %d query heap_top = 0x%lx\n", p->pid, p->heap_top);
        return p->heap_top;
    }

    // 检查地址合法性：页对齐 + 堆范围（简化版，仅检查页对齐）
    if ((new_heap_top % PGSIZE) != 0) {
        printf("sys_brk: PID %d new_heap_top 0x%lx not page-aligned (PGSIZE=%d)\n", 
               p->pid, new_heap_top, PGSIZE);
        return -1;
    }

    // 场景2：调整堆顶（简化版，不检查上下限，仅适配测试）
    printf("sys_brk: PID %d adjust heap_top from 0x%lx to 0x%lx\n", 
           p->pid, p->heap_top, new_heap_top);
    p->heap_top = new_heap_top;
    return new_heap_top;
}

// 内存映射(sys_mmap)（占位实现，仅适配编译）
uint64 sys_mmap() {
    printf("sys_mmap: not implemented (test only brk)\n");
    return -1;
}

// 取消内存映射(sys_munmap)（占位实现）
uint64 sys_munmap() {
    printf("sys_munmap: not implemented (test only brk)\n");
    return -1;
}

// copyin 测试 (int数组)
uint64 sys_copyin() {
    proc_t* p = myproc();
    uint64 addr;
    uint32 len;

    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    printf("sys_copyin: PID %d read %d ints from user addr 0x%lx\n", p->pid, len, addr);
    int tmp;
    for(int i = 0; i < len; i++) {
        uvm_copyin(p->pgtbl, (uint64)&tmp, addr + i * sizeof(int), sizeof(int));
        printf("get a number from user: %d\n", tmp);
    }

    return 0;
}

// copyout 测试 (int数组)
uint64 sys_copyout() {
    int L[5] = {1, 2, 3, 4, 5};
    proc_t* p = myproc();
    uint64 addr;

    arg_uint64(0, &addr);
    printf("sys_copyout: PID %d write 5 ints to user addr 0x%lx\n", p->pid, addr);
    uvm_copyout(p->pgtbl, addr, (uint64)L, sizeof(int) * 5);

    return 5;
}

// copyinstr测试
uint64 sys_copyinstr() {
    char s[64];
    proc_t* p = myproc();

    arg_str(0, s, 64);
    printf("sys_copyinstr: PID %d get str from user: %s\n", p->pid, s);

    return 0;
}