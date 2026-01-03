#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"
#include "riscv.h"

// 连续虚拟地址空间拷贝（用于页表拷贝时的内存数据迁移）
static void vm_copy_virtual_range(pgtbl_t src_pgtbl, pgtbl_t dst_pgtbl, uint64 start_va, uint64 end_va)
{
    uint64 curr_va, phy_addr, new_phy_page;
    int page_flags;
    pte_t* page_table_entry;

    for(curr_va = start_va; curr_va < end_va; curr_va += PGSIZE)
    {
        page_table_entry = vm_getpte(src_pgtbl, curr_va, false);
        assert(page_table_entry != NULL, "vm_copy_virtual_range: invalid page table entry");
        assert((*page_table_entry) & PTE_V, "vm_copy_virtual_range: page table entry not valid");
        
        phy_addr = (uint64)PTE_TO_PA(*page_table_entry);
        page_flags = (int)PTE_FLAGS(*page_table_entry);

        new_phy_page = (uint64)pmem_alloc(false);
        memmove((char*)new_phy_page, (const char*)phy_addr, PGSIZE);
        vm_mappages(dst_pgtbl, curr_va, new_phy_page, PGSIZE, page_flags);
    }
}

// 两个连续的内存映射区域合并
// 保留指定区域，释放另一个区域，不处理链表后继指针
// 用于内存映射释放时的空闲区域合并优化
static void vm_merge_mmap_regions(mmap_region_t* region_a, mmap_region_t* region_b, bool retain_region_a)
{
    // 验证输入有效性和区域连续性
    assert(region_a != NULL && region_b != NULL, "vm_merge_mmap_regions: null region pointer");
    assert(region_a->begin + region_a->npages * PGSIZE == region_b->begin, "vm_merge_mmap_regions: non-contiguous regions");
    
    // 执行区域合并操作
    if(retain_region_a) {
        region_a->npages += region_b->npages;
        mmap_region_free(region_b);
    } else {
        region_b->begin -= region_a->npages * PGSIZE;
        region_b->npages += region_a->npages;
        mmap_region_free(region_a);
    }
}

// 打印内存映射链表详情（调试专用）
void uvm_show_mmaplist(mmap_region_t* mmap_head)
{
    mmap_region_t* curr_region = mmap_head;
    printf("\n[Virtual Memory] Mmap available free regions:\n");
    if(curr_region == NULL)
        printf("  No available mmap free regions (NULL)\n");
    while(curr_region != NULL) {
        uint64 region_end = curr_region->begin + curr_region->npages * PGSIZE;
        printf("  Free region: 0x%lx ~ 0x%lx (pages: %d)\n", curr_region->begin, region_end, curr_region->npages);
        curr_region = curr_region->next;
    }
}

// 递归销毁页表及其映射的物理页
// 顶级页表level=2（SV39三级页表），level=0为最底层页表
static void vm_recursive_destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    // 遍历页表的512个表项
    for (int i = 0; i < 512; i++) {
        pte_t pte_entry = pgtbl[i];
        
        // 跳过无效页表项
        if (!(pte_entry & PTE_V)) continue;
        
        if (level == 0) {
            // 最底层页表，直接释放映射的物理页
            uint64 phy_addr = PTE_TO_PA(pte_entry);
            pmem_free(phy_addr, false);
        } else if (PTE_CHECK(pte_entry)) {
            // 非叶子节点，递归销毁下级页表
            pgtbl_t child_pgtbl = (pgtbl_t)PTE_TO_PA(pte_entry);
            vm_recursive_destroy_pgtbl(child_pgtbl, level - 1);
        } else {
            // 叶子节点（带访问权限），释放对应的物理页
            uint64 phy_addr = PTE_TO_PA(pte_entry);
            pmem_free(phy_addr, false);
        }
    }
    
    // 释放当前页表自身占用的物理页
    pmem_free((uint64)pgtbl, false);
}

// 页表销毁入口：单独处理trampoline和trapframe（特殊映射区域）
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    // 解除trampoline映射（共享资源，不释放物理页）
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    
    // 解除trapframe映射（进程私有资源，释放对应物理页）
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, true);
    
    // 从顶级页表开始递归销毁整个页表结构
    vm_recursive_destroy_pgtbl(pgtbl, 2);
}

