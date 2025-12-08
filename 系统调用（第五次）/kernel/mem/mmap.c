#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"
#include "memlayout.h"

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
void mmap_init() {
    spinlock_init(&list_lk, "mmap_list_lock");
    // 初始化空闲链表
    for (int i = 0; i < N_MMAP - 1; i++) {
        list_mmap_region_node[i].next = &list_mmap_region_node[i + 1];
        list_mmap_region_node[i].mmap.start = 0;
        list_mmap_region_node[i].mmap.len = 0;
        list_mmap_region_node[i].mmap.flags = 0;
    }
    list_mmap_region_node[N_MMAP - 1].next = NULL;
    list_head = &list_mmap_region_node[0];
    printf("mmap_init: mmap pool initialized (total %d nodes)\n", N_MMAP);
}

// 从仓库申请一个 mmap_region_t
mmap_region_t* mmap_region_alloc() {
    spinlock_acquire(&list_lk);
    if (list_head == NULL) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: no free nodes!");
    }
    // 取出头节点
    mmap_region_node_t* node = list_head;
    list_head = list_head->next;
    spinlock_release(&list_lk);
    
    // 清空节点数据
    node->mmap.start = 0;
    node->mmap.len = 0;
    node->mmap.flags = 0;
    node->next = NULL;
    printf("mmap_region_alloc: allocated node at index %d\n", node - list_mmap_region_node);
    return &node->mmap;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t* mmap) {
    if (mmap == NULL) return;
    spinlock_acquire(&list_lk);
    // 转换为node指针
    mmap_region_node_t* node = (mmap_region_node_t*)((char*)mmap - offsetof(mmap_region_node_t, mmap));
    // 插入到空闲链表头部
    node->next = list_head;
    list_head = node;
    // 清空数据
    node->mmap.start = 0;
    node->mmap.len = 0;
    node->mmap.flags = 0;
    spinlock_release(&list_lk);
    printf("mmap_region_free: freed node at index %d\n", node - list_mmap_region_node);
}

// 输出仓库里可用的 mmap_region_node_t
void mmap_show_mmaplist() {
    spinlock_acquire(&list_lk);
    
    mmap_region_node_t* tmp = list_head;
    int node = 1, index = 0;
    printf("mmap_show_mmaplist: free nodes list:\n");
    while (tmp) {
        index = tmp - list_mmap_region_node;
        printf("  node %d: index = %d\n", node++, index);
        tmp = tmp->next;
    }
    if (node == 1) {
        printf("  no free nodes\n");
    }

    spinlock_release(&list_lk);
}