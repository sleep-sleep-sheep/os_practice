#include "fs/buf.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/fs.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"

// 全局超级块（外部定义，来自文件系统初始化模块）
extern super_block_t sb;

// 内存中的inode缓存（icache）配置：最大32个inode同时缓存
#define N_INODE 32
static inode_t icache[N_INODE];    // 内存inode缓存数组
static spinlock_t lk_icache;       // 保护icache的自旋锁（引用计数、inode查找/分配）

// ---------------------- 基础初始化 ----------------------
/**
 * @brief 初始化inode缓存（icache），必须在文件系统初始化后调用
 */
void inode_init()
{
    // 1. 初始化icache全局自旋锁
    spinlock_init(&lk_icache, "icache");

    // 2. 遍历初始化所有内存inode的睡眠锁和默认字段
    for (int i = 0; i < N_INODE; i++) {
        inode_t* ip = &icache[i];
        // 初始化inode睡眠锁（保护元数据和有效性）
        sleeplock_init(&ip->slk, "inode");
        // 初始化默认字段（空闲状态）
        ip->inode_num = INODE_NUM_UNUSED;
        ip->ref = 0;
        ip->valid = false;
        ip->type = FT_UNUSED;
    }
}

// ---------------------- 与inode本身相关（元数据管理） ----------------------
/**
 * @brief 同步inode元数据（磁盘↔内存）
 * @param ip 内存inode指针
 * @param write true=内存→磁盘（写入更新），false=磁盘→内存（读取加载）
 * @note 调用者必须持有inode的睡眠锁（ip->slk）
 */
void inode_rw(inode_t* ip, bool write)
{
    // 1. 断言：验证调用者持有inode睡眠锁，保证数据安全
    assert(sleeplock_holding(&ip->slk), "inode_rw: not holding inode sleeplock");

    // 2. 计算该inode在磁盘上的存储块编号
    // 公式：inode区域起始块 + inode序号 / 每个块可存储的inode数量
    uint32 block_num = sb.inode_start + (ip->inode_num / INODE_PER_BLOCK);

    // 3. 调用buf层读取对应磁盘块
    buf_t* inode_buf = buf_read(block_num);
    assert(inode_buf != NULL, "inode_rw: read inode block failed");

    // 4. 计算该inode在磁盘块中的偏移量（每个inode固定64字节）
    uint8* disk_inode = inode_buf->data + (ip->inode_num % INODE_PER_BLOCK) * INODE_DISK_SIZE;

    // 5. 执行数据同步（内存↔磁盘）
    if (write) {
        // 内存inode元数据 → 磁盘（仅同步前64字节，对应INODE_DISK_SIZE）
        memmove(disk_inode, &ip->type, INODE_DISK_SIZE);
        // 强制写入磁盘，保证数据持久化
        buf_write(inode_buf);
    } else {
        // 磁盘 → 内存inode元数据（加载磁盘上的inode信息）
        memmove(&ip->type, disk_inode, INODE_DISK_SIZE);
    }

    // 6. 释放缓冲区引用，允许buf层回收复用
    buf_release(inode_buf);
}

/**
 * @brief 在icache中查询或分配内存inode
 * @param inode_num 目标inode序号
 * @return 内存inode指针（未上锁，引用计数+1）
 * @note 无空闲inode时panic报错
 */
inode_t* inode_alloc(uint16 inode_num)
{
    inode_t* free_inode = NULL;

    // 1. 获取icache自旋锁，保护缓存遍历和引用计数修改
    spinlock_acquire(&lk_icache);

    // 2. 遍历icache，查找已缓存的对应inode
    for (int i = 0; i < N_INODE; i++) {
        inode_t* ip = &icache[i];
        // 找到已缓存且被引用的inode（ref>0）
        if (ip->ref > 0 && ip->inode_num == inode_num) {
            // 引用计数+1，标记新增使用者
            ip->ref++;
            // 释放自旋锁，返回找到的inode
            spinlock_release(&lk_icache);
            return ip;
        }
        // 记录第一个空闲inode（ref=0，未被使用）
        if (free_inode == NULL && ip->ref == 0) {
            free_inode = ip;
        }
    }

    // 3. 未找到已缓存inode，分配空闲inode
    if (free_inode == NULL) {
        spinlock_release(&lk_icache);
        panic("inode_alloc: no free inode in icache");
    }

    // 4. 初始化空闲inode的核心字段
    free_inode->inode_num = inode_num;
    free_inode->ref = 1;          // 引用计数初始化为1
    free_inode->valid = false;    // 标记元数据无效，需后续从磁盘加载

    // 5. 释放自旋锁，返回新分配的inode
    spinlock_release(&lk_icache);
    return free_inode;
}