// 拷贝用户页表（排除trampoline和trapframe特殊区域）
void uvm_copy_pgtbl(pgtbl_t src_pgtbl, pgtbl_t dst_pgtbl, uint64 heap_top, uint32 ustack_page_count, mmap_region_t* mmap_head)
{
    /* 步骤1：拷贝代码段、数据段、堆区域（PGSIZE ~ heap_top） */
    // 跳过最低地址的空白保护页，从有效用户空间起始地址开始拷贝
    vm_copy_virtual_range(src_pgtbl, dst_pgtbl, PGSIZE, heap_top);

    /* 步骤2：拷贝用户栈区域 */
    uint64 ustack_start_va = TRAPFRAME - ustack_page_count * PGSIZE;
    vm_copy_virtual_range(src_pgtbl, dst_pgtbl, ustack_start_va, TRAPFRAME);

    /* 步骤3：拷贝已映射的mmap区域 */
    // 定义MMAP区域的地址范围（与用户态内存布局保持一致）
    #define MMAP_VIRTUAL_END     (VA_MAX - 34 * PGSIZE)
    #define MMAP_VIRTUAL_BEGIN   (MMAP_VIRTUAL_END - 8096 * PGSIZE)
    
    for (uint64 curr_va = MMAP_VIRTUAL_BEGIN; curr_va < MMAP_VIRTUAL_END; curr_va += PGSIZE) {
        pte_t* pte_entry = vm_getpte(src_pgtbl, curr_va, false);
        // 仅拷贝已建立有效映射的页面
        if (pte_entry != NULL && (*pte_entry & PTE_V)) {
            uint64 src_phy_addr = (uint64)PTE_TO_PA(*pte_entry);
            int page_access_flags = (int)PTE_FLAGS(*pte_entry);
            
            uint64 new_phy_page = (uint64)pmem_alloc(false);
            if (new_phy_page == 0) {
                panic("uvm_copy_pgtbl: insufficient physical memory for mmap region copy");
            }
            memmove((char*)new_phy_page, (const char*)src_phy_addr, PGSIZE);
            vm_mappages(dst_pgtbl, curr_va, new_phy_page, PGSIZE, page_access_flags);
        }
    }
    
    #undef MMAP_VIRTUAL_END
    #undef MMAP_VIRTUAL_BEGIN
}

// 新增用户内存映射区域，从空闲mmap区域中分割并建立物理映射
void uvm_mmap(uint64 region_start, uint32 page_count, int access_perm)
{
    if(page_count == 0) return;
    assert(region_start % PGSIZE == 0, "uvm_mmap: region start address not page-aligned");

    proc_t* curr_proc = myproc();
    uint64 region_length = page_count * PGSIZE;
    
    // 遍历mmap空闲链表，寻找可容纳请求区域的空闲块
    mmap_region_t* prev_region = NULL;
    mmap_region_t* curr_region = curr_proc->mmap;
    
    while (curr_region != NULL) {
        uint64 curr_region_end = curr_region->begin + curr_region->npages * PGSIZE;
        
        // 检查请求区域是否完全包含在当前空闲区域内
        if (region_start >= curr_region->begin && region_start + region_length <= curr_region_end) {
            // 找到匹配的空闲区域，执行分割操作
            if (region_start == curr_region->begin && region_length == curr_region->npages * PGSIZE) {
                // 情况1：完全匹配，移除整个空闲区域
                if (prev_region == NULL) {
                    curr_proc->mmap = curr_region->next;
                } else {
                    prev_region->next = curr_region->next;
                }
                mmap_region_free(curr_region);
            } else if (region_start == curr_region->begin) {
                // 情况2：从空闲区域头部分割
                curr_region->begin += region_length;
                curr_region->npages -= page_count;
            } else if (region_start + region_length == curr_region_end) {
                // 情况3：从空闲区域尾部分割
                curr_region->npages -= page_count;
            } else {
                // 情况4：从空闲区域中间分割，创建新的空闲子区域
                mmap_region_t* new_free_region = mmap_region_alloc();
                new_free_region->begin = region_start + region_length;
                new_free_region->npages = (curr_region_end - region_start - region_length) / PGSIZE;
                new_free_region->next = curr_region->next;
                curr_region->npages = (region_start - curr_region->begin) / PGSIZE;
                curr_region->next = new_free_region;
            }
            break;
        }
        prev_region = curr_region;
        curr_region = curr_region->next;
    }
    
    // 为请求区域分配物理页并建立虚拟地址映射
    for (uint32 i = 0; i < page_count; i++) {
        uint64 curr_va = region_start + i * PGSIZE;
        uint64 new_phy_page = (uint64)pmem_alloc(false);
        if (new_phy_page == 0) {
            panic("uvm_mmap: insufficient physical memory for mapping");
        }
        memset((void*)new_phy_page, 0, PGSIZE);
        vm_mappages(curr_proc->pgtbl, curr_va, new_phy_page, PGSIZE, access_perm | PTE_U);
    }
}

