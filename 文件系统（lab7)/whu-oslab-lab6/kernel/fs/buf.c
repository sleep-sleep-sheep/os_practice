#include "fs/buf.h"
#include "dev/vio.h"
#include "lib/lock.h"
#include "lib/print.h"
#include "lib/str.h"

#define N_BLOCK_BUF 64
#define BLOCK_NUM_UNUSED 0xFFFFFFFF

// 将buf包装成双向循环链表的node（用于管理缓冲区的链表结构）
typedef struct buf_node {
    buf_t buf;                // 核心缓冲区数据（作为第一个成员，支持直接强制类型转换）
    struct buf_node* next;    // 链表后继节点指针
    struct buf_node* prev;    // 链表前驱节点指针
} buf_node_t;

// 全局buf cache管理变量
static buf_node_t buf_cache[N_BLOCK_BUF];  // 固定大小的缓冲区数组（共64个缓冲区）
static buf_node_t head_buf;                // 双向循环链表哨兵头节点：->next 已分配/最近使用 | ->prev 空闲/最少使用（LRU）
static spinlock_t lk_buf_cache;            // 保护链表结构、buf_ref、block_num的自旋锁

// 【内部辅助函数】双向循环链表节点迁移/插入（从原位置移除，插入到head的指定侧）
// head_next=true：插入到head->next（已分配/最近使用侧）
// head_next=false：插入到head->prev（空闲/最少使用侧）
static void insert_head(buf_node_t* buf_node, bool head_next)
{
    // 第一步：将节点从原有链表位置中移除（如果节点已在链表中）
    if (buf_node->next != NULL && buf_node->prev != NULL) {
        buf_node->next->prev = buf_node->prev;  // 后继节点的前驱指向当前节点的前驱
        buf_node->prev->next = buf_node->next;  // 前驱节点的后继指向当前节点的后继
    }

    // 第二步：将节点插入到哨兵头节点的指定侧
    if (head_next) {
        // 插入到head->next（已分配/最近使用侧，链表头部）
        buf_node->prev = &head_buf;
        buf_node->next = head_buf.next;
        head_buf.next->prev = buf_node;
        head_buf.next = buf_node;
    } else {
        // 插入到head->prev（空闲/最少使用侧，链表尾部，符合LRU策略）
        buf_node->next = &head_buf;
        buf_node->prev = head_buf.prev;
        head_buf.prev->next = buf_node;
        head_buf.prev = buf_node;
    }
}

// 【对外接口】buf cache初始化（必须在使用其他buf接口前调用）
void buf_init()
{
    // 1. 初始化buf cache全局自旋锁（保护链表和引用计数）
    spinlock_init(&lk_buf_cache, "buf_cache");

    // 2. 初始化哨兵头节点，构建空的双向循环链表
    head_buf.next = &head_buf;
    head_buf.prev = &head_buf;

    // 3. 遍历初始化所有缓冲区节点，加入空闲链表（head->prev侧）
    for (int i = 0; i < N_BLOCK_BUF; i++) {
        buf_node_t* curr_node = &buf_cache[i];
        buf_t* curr_buf = &curr_node->buf;

        // 3.1 初始化缓冲区核心字段（空闲状态）
        curr_buf->block_num = BLOCK_NUM_UNUSED;  // 未绑定任何磁盘块
        curr_buf->buf_ref = 0;                   // 初始无引用
        curr_buf->disk = false;                  // 初始未与磁盘同步（清零，避免脏数据）
        memset(curr_buf->data, 0, BLOCK_SIZE);   // 缓存数据区域清零

        // 3.2 初始化缓冲区睡眠锁（保护data数据和磁盘操作）
        sleeplock_init(&curr_buf->slk, "buf_sleeplock");

        // 3.3 初始化链表节点指针（初始为NULL，标记未加入链表）
        curr_node->next = NULL;
        curr_node->prev = NULL;

        // 3.4 将节点插入到空闲链表（head->prev侧，符合LRU策略）
        insert_head(curr_node, false);
    }
}