/**
 * @brief 在磁盘创建新inode，并在内存分配对应缓存
 * @param type inode类型（FT_DIR/FT_FILE/FT_DEVICE）
 * @param major 主设备号（设备文件使用）
 * @param minor 次设备号（设备文件使用）
 * @return 内存inode指针（未上锁，已完成磁盘初始化）
 */
inode_t* inode_create(uint16 type, uint16 major, uint16 minor)
{
    // 1. 在位图中分配空闲inode（磁盘层面）
    uint16 inode_num = bitmap_alloc_inode();
    assert(inode_num != INODE_NUM_UNUSED, "inode_create: alloc inode failed");

    // 2. 在内存icache中分配对应inode缓存
    inode_t* ip = inode_alloc(inode_num);
    assert(ip != NULL, "inode_create: alloc inode in icache failed");

    // 3. 上锁并初始化inode元数据
    inode_lock(ip);

    // 3.1 填充核心元数据
    ip->type = type;
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;    // 初始链接数为1
    ip->size = 0;     // 初始文件大小为0
    memset(ip->addrs, 0, sizeof(ip->addrs));  // 清空地址映射数组
    ip->valid = true; // 标记元数据有效

    // 3.2 将初始化后的元数据写入磁盘
    inode_rw(ip, true);

    // 3.3 特殊处理：目录类型inode，分配初始数据块（存储.和..）
    if (type == FT_DIR) {
        // 分配第一个数据块，用于存储目录项
        ip->addrs[0] = bitmap_alloc_block();
        ip->size = 0;  // 后续目录项添加会更新size
        inode_rw(ip, true); // 同步到磁盘
    }

    // 4. 解锁inode，返回创建结果
    inode_unlock(ip);
    return ip;
}

/**
 * @brief 销毁磁盘inode及其关联数据（内部辅助函数，供inode_free调用）
 * @param ip 内存inode指针
 * @note 调用者必须持有lk_icache，且不持有ip->slk
 */
static void inode_destroy(inode_t* ip)
{
    // 1. 获取inode睡眠锁，保护元数据操作
    sleeplock_acquire(&ip->slk);

    // 2. 释放inode管理的所有数据块
    inode_free_data(ip);

    // 3. 清空磁盘inode（标记为未使用）
    ip->type = FT_UNUSED;
    inode_rw(ip, true);

    // 4. 释放inode位图中的对应bit（磁盘层面回收inode）
    bitmap_free_inode(ip->inode_num);

    // 5. 解锁并标记元数据无效
    sleeplock_release(&ip->slk);
    ip->valid = false;
}

/**
 * @brief 释放内存inode引用（引用计数-1），适时销毁磁盘inode
 * @param ip 内存inode指针
 * @note 调用者不应该持有inode睡眠锁（ip->slk）
 */
void inode_free(inode_t* ip)
{
    assert(ip != NULL, "inode_free: invalid NULL inode pointer");

    // 1. 获取icache自旋锁，保护引用计数修改和销毁操作
    spinlock_acquire(&lk_icache);

    // 2. 判断是否需要销毁磁盘inode（最后一个引用+无链接+元数据有效）
    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        inode_destroy(ip);
    }

    // 3. 引用计数-1（确保不会出现负引用）
    assert(ip->ref > 0, "inode_free: inode ref count is zero (double free)");
    ip->ref--;

    // 4. 释放自旋锁
    spinlock_release(&lk_icache);
}

/**
 * @brief 复制inode引用（引用计数+1）
 * @param ip 内存inode指针
 * @return 原inode指针（引用计数已增加）
 */