// 释放用户内存映射区域，归还到mmap空闲链表并合并相邻区域
void uvm_munmap(uint64 region_start, uint32 page_count)
{
    if(page_count == 0) return;
    assert(region_start % PGSIZE == 0, "uvm_munmap: region start address not page-aligned");

    proc_t* curr_proc = myproc();
    uint64 region_length = page_count * PGSIZE;
    
    // 创建新的空闲内存映射区域
    mmap_region_t* new_free_region = mmap_region_alloc();
    new_free_region->begin = region_start;
    new_free_region->npages = page_count;
    new_free_region->next = NULL;
    
    // 按地址升序将新空闲区域插入到mmap链表中
    if (curr_proc->mmap == NULL || region_start < curr_proc->mmap->begin) {
        // 插入到链表头部
        new_free_region->next = curr_proc->mmap;
        curr_proc->mmap = new_free_region;
    } else {
        // 查找合适的插入位置
        mmap_region_t* curr_region = curr_proc->mmap;
        while (curr_region->next != NULL && curr_region->next->begin < region_start) {
            curr_region = curr_region->next;
        }
        new_free_region->next = curr_region->next;
        curr_region->next = new_free_region;
    }
    
    // 尝试与后继空闲区域合并
    if (new_free_region->next != NULL && 
        new_free_region->begin + new_free_region->npages * PGSIZE == new_free_region->next->begin) {
        mmap_region_t* successor_region = new_free_region->next;
        new_free_region->next = successor_region->next;
        vm_merge_mmap_regions(new_free_region, successor_region, true);
    }
    
    // 尝试与前驱空闲区域合并
    mmap_region_t* prev_region = NULL;
    mmap_region_t* curr_region = curr_proc->mmap;
    while (curr_region != new_free_region && curr_region != NULL) {
        prev_region = curr_region;
        curr_region = curr_region->next;
    }
    if (prev_region != NULL && 
        prev_region->begin + prev_region->npages * PGSIZE == new_free_region->begin) {
        prev_region->next = new_free_region->next;
        vm_merge_mmap_regions(prev_region, new_free_region, true);
    }
    
    // 解除虚拟地址映射并释放对应的物理页
    vm_unmappages(curr_proc->pgtbl, region_start, region_length, true);
}

