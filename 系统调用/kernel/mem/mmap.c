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
    // 1. 初始化保护链表的自旋锁（设置锁名称，方便调试）
    spinlock_init(&list_lk, "mmap_list_lock");

    // 2. 初始化预分配对象池，将所有节点串联成单向链表
    for (int i = 0; i < N_MMAP - 1; i++) {
        list_mmap_region_node[i].next = &list_mmap_region_node[i + 1];
        // 可选：初始化mmap区域为默认无效状态，避免垃圾数据
        memset(&list_mmap_region_node[i].mmap, 0, sizeof(mmap_region_t));
    }
    // 3. 链表尾节点next置为NULL，标记链表结束
    list_mmap_region_node[N_MMAP - 1].next = NULL;
    memset(&list_mmap_region_node[N_MMAP - 1].mmap, 0, sizeof(mmap_region_t));

    // 4. 设置链表头指针，指向第一个预分配节点（list_head保留，不被申请）
    list_head = &list_mmap_region_node[0];
}

// 从仓库申请一个 mmap_region_t
// 若申请失败则 panic
// 注意: list_head 保留, 不会被申请出去
mmap_region_t* mmap_region_alloc()
{
    // 1. 加锁保护链表操作，确保多核并发安全
    spinlock_acquire(&list_lk);

    // 2. 检查链表是否为空（仅保留list_head，无可用节点）
    // 注意：list_head是链表头，申请的是list_head->next指向的第一个可用节点
    if (list_head == NULL || list_head->next == NULL) {
        spinlock_release(&list_lk); // 解锁后再panic，避免锁泄露
        panic("mmap_region_alloc: no available mmap region nodes (pool exhausted)");
    }

    // 3. 提取第一个可用节点（跳过list_head，符合要求）
    mmap_region_node_t* alloc_node = list_head->next;

    // 4. 更新链表头指针，移除已申请节点（链表断链重连）
    list_head->next = alloc_node->next;

    // 5. 解锁，释放自旋锁
    spinlock_release(&list_lk);

    // 6. 初始化申请到的节点（清空next，避免链表残留引用；清空mmap数据）
    alloc_node->next = NULL;
    memset(&alloc_node->mmap, 0, sizeof(mmap_region_t));

    // 7. 返回mmap_region_t指针（类型转换，对外暴露有效接口）
    return &alloc_node->mmap;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t* mmap)
{
    // 1. 合法性校验：避免空指针操作
    if (mmap == NULL) {
        panic("mmap_region_free: invalid NULL mmap pointer");
    }

    // 2. 从mmap_region_t反向获取包含它的mmap_region_node_t节点（利用结构体内存布局）
    // 核心：结构体中mmap是第一个成员，指针可以直接转换（偏移量为0）
    mmap_region_node_t* free_node = (mmap_region_node_t*)mmap;

    // 3. 额外校验：确保归还的节点属于预分配对象池（防止非法内存归还）
    if (free_node < &list_mmap_region_node[0] || free_node >= &list_mmap_region_node[N_MMAP]) {
        panic("mmap_region_free: invalid mmap node (not in preallocated pool)");
    }

    // 4. 加锁保护链表操作，确保多核并发安全
    spinlock_acquire(&list_lk);

    // 5. 初始化归还节点的mmap数据，避免残留敏感信息
    memset(&free_node->mmap, 0, sizeof(mmap_region_t));

    // 6. 将归还节点插入链表头部（list_head之后，最简单的链表插入方式）
    free_node->next = list_head->next;
    list_head->next = free_node;

    // 7. 解锁，释放自旋锁
    spinlock_release(&list_lk);
}

// 输出仓库里可用的 mmap_region_node_t
// for debug
void mmap_show_mmaplist()
{
    spinlock_acquire(&list_lk);
    
    mmap_region_node_t* tmp = list_head->next; // 跳过list_head，打印可用节点
    int node = 1, index = 0;
    while (tmp)
    {
        index = tmp - &list_mmap_region_node[0]; // 修正：计算相对于对象池起始的索引
        printf("Available node %d, pool index = %d\n", node++, index);
        tmp = tmp->next;
    }

    // 补充：打印可用节点总数，方便调试
    printf("Total available mmap nodes: %d\n", node - 1);

    spinlock_release(&list_lk);
}