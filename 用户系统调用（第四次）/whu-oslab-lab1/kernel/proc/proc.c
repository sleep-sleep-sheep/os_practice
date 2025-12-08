#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();


// 第一个进程
static proc_t proczero;


//初始化用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
   uint64 pgtbl=pgtbl_create();
   if(!pgtbl){
       panic("proc_pgtbl_init: pgtbl_create failed\n");
   }
   uint64 trampoline_pa=(uint64)trampoline;
   vm_mappages(pgtbl,TRAMPOLINE,trampoline_pa,PGSIZE,PTE_R|PTE_X|PTE_U);

   void *trapframe_pa= pmem_alloc(false);
   vm_mappages(pgtbl,trapframe_pa,(uint64)trapframe_pa,PGSIZE,PTE_R|PTE_W|PTE_U);


   void* ustack_pa= pmem_alloc(false);
    vm_mappages(pgtbl,USER_STACK-(PGSIZE), (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);

   //映射用户代码
   void *code_pa= pmem_alloc(false);
   memcpy(code_pa,initcode,initcode_len);
   vm_mappages(pgtbl, USER_CODE, (uint64)code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
void proc_make_first()
{
    proczero.pid=0;
    proczero.pgtbl=proc_pgtbl_init(TRAPFRAME);
    proczero.tf =(trapframe_t*)TRAPFRAME;
    proczero.ustack_pages= 1;
    proczero.heap_top= USER_HEAP+PGSIZE;


    proczero.tf->epc=USER_CODE;
    proczero.tf->kernel_trap=(uint64)trap_user_return;
    proczero.tf->kernel_sp=KSTACK(proczero.pid)+PGSIZE;

    proczero.kstack=KSTACK(proczero.pid);
    proczero.ctx.ra=(uint64)trap_user_return;
    proczero.ctx.sp=proczero.kernel_sp;
    cpu_t *c=mycpu();
    c->proc=&proczero;
    swtch(&c->ctx,&proczero.ctx);
}