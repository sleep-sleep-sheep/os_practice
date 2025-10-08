#include "mem/vmem.h"
#include "lib/print.h"
#include "mem/str.h"
#include "lib/lock.h"


// 增加调试用宏，用于打印各级VPN
#define VA_PRINT_VPNS(va) \
    printf("VPNs for 0x%lx: VPN2=0x%lx, VPN1=0x%lx, VPN0=0x%lx\n", \
           (va), VA_TO_VPN(va, 2), VA_TO_VPN(va, 1), VA_TO_VPN(va, 0))

//打印页表内容，调试用
// 辅助宏：计算不同级别页表对应的虚拟地址偏移（Sv39 标准）
// L2（顶级）：VPN[2] 占 9 位，偏移 30 位（9+9+12）
// L1（中级）：VPN[1] 占 9 位，偏移 21 位（9+12）
// L0（低级）：VPN[0] 占 9 位，偏移 12 位（12）
#define VA_SHIFT_L2 30
#define VA_SHIFT_L1 21
#define VA_SHIFT_L0 12

// 递归打印页表（level：0=低级，1=中级，2=顶级；va_prefix：当前层级的虚拟地址前缀）
static void vm_print_recursive(pgtbl_t pgtbl, int level, uint64 va_prefix) {
    if (level < 0 || level > 2) return;  // 只处理 0-2 级

    for (int i = 0; i < 512; i++) {  // 每个页表有 512 个条目
        pte_t pte = pgtbl[i];
        if (!(pte & PTE_V)) continue;  // 跳过无效页表项

        uint64 va;  // 当前条目对应的虚拟地址前缀
        uint64 pa = PTE_TO_PA(pte);    // 物理地址（下一级页表或物理页）

        // 根据当前级别计算虚拟地址前缀
        switch (level) {
            case 2: va = va_prefix | ((uint64)i << VA_SHIFT_L2); break;
            case 1: va = va_prefix | ((uint64)i << VA_SHIFT_L1); break;
            case 0: va = va_prefix | ((uint64)i << VA_SHIFT_L0); break;
            default: va = 0;
        }

        // 打印当前页表项信息
        printf("  level %d pte[%d]: va=0x%lx pa=0x%lx flags=0x%lx\n",
               level, i, va, pa, PTE_FLAGS(pte));

        // 若当前页表项指向的是下一级页表（而非物理页，即无 R/W/X 权限），则递归打印
        if (!(pte & (PTE_R | PTE_W | PTE_X))) {
            vm_print_recursive((pgtbl_t)pa, level - 1, va);
        }
    }
}

// 外部调用接口：从顶级页表（level 2）开始打印，初始虚拟地址前缀为 0
void vm_print(pgtbl_t pgtbl) {
    if (!pgtbl) {
        printf("vm_print: null pagetable\n");
        return;
    }

    printf("vm_print: pagetable at %p\n", pgtbl);
    vm_print_recursive(pgtbl, 2, 0);  // 从顶级页表（level 2）开始递归
}
// 获取内核虚拟地址对应的页表项（仅用于内核地址空间）
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc) {
    // 检查内核虚拟地址合法性：必须属于内核地址空间（高17位全为1）
    // 内核地址范围：[KERNEL_BASE, 0xFFFFFFFFFFFFFFFF]，高17位为0x1FFFF（全1）
 /*
  printf("vm_get_kpte:  kernel va 0x%lx \n", va);
    if ((va >> (64 - 17)) != 0x1FFFF) {  // 高17位检查
        printf("vm_get_kpte: invalid kernel va 0x%lx (not in kernel space)\n", va);
        return NULL;
    }

 
 */
    
    pte_t *pte = &pgtbl[VA_TO_VPN(va, 2)];  // 从顶级页表开始（假设3级页表，Sv39/Sv48）

    for (int level = 2; level > 0; level--) {
        if (!(*pte & PTE_V)) {
            // 若需要分配且当前页表项无效，则分配新页表页
            if (!alloc) {
                printf("  Level %d: pte not valid and not allocating\n", level);
                return NULL;
            }

            void *pa = pmem_alloc(true);  // 分配内核页表页（物理地址）
            if (!pa) {
                printf("  Level %d: failed to allocate page table\n", level);
                return NULL;
            }

            memset(pa, 0, PGSIZE);        // 清零新页表页
            *pte = PA_TO_PTE(pa) | PTE_V | PTE_G;  // 内核页表项设为全局（PTE_G）
            printf("  Level %d: created new kernel page table at 0x%lx, pte=0x%lx\n",
                   level, (uint64)pa, *pte);
        }

        pgtbl_t next_pgtbl = (pgtbl_t)PTE_TO_PA(*pte);  // 下一级页表的物理地址
        uint64 vpn = VA_TO_VPN(va, level - 1);          // 当前级别的VPN（虚拟页号）
        pte = &next_pgtbl[vpn];                         // 移动到下一级页表项
    }

    return pte;  // 返回最终的页表项指针（指向物理页）
}
    