inode_t* inode_dup(inode_t* ip)
{
    assert(ip != NULL && ip->ref > 0, "inode_dup: invalid inode or ref count zero");

    // 1. 获取icache自旋锁，保护引用计数修改
    spinlock_acquire(&lk_icache);

    // 2. 引用计数+1
    ip->ref++;

    // 3. 释放自旋锁，返回原inode指针
    spinlock_release(&lk_icache);
    return ip;
}

/**
 * @brief 给inode上锁，若元数据无效则从磁盘加载
 * @param ip 内存inode指针
 */
void inode_lock(inode_t* ip)
{
    assert(ip != NULL && ip->ref > 0, "inode_lock: invalid inode or ref count zero");

    // 1. 获取inode睡眠锁（排他性访问）
    sleeplock_acquire(&ip->slk);

    // 2. 若元数据无效，从磁盘加载inode信息
    if (ip->valid == false) {
        inode_rw(ip, false);
        ip->valid = true;
    }
}

/**
 * @brief 给inode解锁
 * @param ip 内存inode指针
 */
void inode_unlock(inode_t* ip)
{
    assert(ip != NULL, "inode_unlock: invalid NULL inode pointer");
    assert(sleeplock_holding(&ip->slk), "inode_unlock: not holding inode sleeplock");

    // 释放inode睡眠锁
    sleeplock_release(&ip->slk);
}

/**
 * @brief 快捷操作：解锁inode + 释放inode引用
 * @param ip 内存inode指针
 */
void inode_unlock_free(inode_t* ip)
{
    assert(ip != NULL, "inode_unlock_free: invalid NULL inode pointer");

    // 先解锁，再释放引用
    inode_unlock(ip);
    inode_free(ip);
}

// ---------------------- 与inode管理的data相关（数据块读写与释放） ----------------------
/**
 * @brief 辅助函数：递归查询或创建三级映射中的数据块
 * @param entry 地址项指针
 * @param bn 数据块序号
 * @param size 当前层级的块数量
 * @return 数据块的磁盘编号
 */
static uint32 locate_block(uint32* entry, uint32 bn, uint32 size)
{
    // 1. 若地址项为空，分配新数据块并绑定
    if (*entry == 0) {
        *entry = bitmap_alloc_block();
    }

    // 2. 若当前是一级映射，直接返回数据块编号
    if (size == 1) {
        return *entry;
    }

    // 3. 递归处理多级映射（二级/三级）
    uint32* next_entry;
    uint32 next_size = size / ENTRY_PER_BLOCK;
    uint32 next_bn = bn % next_size;
    uint32 ret = 0;

    // 4. 读取当前元数据块，查找下一级地址项
    buf_t* buf = buf_read(*entry);
    next_entry = (uint32*)(buf->data) + (bn / next_size);
    ret = locate_block(next_entry, next_bn, next_size);

    // 5. 释放缓冲区，返回递归结果
    buf_release(buf);
    return ret;
}

/**
 * @brief 定位inode管理的第bn个数据块（不存在则创建）
 * @param ip 内存inode指针
 * @param bn 数据块序号（从0开始）
 * @return 数据块的磁盘编号
 */
static uint32 inode_locate_block(inode_t* ip, uint32 bn)
{
    // 1. 一级映射区域（直接映射，N_ADDRS_1个块）
    if (bn < N_ADDRS_1) {
        return locate_block(&ip->addrs[bn], bn, 1);
    }

    // 2. 二级映射区域（间接映射，N_ADDRS_2 * ENTRY_PER_BLOCK个块）
    bn -= N_ADDRS_1;
    if (bn < N_ADDRS_2 * ENTRY_PER_BLOCK) {
        uint32 size = ENTRY_PER_BLOCK;
        uint32 idx = bn / size;
        uint32 b = bn % size;
        return locate_block(&ip->addrs[N_ADDRS_1 + idx], b, size);
    }

    // 3. 三级映射区域（二级间接映射，N_ADDRS_3 * ENTRY_PER_BLOCK^2个块）
    bn -= N_ADDRS_2 * ENTRY_PER_BLOCK;
    if (bn < N_ADDRS_3 * ENTRY_PER_BLOCK * ENTRY_PER_BLOCK) {
        uint32 size = ENTRY_PER_BLOCK * ENTRY_PER_BLOCK;
        uint32 idx = bn / size;
        uint32 b = bn % size;
        return locate_block(&ip->addrs[N_ADDRS_1 + N_ADDRS_2 + idx], b, size);
    }

    // 4. 超出最大映射范围，报错退出
    panic("inode_locate_block: data block number overflow");
    return 0;
}

