#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "trap/trap.h"
#include "memlayout.h"
#include "riscv.h"

/*----------------外部空间------------------*/

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

// in trap_user.c
extern void trap_user_handler();

// 内核页表
extern pgtbl_t kernel_pagetable;

/*----------------本地变量------------------*/

// 进程数组
static proc_t procs[NPROC];

// 第一个进程的指针
static proc_t* proczero;

// 全局的pid和保护它的锁 
static int global_pid = 1;
static spinlock_t lk_pid;

// 申请一个pid(锁保护)
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&lk_pid);
    assert(global_pid >= 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&lk_pid);
    return tmp;
}

// 由于调度器中上了锁，所以这里需要解锁
static void fork_return()
{
    proc_t* p = myproc();
    spinlock_release(&p->lk);
    trap_user_return();
}

// 返回一个未使用的进程空间
// 设置pid + 设置上下文中的ra和sp
// 申请tf和pgtbl使用的物理页
// 返回时持有锁
proc_t* proc_alloc()
{
    proc_t* p;

    // 遍历进程数组找到未使用的槽位
    for (int i = 0; i < NPROC; i++) {
        p = &procs[i];
        spinlock_acquire(&p->lk);
        if (p->state == UNUSED) {
            goto found;
        }
        spinlock_release(&p->lk);
    }
    return NULL;

found:
    // 分配PID
    p->pid = alloc_pid();
    
    // 分配trapframe物理页
    p->tf = (trapframe_t*)pmem_alloc(false);
    if (p->tf == NULL) {
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, sizeof(trapframe_t));
    
    // 初始化页表（包含trapframe和trampoline的映射）
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (p->pgtbl == NULL) {
        pmem_free((uint64)p->tf, false);
        p->tf = NULL;
        spinlock_release(&p->lk);
        return NULL;
    }
    
    // 设置上下文：ra指向fork_return，sp指向内核栈顶
    memset(&p->ctx, 0, sizeof(context_t));
    p->ctx.ra = (uint64)fork_return;
    p->ctx.sp = p->kstack + PGSIZE;
    
    // 初始化其他字段
    p->parent = NULL;
    p->exit_state = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_pages = 0;
    p->mmap = NULL;
    
    return p;
}

// 释放一个进程空间
// 释放pgtbl的整个地址空间
// 释放mmap_region到仓库
// 设置其余各个字段为合适初始值
// tips: 调用者需持有p->lk
void proc_free(proc_t* p)
{
    // 释放trapframe
    if (p->tf) {
        pmem_free((uint64)p->tf, false);
        p->tf = NULL;
    }
    
    // 释放页表及其管理的物理页
    if (p->pgtbl) {
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
    }
    
    // 释放mmap区域链表
    mmap_region_t* mmap = p->mmap;
    while (mmap != NULL) {
        mmap_region_t* next = mmap->next;
        mmap_region_free(mmap);
        mmap = next;
    }
    p->mmap = NULL;
    
    // 重置其他字段
    p->pid = 0;
    p->state = UNUSED;
    p->parent = NULL;
    p->exit_state = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_pages = 0;
}

// 进程模块初始化
void proc_init()
{
    // 初始化PID锁
    spinlock_init(&lk_pid, "pid");
    
    // 遍历进程数组，初始化每个进程的锁和内核栈地址
    for (int i = 0; i < NPROC; i++) {
        spinlock_init(&procs[i].lk, "proc");
        procs[i].kstack = KSTACK(i);//分配内核栈地址
        procs[i].state = UNUSED;//标记为空闲
    }
    
}

// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl;

    // 分配顶级页表
    pgtbl = (pgtbl_t)pmem_alloc(false);
    if (pgtbl == NULL) {
        panic("proc_pgtbl_init: failed to allocate page table");
    }
    memset(pgtbl, 0, PGSIZE);

    // 映射跳板页（和内核页表共享同一虚拟地址和物理页）
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 映射trapframe页
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

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
    uint64 page;
    
    // 使用 proc_alloc 分配进程结构
    proczero = proc_alloc();
    if (proczero == NULL) {
        panic("proc_make_first: failed to allocate process");
    }
    
    // 第一个进程的pid设为0
    proczero->pid = 0;
    
    // ustack 映射 + 设置 ustack_pages 
    page = (uint64)pmem_alloc(false);
    if (page == 0) {
        panic("proc_make_first: failed to allocate user stack");
    }
    memset((void*)page, 0, PGSIZE);
    proczero->ustack_pages = 1;
    // 用户栈在 TRAPFRAME 下方
    uint64 ustack_va = TRAPFRAME - PGSIZE;
    vm_mappages(proczero->pgtbl, ustack_va, page, PGSIZE, PTE_R | PTE_W | PTE_U);

    // data + code 映射
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big\n");
    page = (uint64)pmem_alloc(false);
    if (page == 0) {
        panic("proc_make_first: failed to allocate code page");
    }
    memset((void*)page, 0, PGSIZE);
    // 复制initcode到物理页
    memmove((void*)page, initcode, initcode_len);
    // 代码段在虚拟地址 PGSIZE (跳过最低的空白页)
    vm_mappages(proczero->pgtbl, PGSIZE, page, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // 设置 heap_top
    proczero->heap_top = 2 * PGSIZE;  // 代码段之后
    
    // 初始化 mmap 链表为空
    proczero->mmap = NULL;

    // tf字段设置
    proczero->tf->epc = PGSIZE;                     // 用户入口点（代码起始地址）
    proczero->tf->sp = ustack_va + PGSIZE;          // 用户栈顶（栈向下生长）
    proczero->tf->kernel_satp = r_satp();           // 内核页表
    proczero->tf->kernel_sp = proczero->kstack + PGSIZE;   // 内核栈顶
    proczero->tf->kernel_trap = (uint64)trap_user_handler;
    proczero->tf->kernel_hartid = r_tp();

    // 设置进程状态为RUNNABLE，让调度器调度它
    proczero->state = RUNNABLE;
    
    // 释放alloc时获取的锁
    spinlock_release(&proczero->lk);
    
    // 替换原有输出：修改格式、前缀、表述内容
    printf("[Process Manager] Init process (pid=%d) created successfully. Entry point=0x%lx, User stack top=0x%lx\n", 
           proczero->pid, proczero->tf->epc, proczero->tf->sp);
}