// 【对外接口】读取指定磁盘块到缓冲区（合并xv6 bget()逻辑，支持LRU缓存）
// 功能：优先查找缓存，未缓存则分配空闲缓冲区从磁盘读取，无空闲则panic
buf_t* buf_read(uint32 block_num)
{
    // 入参合法性校验：无效磁盘块编号直接报错
    if (block_num == BLOCK_NUM_UNUSED) {
        panic("buf_read: invalid block number (BLOCK_NUM_UNUSED)");
    }

    buf_node_t* target_node = NULL;

    // 第一步：获取自旋锁，保护链表遍历和引用计数操作
    spinlock_acquire(&lk_buf_cache);

    // 第二步：遍历已分配链表（head->next侧），查找是否已缓存该磁盘块
    for (target_node = head_buf.next; target_node != &head_buf; target_node = target_node->next) {
        if (target_node->buf.block_num == block_num) {
            // 找到已缓存的缓冲区，增加引用计数（标记新增一个使用者）
            target_node->buf.buf_ref++;

            // 释放自旋锁（链表操作完成，后续操作不涉及链表）
            spinlock_release(&lk_buf_cache);

            // 第三步：获取缓冲区睡眠锁，保护核心数据访问（排他性，确保数据安全）
            sleeplock_acquire(&target_node->buf.slk);

            // 直接返回已缓存的缓冲区（无需从磁盘读取，提升效率）
            return &target_node->buf;
        }
    }

    // 第三步：未找到缓存，遍历空闲链表（head->prev侧，LRU策略：从最少使用侧查找空闲）
    for (target_node = head_buf.prev; target_node != &head_buf; target_node = target_node->prev) {
        if (target_node->buf.buf_ref == 0) {  // 引用计数为0，表示无进程使用，为空闲缓冲区
            // 初始化空闲缓冲区，绑定目标磁盘块
            target_node->buf.block_num = block_num;
            target_node->buf.buf_ref = 1;  // 引用计数置1（当前调用者为第一个使用者）
            target_node->buf.disk = false; // 重置磁盘同步标志（准备从磁盘读取数据）

            // 释放自旋锁（链表操作完成，后续操作不涉及链表）
            spinlock_release(&lk_buf_cache);

            // 第四步：获取缓冲区睡眠锁，保护磁盘I/O和数据访问
            sleeplock_acquire(&target_node->buf.slk);

            // 第五步：调用虚拟磁盘驱动，从磁盘读取数据到缓冲区（false=读操作）
            virtio_disk_rw(&target_node->buf, false);

            // 标记缓冲区已与磁盘同步，返回缓冲区指针
            target_node->buf.disk = true;
            return &target_node->buf;
        }
    }

    // 第四步：无空闲缓冲区可用，直接panic报错（缓冲区耗尽）
    spinlock_release(&lk_buf_cache);  // 释放自旋锁，避免死锁
    panic("buf_read: no free buf available (all  bufs are in use)");
    return NULL;  // 不可达，仅用于满足函数返回值要求
}

// 【对外接口】将缓冲区数据写入磁盘（强制同步，保证内存与磁盘数据一致）
void buf_write(buf_t* buf)
{
    // 入参合法性校验：缓冲区指针不能为空
    if (buf == NULL) {
        panic("buf_write: invalid NULL buf pointer");
    }

    // 断言：验证调用者是否持有缓冲区睡眠锁（确保数据访问安全，防止非法写入）
    assert(sleeplock_holding(&buf->slk), "buf_write: not holding buf sleeplock (illegal write)");

    // 调用虚拟磁盘驱动，将缓冲区数据写入磁盘（true=写操作）
    virtio_disk_rw(buf, true);

    // 标记缓冲区已与磁盘同步，避免重复写入
    buf->disk = true;
}

// 【对外接口】释放缓冲区引用（减少引用计数，支持LRU缓存策略）
void buf_release(buf_t* buf)
{
    // 入参合法性校验：缓冲区指针不能为空
    if (buf == NULL) {
        panic("buf_release: invalid NULL buf pointer");
    }

    // 断言：验证调用者是否持有缓冲区睡眠锁（确保合法释放，防止非法操作）
    assert(sleeplock_holding(&buf->slk), "buf_release: not holding buf sleeplock (illegal release)");

    // 第一步：释放缓冲区睡眠锁（表示调用者不再操作核心数据）
    sleeplock_release(&buf->slk);

    // 第二步：将buf_t*转换为buf_node_t*（利用buf是buf_node_t第一个成员的特性，安全转换）
    buf_node_t* target_node = (buf_node_t*)buf;

    // 第三步：获取自旋锁，保护引用计数修改和链表操作
    spinlock_acquire(&lk_buf_cache);

    // 第四步：减少引用计数（标记调用者已释放该缓冲区）
    assert(target_node->buf.buf_ref > 0, "buf_release: buf ref count is zero (double release)");
    target_node->buf.buf_ref--;

    // 第五步：如果引用计数归0（无任何进程使用），执行LRU策略：迁移到最少使用侧（head->prev）
    if (target_node->buf.buf_ref == 0) {
        insert_head(target_node, false);  // 插入到head->prev，标记为空闲/最少使用
    }

    // 第六步：释放自旋锁（链表和引用计数操作完成）
    spinlock_release(&lk_buf_cache);
}

// 【对外接口】打印当前buf cache的状态（用于调试，查看缓冲区使用情况）
void buf_print()
{
    printf("\n===================== buf_cache status =====================\n");
    printf("Total bufs: %d\n", N_BLOCK_BUF);
    printf("Format: buf [index] | ref [count] | block [num] | data [first 8 bytes]\n\n");

    buf_node_t* curr_node = NULL;

    // 获取自旋锁，保护链表遍历（避免遍历过程中链表结构被修改）
    spinlock_acquire(&lk_buf_cache);

    // 遍历已分配链表（head->next侧），打印所有缓冲区状态
    for (curr_node = head_buf.next; curr_node != &head_buf; curr_node = curr_node->next) {
        buf_t* curr_buf = &curr_node->buf;
        int buf_index = (int)(curr_node - buf_cache);  // 计算缓冲区在数组中的索引

        // 打印缓冲区核心信息
        printf("buf [%2d] | ref [%2d] | block [0x%08X] | data [",
               buf_index, curr_buf->buf_ref, curr_buf->block_num);

        // 打印缓冲区数据的前8个字节（用于调试，查看数据内容）
        for (int i = 0; i < 8; i++) {
            printf("%02X ", curr_buf->data[i]);
        }
        printf("]\n");
    }

    // 释放自旋锁（遍历完成）
    spinlock_release(&lk_buf_cache);

    printf("=============================================================\n\n");
}