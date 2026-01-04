#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"

// 包装 mmap_region_t 用于仓库组织
typedef struct mmap_region_node {
    mmap_region_t mmap;
    struct mmap_region_node* next;
} mmap_region_node_t;

#define N_MMAP 256

// mmap_region_node_t 仓库(单向链表) + 指向链表头节点的指针 + 保护仓库的锁
static mmap_region_node_t list_mmap_region_node[N_MMAP];
static mmap_region_node_t* list_head;
static spinlock_t list_lk;

// 初始化上述三个数据结构
void mmap_init()
{
    // 初始化自旋锁
    spinlock_init(&list_lk, "mmap_list");
    
    // list_mmap_region_node[0] 作为保留的头节点
    list_head = &list_mmap_region_node[0];
    list_head->next = NULL;
    
    // 将其余节点串成链表（头插法）
    for (int i = 1; i < N_MMAP; i++) {
        list_mmap_region_node[i].next = list_head->next;
        list_head->next = &list_mmap_region_node[i];
    }
}

// 从仓库申请一个 mmap_region_t
// 若申请失败则 panic
// 注意: list_head 保留, 不会被申请出去
mmap_region_t* mmap_region_alloc()
{
    spinlock_acquire(&list_lk);
    
    // 检查链表是否为空（跳过保留的头节点）
    if (list_head->next == NULL) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: no available mmap_region");
    }
    
    // 取出链表的第一个节点（头节点之后的节点）
    mmap_region_node_t* node = list_head->next;
    list_head->next = node->next;
    
    spinlock_release(&list_lk);
    
    // 初始化 mmap_region
    node->mmap.begin = 0;
    node->mmap.npages = 0;
    node->mmap.next = NULL;
    
    return &node->mmap;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t* mmap)
{
    if (mmap == NULL) return;
    
    // 通过 mmap 指针计算出包含它的 mmap_region_node_t 的地址
    // mmap 是 mmap_region_node_t 的第一个成员，所以它们地址相同
    mmap_region_node_t* node = (mmap_region_node_t*)mmap;
    
    spinlock_acquire(&list_lk);
    
    // 头插法归还到链表
    node->next = list_head->next;
    list_head->next = node;
    
    spinlock_release(&list_lk);
}

// 输出仓库里可用的 mmap_region_node_t
// for debug
void mmap_show_mmaplist()
{
    spinlock_acquire(&list_lk);
    
    mmap_region_node_t* tmp = list_head;
    int node = 1, index = 0;
    while (tmp)
    {
        index = tmp - list_head;
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}