// 扩展用户堆空间，返回新的堆顶地址（不更新进程堆顶字段）
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 current_heap_top, uint32 grow_length)
{
    uint64 new_heap_top = current_heap_top + grow_length;
    
    // 计算需要新增映射的页面范围（按页对齐）
    uint64 old_aligned_heap = PG_ROUND_UP(current_heap_top);
    uint64 new_aligned_heap = PG_ROUND_UP(new_heap_top);
    
    // 为新增区域分配物理页并建立虚拟映射
    for (uint64 curr_va = old_aligned_heap; curr_va < new_aligned_heap; curr_va += PGSIZE) {
        uint64 new_phy_page = (uint64)pmem_alloc(false);
        if (new_phy_page == 0) {
            panic("uvm_heap_grow: insufficient physical memory for heap expansion");
        }
        memset((void*)new_phy_page, 0, PGSIZE);
        vm_mappages(pgtbl, curr_va, new_phy_page, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    
    return new_heap_top;
}

// 收缩用户堆空间，返回新的堆顶地址（不更新进程堆顶字段）
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 current_heap_top, uint32 shrink_length)
{
    uint64 new_heap_top = current_heap_top - shrink_length;
    
    // 计算需要释放的页面范围（按页对齐）
    uint64 old_aligned_heap = PG_ROUND_UP(current_heap_top);
    uint64 new_aligned_heap = PG_ROUND_UP(new_heap_top);
    
    // 释放不再使用的物理页并解除虚拟映射
    if (new_aligned_heap < old_aligned_heap) {
        uint64 free_page_count = (old_aligned_heap - new_aligned_heap) / PGSIZE;
        vm_unmappages(pgtbl, new_aligned_heap, free_page_count * PGSIZE, true);
    }
    
    return new_heap_top;
}

// 用户态地址空间拷贝到内核态地址空间（支持非页对齐地址）
void uvm_copyin(pgtbl_t pgtbl, uint64 kernel_dst, uint64 user_src, uint32 copy_length)
{
    uint64 copy_bytes, curr_va, curr_pa;
    
    while (copy_length > 0) {
        // 获取当前用户态地址所在页的起始地址
        curr_va = PG_ROUND_DOWN(user_src);
        
        // 通过页表查找对应的物理地址
        pte_t* pte_entry = vm_getpte(pgtbl, curr_va, false);
        if (pte_entry == NULL || !(*pte_entry & PTE_V)) {
            panic("uvm_copyin: invalid or unallocated user page");
        }
        curr_pa = PTE_TO_PA(*pte_entry);
        
        // 计算当前页内可拷贝的最大字节数
        copy_bytes = PGSIZE - (user_src - curr_va);
        if (copy_bytes > copy_length) copy_bytes = copy_length;
        
        // 执行内存拷贝（物理地址偏移 -> 内核地址）
        memmove((void*)kernel_dst, (void*)(curr_pa + (user_src - curr_va)), copy_bytes);
        
        // 更新剩余拷贝参数
        copy_length -= copy_bytes;
        kernel_dst += copy_bytes;
        user_src = curr_va + PGSIZE;
    }
}

// 内核态地址空间拷贝到用户态地址空间（支持非页对齐地址）
void uvm_copyout(pgtbl_t pgtbl, uint64 user_dst, uint64 kernel_src, uint32 copy_length)
{
    uint64 copy_bytes, curr_va, curr_pa;
    
    while (copy_length > 0) {
        // 获取当前用户态地址所在页的起始地址
        curr_va = PG_ROUND_DOWN(user_dst);
        
        // 通过页表查找对应的物理地址
        pte_t* pte_entry = vm_getpte(pgtbl, curr_va, false);
        if (pte_entry == NULL || !(*pte_entry & PTE_V)) {
            panic("uvm_copyout: invalid or unallocated user page");
        }
        curr_pa = PTE_TO_PA(*pte_entry);
        
        // 计算当前页内可拷贝的最大字节数
        copy_bytes = PGSIZE - (user_dst - curr_va);
        if (copy_bytes > copy_length) copy_bytes = copy_length;
        
        // 执行内存拷贝（内核地址 -> 物理地址偏移）
        memmove((void*)(curr_pa + (user_dst - curr_va)), (void*)kernel_src, copy_bytes);
        
        // 更新剩余拷贝参数
        copy_length -= copy_bytes;
        kernel_src += copy_bytes;
        user_dst = curr_va + PGSIZE;
    }
}

// 用户态字符串拷贝到内核态（支持非页对齐，遇'\0'终止或达到最大长度）
void uvm_copyin_str(pgtbl_t pgtbl, uint64 kernel_dst, uint64 user_src, uint32 max_copy_len)
{
    uint64 copy_bytes, curr_va, curr_pa;
    bool null_terminator_found = false;
    
    while (!null_terminator_found && max_copy_len > 0) {
        // 获取当前用户态地址所在页的起始地址
        curr_va = PG_ROUND_DOWN(user_src);
        
        // 通过页表查找对应的物理地址
        pte_t* pte_entry = vm_getpte(pgtbl, curr_va, false);
        if (pte_entry == NULL || !(*pte_entry & PTE_V)) {
            panic("uvm_copyin_str: invalid or unallocated user page");
        }
        curr_pa = PTE_TO_PA(*pte_entry);
        
        // 计算当前页内可处理的最大字节数
        copy_bytes = PGSIZE - (user_src - curr_va);
        if (copy_bytes > max_copy_len) copy_bytes = max_copy_len;
        
        // 逐字节拷贝，遇到空终止符则停止
        char* phy_addr_ptr = (char*)(curr_pa + (user_src - curr_va));
        while (copy_bytes > 0) {
            if (*phy_addr_ptr == '\0') {
                *(char*)kernel_dst = '\0';
                null_terminator_found = true;
                break;
            }
            *(char*)kernel_dst = *phy_addr_ptr;
            copy_bytes--;
            max_copy_len--;
            phy_addr_ptr++;
            kernel_dst++;
        }
        
        // 切换到下一个页面继续处理
        user_src = curr_va + PGSIZE;
    }
}