//建立虚拟地址到物理地址的映射
void vm_mappages(pgtbl_t pgtbl ,uint64 va , uint64 pa, uint64 len, int perm){
   //检查地址对齐
   if(va%PGSIZE !=0 || pa%PGSIZE !=0 || len%PGSIZE !=0){
       panic("vm_mappages: addresses or length not page-aligned");
   }

   //检查权限的合法性
    if ((perm & ~(PTE_R | PTE_W | PTE_X | PTE_U | PTE_G)) != 0) {
        panic("vm_mappages: invalid permissions");
    }

    for(uint64 i=0;i<len;i+=PGSIZE){
        pte_t *pte = vm_getpte(pgtbl, va + i, true); //获取或创建页表项
        if(!pte){
            panic("vm_mappages: vm_getpte failed");
        }

       
       if (*pte & PTE_V) {
          //  panic("vm_mappages: va 0x%lx already mapped", va + i);
          printf ("vm_mappages: va 0x%lx already mapped, overwriting\n", va + i);
        }
        
        // 设置页表项：物理地址 + 权限 + 有效位
        *pte = PA_TO_PTE(pa + i) | perm | PTE_V;
    }
}



//取消虚拟地址到物理地址的映射
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit){
     // 检查地址对齐
    if (va % PGSIZE != 0 || len % PGSIZE != 0) {
        panic("vm_unmappages: addresses or length not page-aligned");
    }

    //解除每一页的映射
    for(uint64 i=0;i<len;i+=PGSIZE){
        pte_t *pte = vm_getpte(pgtbl,va+i,false); //获取页表项，不创建
        if(!pte || !(*pte &PTE_V)){
         //   panic("vm_unmappages: va 0x%lx not mapped", va + i);
        }
        if(freeit){
            char *kernel_mem_end =(char*) PG_ROUND_DOWN((uint64)end) + KERNEL_PAGES * PGSIZE;
            uint64 pa =PTE_TO_PA(*pte);
            bool in_kernel= (char*) pa < kernel_mem_end;
            pmem_free((void*)pa,in_kernel); //释放物理页
        }

        *pte=0; //清除页表项
    }
}



//根据页表查找虚拟地址对应的物理地址
uint64 vm_walkaddr(pgtbl_t pgtbl, uint64 va){
   if(va> VA_MAX){
       //printf("vm_walkaddr: invalid va %p\n", va);
       return 0;
   }

   pte_t *pte =vm_getpte(pgtbl,va,false); //获取页表项，不创建
   if(!pte || !(*pte & PTE_V)){
         return 0; //无效或未映射
   }

   if(PTE_IS_TABLE(*pte)){
       //printf("vm_walkaddr: va %p is a table\n", va);
       return 0; //页表项指向下一级页表，不是叶子
   }

   // 返回物理地址：页基址+ 偏移地址
   return PTE_TO_PA(*pte)+(va & (PGSIZE -1)); //返回物理地址
}
    


//创建新的页表(内核页表，它是创建一个顶级页表结构（用于管理虚拟地址映射）)
pgtbl_t vm_create(){
    pgtbl_t pgtbl = (pgtbl_t) pmem_alloc(true); //分配内核页表页
    if(pgtbl){
        memset(pgtbl,0,PGSIZE); //清零页表
    }
    return pgtbl;
}



//释放整个页表及其映射的物理页
static void vm_free_pgtbl(pgtbl_t pgtbl ,int level){
   if(!pgtbl) return;

   if(level<2){
    for(int i=0;i<512;i++){
        pte_t pte = pgtbl[i];
        if((pte & PTE_V) && PTE_IS_TABLE(pte)){
            pgtbl_t child= (pgtbl_t) PTE_TO_PA(pte);
            vm_free_pgtbl(child,level+1); //递归释放下一级页表
        }
    }
   }
     // 释放当前页表
    pmem_free(pgtbl, true);
}

// 释放页表及其所有映射
void vm_free(pgtbl_t pgtbl) {
    if (pgtbl) {
        vm_free_pgtbl(pgtbl, 0);
    }
}
