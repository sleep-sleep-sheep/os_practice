#include "mem/pmem.h"
#include "mem/str.h"
#include "memlayout.h"
#include "lib/lock.h"
#include "lib/print.h"



extern char end[]; //内核结束地址，由链接器提供

struct {
    struct spinlock lock; //保护空闲链表的自旋锁
    struct run *freelist_kernel; //内核空闲链表头指针
    struct run *freelist_user;   //用户空闲链表头指针
    uint64 kernel_pages_used; //内核已用物理页数
    uint64 user_pages_used;   //用户已用物理页数
} kmem;
// 初始化物理内存分配器
void pmem_init(){
    spinlock_init(&kmem.lock, "kpmem");
    printf("pmem_init: initializing physical memory allocator\n");
    char *pa_start = (char*)PG_ROUND_UP((uint64)end); //内核结束地址对齐到下一页
    char *pa_end =(char* ) PHYS_TOP; //物理内存顶端地址
    uint64 total_pages = (pa_end - pa_start) / PGSIZE; //总物理页数

    //计算内核和用户内存区域的边界
    char *kernel_mem_end =pa_start + KERNEL_PAGES * PGSIZE; //内核内存区结束地址

    //确保内核区域不会超出物理内存范围
    if(kernel_mem_end > pa_end)
        kernel_mem_end = pa_end;

       // 初始化内核空闲页链表
    for (char *p = pa_start; p + PGSIZE <= kernel_mem_end; p += PGSIZE) {
        // 跳过设备地址区域
    /*   if ((uint64)p >= DEVBASE && (uint64)p < DEVBASE + DEVSIZE) {
            continue;
        }

    */
        pmem_free(p, true);
    }
    
    // 初始化用户空闲页链表
    for (char *p = kernel_mem_end; p + PGSIZE <= pa_end; p += PGSIZE) {
        // 跳过设备地址区域
    /*
        if ((uint64)p >= DEVBASE && (uint64)p < DEVBASE + DEVSIZE) {
            continue;
        }
    */
        pmem_free(p, false);
    }

   
    printf("pmem_init: kernel memory from %p to %p\n", pa_start, kernel_mem_end);
    printf("pmem_init: user memory from %p to %p\n", kernel_mem_end, pa_end);
    printf("pmem_init: total pages %d, kernel pages %d, user pages %d\n",
           total_pages,
           (kernel_mem_end - pa_start)/PGSIZE,
           (pa_end - kernel_mem_end)/PGSIZE);


}


//分配物理页

void * pmem_alloc(bool in_kernel){
    struct run* r=NULL;

    spinlock_acquire(&kmem.lock);

    //根据不同的链表进行分配

    if(in_kernel){
        r= kmem.freelist_kernel;
        if(r){
            kmem.freelist_kernel = r->next;
            kmem.kernel_pages_used++;
        }

    }  else{
        r= kmem.freelist_user;
        if(r){
            kmem.freelist_user = r->next;
            kmem.user_pages_used++;
        }
    }
 
    spinlock_release(&kmem.lock);
    
    if(in_kernel && !r) //内核内存分配失败，引发恐慌，因为内核申请必须实现
    {
        printf("pmem_alloc: kernel out of memory\n");
    }

    return (void*)r; //返回分配的物理页地址，可能为NULL

}

//释放物理页
void pmem_free(void *pa , bool in_kernel){
    struct run *r =(struct run *) pa;

    // 合法性检查
    if (
        ((uint64)pa % PGSIZE) != 0 ||                  // 地址必须页对齐
        (char *)pa < (char *)PG_ROUND_UP((uint64)end) ||  // 不能释放内核代码数据区
        (uint64)pa >= PHYS_TOP                         // 不能释放超出物理内存上限的页
       //  || ((uint64)pa >= DEVBASE && (uint64)pa < DEVBASE + DEVSIZE)  // 不能释放设备地址
    ) {
        // panic("pmem_free: invalid physical address %p", pa);
        printf("pmem_free: invalid physical address %p", pa);
    }
     

    // 检查释放到正确的区域
    char *kernel_mem_end = (char *)PG_ROUND_UP((uint64)end) + KERNEL_PAGES * PGSIZE;
    if (in_kernel && (char *)pa >= kernel_mem_end) {
        // panic("pmem_free: trying to free user page as kernel page %p", pa);
        printf("pmem_free: trying to free user page as kernel page %p", pa);
    }
    if (!in_kernel && (char *)pa < kernel_mem_end) {
       //   panic("pmem_free: trying to free kernel page as user page %p", pa);
        printf("pmem_free: trying to free kernel page as user page %p", pa);
    }

    memset (pa, 0xaa,PGSIZE); //填充垃圾数据，帮助检测错误
    spinlock_acquire(&kmem.lock);

    if(in_kernel){
        r->next= kmem.freelist_kernel;
        kmem.freelist_kernel = r;  
        kmem.kernel_pages_used--;
    } else{
        r->next= kmem.freelist_user;
        kmem.freelist_user = r;
        kmem.user_pages_used--;
    }
    spinlock_release(&kmem.lock);
 
}

uint64 pmem_free_count(bool in_kernel){
    uint64 count=0;
   struct run*r;
   spinlock_acquire(&kmem.lock);
    if(in_kernel){
        for(r=kmem.freelist_kernel;r;r=r->next)
            count++;
    } else{
        for(r=kmem.freelist_user;r;r=r->next)
            count++;
    }

    spinlock_release(&kmem.lock);
    return count;
}
    