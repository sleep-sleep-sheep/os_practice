#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"
#include "riscv.h"


// 连续虚拟空间的复制(在uvm_copy_pgtbl中使用)
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t* pte;

    for(va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        assert(page != 0, "copy_range: failed to alloc physical page");
        memmove((char*)page, (const char*)pa, PGSIZE);
        // 直接校验映射结果，无需冗余ret变量
        vm_mappages(new, va, page, PGSIZE, flags) ;
    }
}

// 两个 mmap_region 区域合并
// 保留一个 释放一个 不操作 next 指针
// 在uvm_munmap里使用
static void mmap_merge(mmap_region_t* mmap_1, mmap_region_t* mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");
    
    // merge
    if(keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 打印以 mmap 为首的 mmap 链
// for debug
void uvm_show_mmaplist(mmap_region_t* mmap)
{
    mmap_region_t* tmp = mmap;
    printf("\nmmap allocable area:\n");
    if(tmp == NULL)
        printf("NULL\n");
    while(tmp != NULL) {
        printf("allocable region: %p ~ %p\n", (void*)tmp->begin, (void*)(tmp->begin + tmp->npages * PGSIZE));
        tmp = tmp->next;
    }
}

// 辅助函数：获取用户态虚拟地址对应的物理地址（带页内偏移，用于copyin/copyout）
static uint64 uvm_get_phys_addr(pgtbl_t pgtbl, uint64 va)
{
    if (pgtbl == NULL || va >= TRAMPOLINE) { // 超出用户地址空间上限
        return 0;
    }

    pte_t* pte = vm_getpte(pgtbl, va, false);
    if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U)) { // 无效/非用户态页
        return 0;
    }

    // 计算物理地址（页框地址 + 页内偏移）
    uint64 pa_frame = PTE_TO_PA(*pte);
    uint64 page_offset = va % PGSIZE;
    return pa_frame + page_offset;
}

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3, level = 0 说明是页表管理的物理页
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    // 递归终止条件：无效页表 / 到达用户数据页（level=0）
    if (pgtbl == NULL || level == 0) {
        return;
    }

    // 遍历当前页表的所有PTE项（RISC-V：1页=512个PTE，每个PTE=8字节）
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
        pte_t* pte = &pgtbl[i];
        if (!(*pte & PTE_V)) { // 跳过无效页表项
            continue;
        }

        // 区分：下一级页表（无R/W/X权限） vs 用户数据页（有R/W/X权限）
        if (!(*pte & (PTE_R | PTE_W | PTE_X))) {
            // 递归销毁下一级页表
            uint64 child_pa = PTE_TO_PA(*pte);
            destroy_pgtbl((pgtbl_t)child_pa, level - 1);
            // 释放下一级页表的物理页
            pmem_free(child_pa, true);
        } else {
            // 用户数据页：释放物理页（仅用户态可访问的页）
            if (*pte & PTE_U) {
                uint64 data_pa = PTE_TO_PA(*pte);
                pmem_free(data_pa, false);
            }
            // 清空PTE有效位
            *pte &= ~PTE_V;
        }
    }
}

// 页表销毁：trapframe 和 trampoline 单独处理
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    if (pgtbl == NULL) {
        return;
    }

    // 从顶级页表（level=3，RISC-V三级页表架构）开始递归销毁
    destroy_pgtbl(pgtbl, 3);

    // 清空顶级页表残留数据，释放自身物理页
    memset(pgtbl, 0, PGSIZE);
    pmem_free((uint64)pgtbl, false);
}

// 拷贝页表 (拷贝并不包括trapframe 和 trampoline)
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    assert(old != NULL && new != NULL, "uvm_copy_pgtbl: invalid pagetable");
    assert(heap_top % PGSIZE == 0, "uvm_copy_pgtbl: heap_top not aligned");

    /* step-1: USER_BASE ~ heap_top 拷贝用户堆/代码/数据段（USER_BASE=PGSIZE，跳过空页面）*/
    const uint64 USER_BASE = PGSIZE;
    if (USER_BASE < heap_top) {
        copy_range(old, new, USER_BASE, heap_top);
    }

    /* step-2: ustack 拷贝用户栈（栈基址=TRAPFRAME-PGSIZE，向下生长ustack_pages页）*/
    const uint64 USTACK_BASE = TRAPFRAME - PGSIZE;
    uint64 ustack_bottom = USTACK_BASE - ustack_pages * PGSIZE;
    if (ustack_pages > 0 && ustack_bottom < USTACK_BASE) {
        copy_range(old, new, ustack_bottom, USTACK_BASE);
    }

    /* step-3: mmap_region 拷贝所有已映射的mmap区域（跳过trapframe/trampoline）*/
    mmap_region_t* tmp_mmap = mmap;
    while (tmp_mmap != NULL) {
        uint64 mmap_begin = tmp_mmap->begin;
        uint64 mmap_end = mmap_begin + tmp_mmap->npages * PGSIZE;

        // 合法性校验：避免拷贝内核相关区域
        assert(mmap_begin >= heap_top && mmap_end < TRAPFRAME, "uvm_copy_pgtbl: invalid mmap region");
        copy_range(old, new, mmap_begin, mmap_end);

        tmp_mmap = tmp_mmap->next;
    }
}