// 进程复制
// UNUSED -> RUNNABLE
int proc_fork()
{
    proc_t* p = myproc();
    
    // 替换原有输出：新增进程操作标识，修改调试信息表述
    printf("[Process Operation] Fork request received from process (pid=%d). Starting child process creation...\n", p->pid);
    
    // 分配新进程
    proc_t* np = proc_alloc();
    if (np == NULL) {
        return -1;
    }
    
    // 复制用户内存空间
    // 首先为新进程分配用户栈
    uint64 page = (uint64)pmem_alloc(false);
    if (page == 0) {
        proc_free(np);
        spinlock_release(&np->lk);
        return -1;
    }
    memset((void*)page, 0, PGSIZE);
    np->ustack_pages = p->ustack_pages;
    uint64 ustack_va = TRAPFRAME - PGSIZE;
    vm_mappages(np->pgtbl, ustack_va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
    
    // 复制父进程的页表内容（代码、堆、mmap等区域）
    uvm_copy_pgtbl(p->pgtbl, np->pgtbl, p->heap_top, p->ustack_pages, p->mmap);
    
    // 复制堆顶和mmap区域信息
    np->heap_top = p->heap_top;
    
    // 复制mmap链表
    mmap_region_t* src_mmap = p->mmap;
    mmap_region_t** dst_mmap = &np->mmap;
    while (src_mmap != NULL) {
        mmap_region_t* new_mmap = mmap_region_alloc();
        if (new_mmap == NULL) {
            proc_free(np);
            spinlock_release(&np->lk);
            return -1;
        }
        new_mmap->begin = src_mmap->begin;
        new_mmap->npages = src_mmap->npages;
        new_mmap->next = NULL;
        *dst_mmap = new_mmap;
        dst_mmap = &new_mmap->next;
        src_mmap = src_mmap->next;
    }
    
    // 复制trapframe,复制所有寄存器状态
    memmove(np->tf, p->tf, sizeof(trapframe_t));
    
    // 设置子进程返回值为0（通过修改a0寄存器）
    np->tf->a0 = 0;
    
    // 设置子进程的内核栈信息
    np->tf->kernel_sp = np->kstack + PGSIZE;
    np->tf->kernel_satp = r_satp();
    np->tf->kernel_trap = (uint64)trap_user_handler;
    
    // 设置父进程
    np->parent = p;
    
    // 保存子进程pid用于返回
    int pid = np->pid;
    
    // 设置子进程状态为RUNNABLE
    np->state = RUNNABLE;
    
    // 替换原有输出：增强信息维度，修改表述风格
    printf("[Process Operation] Child process (pid=%d) created successfully by parent (pid=%d). Ready for scheduling.\n", 
           pid, p->pid);
    
    // 释放锁
    spinlock_release(&np->lk);
    
    // 父进程返回子进程pid
    return pid;
}

// 进程放弃CPU的控制权
// RUNNING -> RUNNABLE
void proc_yield()
{
    proc_t* p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 等待一个子进程进入 ZOMBIE 状态
// 将退出的子进程的exit_state放入用户给的地址 addr
// 成功返回子进程pid，失败返回-1
int proc_wait(uint64 addr)
{
    proc_t* p = myproc();
    int havekids, pid;
    
    // 替换原有输出：修改等待操作的表述，增加进程标识清晰度
    printf("[Process Synchronization] Process (pid=%d) entering wait state, waiting for child process exit...\n", p->pid);
    
    spinlock_acquire(&p->lk);
    
    for (;;) {
        // 扫描进程表查找已退出的子进程
        havekids = 0;
        for (int i = 0; i < NPROC; i++) {
            proc_t* pp = &procs[i];
            if (pp->parent == p) {
                spinlock_acquire(&pp->lk);
                havekids = 1;
                
                if (pp->state == ZOMBIE) {
                    // 找到已退出的子进程,回收子进程资源并返回子进程PID
                    pid = pp->pid;
                    
                    // 替换原有输出：增强退出状态信息，修改调试格式
                    printf("[Process Synchronization] Process (pid=%d) detected zombie child (pid=%d), exit status: %d. Starting resource reclamation...\n", 
                           p->pid, pid, pp->exit_state);
                    
                    // 如果用户提供了地址，复制exit_state
                    if (addr != 0) {
                        uvm_copyout(p->pgtbl, addr, (uint64)&pp->exit_state, sizeof(int));
                    }
                    
                    // 释放子进程资源
                    proc_free(pp);
                    spinlock_release(&pp->lk);
                    spinlock_release(&p->lk);
                    return pid;
                }
                
                spinlock_release(&pp->lk);
            }
        }
        
        // 没有子进程
        if (!havekids) {
            spinlock_release(&p->lk);
            return -1;
        }
        
        // 等待子进程退出
        // 替换原有输出：修改睡眠操作的表述，增加进程状态信息
        printf("[Process Synchronization] Process (pid=%d) has no exited children, entering sleep state...\n", p->pid);
        proc_sleep(p, &p->lk);
        printf("[Process Synchronization] Process (pid=%d) woken up, resuming wait operation...\n", p->pid);
    }
}

// 父进程退出，子进程认proczero做父，因为它永不退出
static void proc_reparent(proc_t* parent)
{
    for (int i = 0; i < NPROC; i++) {
        proc_t* p = &procs[i];
        if (p->parent == parent) {
            p->parent = proczero;
        }
    }
}

// 唤醒一个进程（被proc_exit调用唤醒父进程）
static void proc_wakeup_one(proc_t* p)
{
    // 不需要断言，直接检查条件
    spinlock_acquire(&p->lk);
    if (p->state == SLEEPING && p->sleep_space == p) {
        p->state = RUNNABLE;
    }
    spinlock_release(&p->lk);
}

// 进程退出
void proc_exit(int exit_state)
{
    proc_t* p = myproc();
    
    // 替换原有输出：修改退出操作的表述，增加退出状态信息
    printf("[Process Operation] Process (pid=%d) initiating exit procedure, exit status: %d\n", p->pid, exit_state);
    
    if (p == proczero) {
        panic("proc_exit: proczero exiting");
    }
    
    // 将子进程托付给proczero
    proc_reparent(p);
    
    // 唤醒父进程（它可能在wait中睡眠）
    // 替换原有输出：修改唤醒操作的表述，增加父子进程标识
    printf("[Process Synchronization] Process (pid=%d) waking up its parent process (pid=%d) for exit notification...\n", 
           p->pid, p->parent->pid);
    proc_wakeup_one(p->parent);
    
    // 获取锁，设置退出状态
    spinlock_acquire(&p->lk);
    p->exit_state = exit_state;
    p->state = ZOMBIE;
    
    // 替换原有输出：修改僵尸进程的表述，增加进程状态信息
    printf("[Process Operation] Process (pid=%d) has entered ZOMBIE state, waiting for parent to reclaim resources.\n", p->pid);
    
    // 切换到调度器，永不返回
    proc_sched();
    
    panic("proc_exit: zombie exit");
}

// 进程切换到调度器
// ps: 调用者保证持有当前进程的锁
void proc_sched()
{
    proc_t* p = myproc();
    
    // 检查是否持有进程锁
    assert(spinlock_holding(&p->lk), "proc_sched: not holding lock");
    
    // 检查锁的嵌套深度为1（只持有进程锁）
    assert(mycpu()->noff == 1, "proc_sched: locks");
    
    // 检查进程状态不是RUNNING
    assert(p->state != RUNNING, "proc_sched: running");
    
    // 检查中断是否关闭
    assert(intr_get() == 0, "proc_sched: interruptible");
    
    // 保存中断状态
    int intena = mycpu()->origin;
    
    // 切换到调度器上下文
    swtch(&p->ctx, &mycpu()->ctx);
    
    // 恢复中断状态
    mycpu()->origin = intena;
}

// 调度器
void proc_scheduler()
{
    cpu_t* c = mycpu();
    c->proc = NULL;
    
    // 记录每个CPU上一次运行的进程pid，用于减少重复输出
    static int last_pid[NCPU] = {-1, -1};
    
    // 替换原有输出：修改调度器入口的表述，增加CPU标识清晰度
    printf("[Scheduler] CPU %d has entered the global process scheduler loop.\n", mycpuid());
    
    for (;;) {
        // 开启中断以处理设备中断，响应时钟
        intr_on();
        
        //遍历进程表，找到RUNNABLE状态的进程
        for (int i = 0; i < NPROC; i++) {
            proc_t* p = &procs[i];
            spinlock_acquire(&p->lk);
            
            if (p->state == RUNNABLE) {
                // 只在切换到不同进程时输出
                if (last_pid[mycpuid()] != p->pid) {
                    // 替换原有输出：修改调度操作的表述，增加CPU与进程的关联信息
                    printf("[Scheduler] CPU %d is scheduling process (pid=%d) for execution.\n", mycpuid(), p->pid);
                    last_pid[mycpuid()] = p->pid;
                }
                
                p->state = RUNNING;
                c->proc = p;
                
                swtch(&c->ctx, &p->ctx);//切换上下文
                
                // 进程执行完毕(被时钟中断或主动yield)回到这里
                c->proc = NULL;
            }
            
            spinlock_release(&p->lk);
        }
        
        intr_on();
        asm volatile("wfi");// wait For interrupt
    }
}

// 进程睡眠在sleep_space
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    proc_t* p = myproc();
    
    // 必须先获取进程锁，然后才能释放条件锁
    // 这样可以保证不会丢失wakeup
    if (lk != &p->lk) {
        spinlock_acquire(&p->lk);
        spinlock_release(lk);
    }
    
    // 设置睡眠等待空间
    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    
    // 调度到其他进程
    proc_sched();
    
    // 被唤醒后清除睡眠空间
    p->sleep_space = NULL;
    
    // 重新获取条件锁
    if (lk != &p->lk) {
        spinlock_release(&p->lk);
        spinlock_acquire(lk);
    }
}

// 唤醒所有在sleep_space沉睡的进程
void proc_wakeup(void* sleep_space)
{
    for (int i = 0; i < NPROC; i++) {
        proc_t* p = &procs[i];
        if (p != myproc()) {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}