/**
 * @brief 从inode中读取数据
 * @param ip 内存inode指针
 * @param offset 读取起始偏移量（字节）
 * @param len 读取字节数
 * @param dst 目标缓冲区指针
 * @param user true=用户态缓冲区，false=内核态缓冲区
 * @return 实际读取的字节数
 * @note 调用者必须持有inode睡眠锁
 */
uint32 inode_read_data(inode_t* ip, uint32 offset, uint32 len, void* dst, bool user)
{
    assert(sleeplock_holding(&ip->slk), "inode_read_data: not holding inode sleeplock");
    assert(dst != NULL, "inode_read_data: invalid NULL dst pointer");

    // 1. 边界检查：偏移量超出文件大小，返回0
    if (offset > ip->size) {
        return 0;
    }

    // 2. 调整读取长度：避免超出文件末尾
    if (offset + len > ip->size) {
        len = ip->size - offset;
    }

    uint32 total_read = 0;
    uint32 block_num, block_offset, read_len;

    // 3. 循环读取数据，直到完成或无更多数据
    while (total_read < len) {
        // 3.1 计算当前数据块编号和块内偏移
        block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
        block_offset = offset % BLOCK_SIZE;

        // 3.2 计算本次可读取的字节数
        read_len = BLOCK_SIZE - block_offset;
        if (read_len > len - total_read) {
            read_len = len - total_read;
        }

        // 3.3 读取数据块到缓冲区
        buf_t* buf = buf_read(block_num);

        // 3.4 复制数据到目标缓冲区（区分用户态/内核态）
        if (user) {
            // 用户态缓冲区：通过虚拟内存拷贝（uvm_copyout）
            uvm_copyout(myproc()->pgtbl, (uint64)dst + total_read,
                       (uint64)(buf->data + block_offset), read_len);
        } else {
            // 内核态缓冲区：直接内存拷贝
            memmove((char*)dst + total_read, buf->data + block_offset, read_len);
        }

        // 3.5 释放缓冲区，更新统计信息
        buf_release(buf);
        total_read += read_len;
        offset += read_len;
    }

    // 4. 返回实际读取的字节数
    return total_read;
}

/**
 * @brief 向inode中写入数据（可能扩展数据块）
 * @param ip 内存inode指针
 * @param offset 写入起始偏移量（字节）
 * @param len 写入字节数
 * @param src 源缓冲区指针
 * @param user true=用户态缓冲区，false=内核态缓冲区
 * @return 实际写入的字节数
 * @note 调用者必须持有inode睡眠锁
 */
uint32 inode_write_data(inode_t* ip, uint32 offset, uint32 len, void* src, bool user)
{
    assert(sleeplock_holding(&ip->slk), "inode_write_data: not holding inode sleeplock");
    assert(src != NULL, "inode_write_data: invalid NULL src pointer");

    // 1. 边界检查：偏移量超出最大限制，返回0
    if (offset > INODE_MAXSIZE) {
        return 0;
    }

    // 2. 边界检查：写入后超出最大限制，返回0
    if (offset + len > INODE_MAXSIZE) {
        return 0;
    }

    uint32 total_written = 0;
    uint32 block_num, block_offset, write_len;

    // 3. 循环写入数据，直到完成
    while (total_written < len) {
        // 3.1 计算当前数据块编号和块内偏移
        block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
        block_offset = offset % BLOCK_SIZE;

        // 3.2 计算本次可写入的字节数
        write_len = BLOCK_SIZE - block_offset;
        if (write_len > len - total_written) {
            write_len = len - total_written;
        }

        // 3.3 读取数据块到缓冲区
        buf_t* buf = buf_read(block_num);

        // 3.4 复制数据到缓冲区（区分用户态/内核态）
        if (user) {
            // 用户态缓冲区：通过虚拟内存拷贝（uvm_copyin）
            uvm_copyin(myproc()->pgtbl, (uint64)(buf->data + block_offset),
                      (uint64)src + total_written, write_len);
        } else {
            // 内核态缓冲区：直接内存拷贝
            memmove(buf->data + block_offset, (char*)src + total_written, write_len);
        }

        // 3.5 强制写入磁盘，释放缓冲区
        buf_write(buf);
        buf_release(buf);

        // 3.6 更新统计信息
        total_written += write_len;
        offset += write_len;
    }

    // 4. 更新inode文件大小（若写入超出原有大小）
    if (offset > ip->size) {
        ip->size = offset;
    }

    // 5. 将更新后的inode元数据写入磁盘
    inode_rw(ip, true);

    // 6. 返回实际写入的字节数
    return total_written;
}