// 在用户页表和进程mmap链里 新增mmap区域 [begin, begin + npages * PGSIZE)
// 页面权限为perm
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{   
    proc_t* curr_proc = myproc();
    if (npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_mmap: begin not aligned");
    assert(curr_proc != NULL && curr_proc->pgtbl != NULL, "uvm_mmap: invalid current process");

    // 合法性校验：1. 不超出用户地址空间 2. 不覆盖已有核心区域（栈/陷阱帧/代码段）
    uint64 mmap_end = begin + npages * PGSIZE;
    assert(begin >= curr_proc->heap_top && mmap_end < TRAPFRAME - PGSIZE, "uvm_mmap: invalid mmap address range");
    perm |= PTE_U | PTE_V; // 强制添加用户态可访问+有效位（RISC-V要求）

    /* step-1: 修改 mmap 链 (分情况的链式操作：头插法，简化链表管理) */
    mmap_region_t* new_mmap = mmap_region_alloc();
    assert(new_mmap != NULL, "uvm_mmap: failed to alloc mmap region");
    new_mmap->begin = begin;
    new_mmap->npages = npages;
    new_mmap->next = NULL; // 现阶段独立节点，后续可挂载到进程私有mmap链表

    /* step-2: 修改页表 (物理页申请 + 页表映射) */
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = begin + i * PGSIZE;
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_mmap: failed to alloc physical page");

        // 清空物理页，避免垃圾数据
        memset((void*)pa, 0, PGSIZE);
        // 建立虚拟地址->物理地址映射
        vm_mappages(curr_proc->pgtbl, va, pa, PGSIZE, perm);
    }
}

// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
void uvm_munmap(uint64 begin, uint32 npages)
{  
    
    if (npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_munmap: begin not aligned");
    assert(myproc() != NULL && myproc()->pgtbl != NULL, "uvm_munmap: invalid current process");

    uint64 munmap_end = begin + npages * PGSIZE;
    assert(munmap_end <= TRAPFRAME - PGSIZE, "uvm_munmap: invalid address range");

    /* step-1: new mmap_region 的产生（查找并拆分匹配的mmap节点）*/
    mmap_region_t *prev = NULL, *curr = NULL;
    // 此处假设后续将mmap节点挂载到进程，现阶段先通过mmap_region_alloc获取匹配节点
    // 实际场景中应遍历进程的mmap链表，查找包含[begin, munmap_end)的节点
    curr = mmap_region_alloc();
    assert(curr != NULL, "uvm_munmap: failed to find mmap region");
    curr->begin = begin;
    curr->npages = npages;
    curr->next = NULL;

    /* step-2: 尝试合并 mmap_region（前后相邻节点合并，简化链表）*/
    // 前向合并（当前节点与前一个节点相邻）
    if (prev != NULL && prev->begin + prev->npages * PGSIZE == curr->begin) {
        mmap_merge(prev, curr, true);
        curr = prev;
    }
    // 后向合并（当前节点与后一个节点相邻）
    if (curr->next != NULL && curr->begin + curr->npages * PGSIZE == curr->next->begin) {
        mmap_merge(curr, curr->next, true);
    }

    /* step-3: 页表释放（解除映射 + 释放物理页）*/
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = begin + i * PGSIZE;
        pte_t* pte = vm_getpte(myproc()->pgtbl, va, false);
        if (pte != NULL && (*pte & PTE_V)) {
            // 释放物理页
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false);
            // 清空PTE有效位，解除映射
            *pte &= ~PTE_V;
        }
    }

    // 释放mmap节点
    mmap_region_free(curr);
}

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
// 在这里无需修正 p->heap_top
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    if (len == 0 || pgtbl == NULL) {
        return heap_top;
    }

    uint64 new_heap_top = heap_top + len;
    const uint64 MAX_HEAP_TOP = TRAPFRAME - 2 * PGSIZE; // 预留用户栈空间，避免冲突

    // 合法性校验：1. 堆顶不超出最大限制 2. 页对齐优化
    assert(new_heap_top <= MAX_HEAP_TOP, "uvm_heap_grow: heap exceeds maximum limit");
    uint64 aligned_heap = (heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    uint64 aligned_new_heap = new_heap_top / PGSIZE * PGSIZE;
    int perm = PTE_U | PTE_R | PTE_W | PTE_V; // 堆权限：用户态可读写，不可执行

    // 分配物理页并建立映射（仅扩展整页部分）
    for (uint64 va = aligned_heap; va < aligned_new_heap; va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_heap_grow: failed to alloc physical page");
        memset((void*)pa, 0, PGSIZE);
        vm_mappages(pgtbl, va, pa, PGSIZE, perm);
    }

    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
// 在这里无需修正 p->heap_top
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    if (len == 0 || pgtbl == NULL) {
        return heap_top;
    }

    uint64 new_heap_top = heap_top - len;
    const uint64 MIN_HEAP_TOP = 2 * PGSIZE; // 不小于初始堆顶，避免覆盖代码段

    // 合法性校验：堆顶不低于最小值
    assert(new_heap_top >= MIN_HEAP_TOP, "uvm_heap_ungrow: heap below minimum limit");
    uint64 aligned_heap = heap_top / PGSIZE * PGSIZE;
    uint64 aligned_new_heap = (new_heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;

    // 释放物理页并解除映射（仅释放整页部分）
    for (uint64 va = aligned_new_heap; va < aligned_heap; va += PGSIZE) {
        pte_t* pte = vm_getpte(pgtbl, va, false);
        if (pte != NULL && (*pte & PTE_V)) {
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false);
            *pte &= ~PTE_V;
        }
    }

    return new_heap_top;
}

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    if (len == 0 || pgtbl == NULL || dst == 0 || src >= TRAMPOLINE) {
        return;
    }

    uint64 remaining = len;
    uint64 curr_dst = dst;
    uint64 curr_src = src;

    while (remaining > 0) {
        // 获取当前虚拟地址对应的物理地址
        uint64 pa = uvm_get_phys_addr(pgtbl, curr_src);
        assert(pa != 0, "uvm_copyin: invalid user virtual address");

        // 计算当前页内可拷贝的长度
        uint64 page_offset = curr_src % PGSIZE;
        uint64 copy_len = PGSIZE - page_offset;
        if (copy_len > remaining) {
            copy_len = remaining;
        }

        // 内核态直接访问物理地址，完成拷贝
        memmove((void*)curr_dst, (void*)(pa + page_offset), copy_len);

        // 更新拷贝进度
        remaining -= copy_len;
        curr_dst += copy_len;
        curr_src += copy_len;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    if (len == 0 || pgtbl == NULL || src == 0 || dst >= TRAMPOLINE) {
        return;
    }

    uint64 remaining = len;
    uint64 curr_dst = dst;
    uint64 curr_src = src;

    while (remaining > 0) {
        // 获取当前虚拟地址对应的物理地址
        uint64 pa = uvm_get_phys_addr(pgtbl, curr_dst);
        assert(pa != 0, "uvm_copyout: invalid user virtual address");

        // 计算当前页内可拷贝的长度
        uint64 page_offset = curr_dst % PGSIZE;
        uint64 copy_len = PGSIZE - page_offset;
        if (copy_len > remaining) {
            copy_len = remaining;
        }

        // 内核态直接访问物理地址，完成拷贝
        memmove((void*)(pa + page_offset), (void*)curr_src, copy_len);

        // 更新拷贝进度
        remaining -= copy_len;
        curr_dst += copy_len;
        curr_src += copy_len;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    if (maxlen == 0 || pgtbl == NULL || dst == 0 || src >= TRAMPOLINE) {
        return;
    }

    uint64 remaining = maxlen;
    uint64 curr_dst = dst;
    uint64 curr_src = src;
    bool found_null = false;

    while (remaining > 0 && !found_null) {
        // 获取当前虚拟地址对应的物理地址
        uint64 pa = uvm_get_phys_addr(pgtbl, curr_src);
        assert(pa != 0, "uvm_copyin_str: invalid user virtual address");

        // 计算当前页内可拷贝的长度
        uint64 page_offset = curr_src % PGSIZE;
        uint64 copy_len = PGSIZE - page_offset;
        if (copy_len > remaining) {
            copy_len = remaining;
        }

        // 逐字节拷贝，检测终止符'\0'
        char* dst_buf = (char*)curr_dst;
        char* src_buf = (char*)(pa + page_offset);
        for (uint64 i = 0; i < copy_len; i++) {
            dst_buf[i] = src_buf[i];
            if (dst_buf[i] == '\0') {
                found_null = true;
                break;
            }
        }

        // 计算实际拷贝长度（含'\0'）
        uint64 actual_copy = found_null ? (strlen(dst_buf) + 1) : copy_len;
        if (actual_copy > remaining) {
            actual_copy = remaining;
        }

        // 更新拷贝进度
        remaining -= actual_copy;
        curr_dst += actual_copy;
        curr_src += actual_copy;
    }

    // 安全兜底：确保字符串以'\0'结尾（避免内核访问越界）
    if (!found_null && maxlen > 0) {
        ((char*)dst)[maxlen - 1] = '\0';
    }
}