/**
 * @brief 辅助函数：递归释放inode管理的数据块（包括元数据块）
 * @param block_num 数据块/元数据块编号
 * @param level 映射层级（0=数据块，1=二级元数据块，2=三级元数据块）
 */
static void data_free(uint32 block_num, uint32 level)
{
    assert(block_num != 0, "data_free: block_num is zero (invalid block)");

    // 1. 层级0：直接释放数据块（无下级映射）
    if (level == 0) {
        goto ret;
    }

    // 2. 层级>0：递归释放下级元数据块/数据块
    buf_t* buf = buf_read(block_num);
    for (uint32* addr = (uint32*)buf->data; addr < (uint32*)(buf->data + BLOCK_SIZE); addr++) {
        if (*addr == 0) {
            break; // 无更多下级块，退出循环
        }
        data_free(*addr, level - 1); // 递归释放下一级
    }
    buf_release(buf); // 释放当前元数据块缓冲区

ret:
    // 3. 释放当前块（位图层面回收）
    bitmap_free_block(block_num);
    return;
}

/**
 * @brief 释放inode管理的所有数据块，清空地址映射
 * @param ip 内存inode指针
 * @note 调用者必须持有inode睡眠锁
 */
void inode_free_data(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "inode_free_data: not holding inode sleeplock");

    // 1. 释放一级映射数据块（层级0）
    for (int i = 0; i < N_ADDRS_1; i++) {
        if (ip->addrs[i] != 0) {
            data_free(ip->addrs[i], 0);
            ip->addrs[i] = 0; // 清空地址项
        }
    }

    // 2. 释放二级映射数据块（层级1）
    for (int i = 0; i < N_ADDRS_2; i++) {
        if (ip->addrs[N_ADDRS_1 + i] != 0) {
            data_free(ip->addrs[N_ADDRS_1 + i], 1);
            ip->addrs[N_ADDRS_1 + i] = 0; // 清空地址项
        }
    }

    // 3. 释放三级映射数据块（层级2）
    for (int i = 0; i < N_ADDRS_3; i++) {
        if (ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i] != 0) {
            data_free(ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i], 2);
            ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i] = 0; // 清空地址项
        }
    }

    // 4. 重置文件大小，同步到磁盘
    ip->size = 0;
    inode_rw(ip, true);
}

// ---------------------- 调试辅助函数 ----------------------
static char* inode_types[] = {
    "INODE_UNUSED",
    "INODE_DIR",
    "INODE_FILE",
    "INODE_DEVICE",
};

/**
 * @brief 打印inode详细信息（调试用）
 * @param ip 内存inode指针
 * @note 调用者必须持有inode睡眠锁
 */
void inode_print(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "inode_print: not holding inode sleeplock");

    printf("\n===================== Inode Debug Info =====================\n");
    printf("inode num = %d, ref count = %d, valid = %s\n",
           ip->inode_num, ip->ref, ip->valid ? "true" : "false");
    printf("type = %s, major = %d, minor = %d, nlink = %d\n",
           inode_types[ip->type], ip->major, ip->minor, ip->nlink);
    printf("file size = %d bytes, addrs = [", ip->size);
    for (int i = 0; i < N_ADDRS; i++) {
        if (i > 0 && i % 6 == 0) { // 每6个地址换行，优化排版
            printf("\n\t");
        }
        printf(" %d", ip->addrs[i]);
    }
    printf(" ]\n");
    printf("=============================================================\n\n");
}