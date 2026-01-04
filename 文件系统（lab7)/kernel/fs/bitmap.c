#include "fs/buf.h"
#include "fs/fs.h"
#include "fs/bitmap.h"
#include "lib/print.h"

// 全局超级块（外部定义，来自文件系统初始化模块）
extern super_block_t sb;

/**
 * @brief 静态辅助函数：在指定位图磁盘块中，查找第一个空闲bit并置为1（已分配）
 * @param bitmap_block 存储位图的磁盘块编号
 * @return 找到的空闲bit的序号（从0开始，范围：0 ~ BLOCK_SIZE*8-1）
 */
static uint32 bitmap_search_and_set(uint32 bitmap_block)
{
    uint32 byte_idx;    // 缓冲区数据中的字节索引（0 ~ BLOCK_SIZE-1）
    uint32 bit_shift;   // 字节内的bit偏移量（0 ~ 7）
    uint8 bit_cmp;      // bit判断掩码（用于定位单个bit）

    // 1. 调用buf层函数，读取存储位图的磁盘块到缓冲区
    buf_t* bitmap_buf = buf_read(bitmap_block);
    assert(bitmap_buf != NULL, "bitmap_search_and_set: read bitmap block failed");

    // 2. 遍历缓冲区中的所有字节（每个字节对应8个bit）
    for (byte_idx = 0; byte_idx < BLOCK_SIZE; byte_idx++) {
        bit_cmp = 1;  // 初始化掩码，从第0位开始判断（0b00000001）

        // 3. 遍历当前字节的所有8个bit，查找第一个空闲bit（值为0）
        for (bit_shift = 0; bit_shift <= 7; bit_shift++) {
            // 判断当前bit是否为空闲（与运算结果为0表示空闲）
            if ((bit_cmp & bitmap_buf->data[byte_idx]) == 0) {
                // 4. 找到空闲bit，将其置为1（已分配，或运算）
                bitmap_buf->data[byte_idx] |= bit_cmp;

                // 5. 强制写入磁盘，保证位图数据与磁盘同步
                buf_write(bitmap_buf);

                // 6. 释放缓冲区引用，允许buf层回收复用
                buf_release(bitmap_buf);

                // 7. 返回该bit的全局序号（字节索引*8 + 字节内偏移量）
                return byte_idx * 8 + bit_shift;
            }

            // 8. 掩码左移一位，判断下一个bit
            bit_cmp = bit_cmp << 1;
        }
    }

    // 9. 遍历完所有bit，无空闲资源，报错并退出
    buf_release(bitmap_buf);  // 释放缓冲区，避免死锁
    panic("bitmap_search_and_set: no free bit available in block ");
    return 0;  // 不可达，仅满足函数返回值要求
}

/**
 * @brief 静态辅助函数：在指定位图磁盘块中，将指定序号的bit置为0（空闲）
 * @param bitmap_block 存储位图的磁盘块编号
 * @param num 要置为空闲的bit序号（从0开始，范围：0 ~ BLOCK_SIZE*8-1）
 */
static void bitmap_unset(uint32 bitmap_block, uint32 num)
{
    // 1. 计算该bit对应的字节索引和字节内偏移量
    uint32 byte_idx = num / 8;    // 字节索引 = bit序号 / 8（整数除法）
    uint32 bit_shift = num % 8;   // 字节内偏移 = bit序号 % 8
    uint8 bit_cmp = 1 << bit_shift;  // 构建bit判断/操作掩码

    // 2. 合法性校验：字节索引不能超过磁盘块大小（避免越界访问）
    if (byte_idx >= BLOCK_SIZE) {
        panic("bitmap_unset: invalid bit num  (byte index out of range)");
    }

    // 3. 调用buf层函数，读取存储位图的磁盘块到缓冲区
    buf_t* bitmap_buf = buf_read(bitmap_block);
    assert(bitmap_buf != NULL, "bitmap_unset: read bitmap block failed");

    // 4. 校验该bit是否已经是空闲状态（避免重复释放）
    if ((bitmap_buf->data[byte_idx] & bit_cmp) == 0) {
        buf_release(bitmap_buf);  // 释放缓冲区，避免死锁
        panic("bitmap_unset: bit in block  is already free");
    }

    // 5. 将该bit置为0（空闲，与运算取反掩码）
    bitmap_buf->data[byte_idx] &= ~bit_cmp;

    // 6. 强制写入磁盘，保证位图数据与磁盘同步
    buf_write(bitmap_buf);

    // 7. 释放缓冲区引用，允许buf层回收复用
    buf_release(bitmap_buf);
}

/**
 * @brief 分配一个空闲的数据块（返回数据块的磁盘块编号）
 * @return 空闲数据块的磁盘块编号
 */
uint32 bitmap_alloc_block()
{
    // 1. 在位图数据块中查找并分配一个空闲bit
    uint32 free_bit_num = bitmap_search_and_set(sb.data_bitmap_start);

    // 2. 转换为数据块的磁盘块编号（数据区域起始块 + bit序号）
    // 解释：data_bitmap中的第N个bit，对应data区域的第N个数据块
    return sb.data_start + free_bit_num;
}

/**
 * @brief 释放一个已分配的数据块（将对应位图bit置为空闲）
 * @param block_num 要释放的数据块磁盘编号
 */
void bitmap_free_block(uint32 block_num)
{
    // 1. 合法性校验：数据块编号不能小于数据区域起始块
    if (block_num < sb.data_start) {
        panic("bitmap_free_block: invalid data block num  (less than data start )");
    }

    // 2. 转换为data位图中的bit序号（数据块编号 - 数据区域起始块）
    uint32 bit_num = block_num - sb.data_start;

    // 3. 校验bit序号是否超出data位图的最大范围（避免越界）
    if (bit_num >= (sb.data_blocks * BLOCK_SIZE * 8)) {
        panic("bitmap_free_block: invalid data block num (out of data area range)");
    }

    // 4. 置位data位图中对应的bit为空闲
    bitmap_unset(sb.data_bitmap_start, bit_num);
}

/**
 * @brief 分配一个空闲的inode（返回inode序号）
 * @return 空闲inode的序号（从0开始）
 */
uint16 bitmap_alloc_inode()
{
    // 1. 在位图inode块中查找并分配一个空闲bit
    uint32 free_bit_num = bitmap_search_and_set(sb.inode_bitmap_start);

    // 2. 转换为uint16类型返回（inode序号范围通常较小，满足uint16存储）
    return (uint16)free_bit_num;
}

/**
 * @brief 释放一个已分配的inode（将对应位图bit置为空闲）
 * @param inode_num 要释放的inode序号
 */
void bitmap_free_inode(uint16 inode_num)
{
    // 1. 置位inode位图中对应的bit为空闲（强制转换为uint32适配函数参数）
    bitmap_unset(sb.inode_bitmap_start, (uint32)inode_num);
}

/**
 * @brief 打印指定位图磁盘块中所有已分配的bit序号（调试用）
 * @param bitmap_block_num 存储位图的磁盘块编号
 */
void bitmap_print(uint32 bitmap_block_num)
{
    uint8 bit_cmp;      // bit判断掩码
    uint32 byte_idx;    // 缓冲区数据中的字节索引
    uint32 bit_shift;   // 字节内的bit偏移量

    printf("\n===================== Bitmap Debug Info =====================\n");
    printf("Bitmap block num: %d\n", bitmap_block_num);
    printf("Allocated bits (start from 0):\n\n");

    // 1. 调用buf层函数，读取存储位图的磁盘块到缓冲区
    buf_t* bitmap_buf = buf_read(bitmap_block_num);
    assert(bitmap_buf != NULL, "bitmap_print: read bitmap block failed");

    // 2. 遍历缓冲区中的所有字节（每个字节对应8个bit）
    for (byte_idx = 0; byte_idx < BLOCK_SIZE; byte_idx++) {
        bit_cmp = 1;  // 初始化掩码，从第0位开始判断（0b00000001）

        // 3. 遍历当前字节的所有8个bit，查找已分配的bit（值为1）
        for (bit_shift = 0; bit_shift <= 7; bit_shift++) {
            // 判断当前bit是否已分配（与运算结果非0表示已分配）
            if (bit_cmp & bitmap_buf->data[byte_idx]) {
                printf("  Bit %d is allocated\n", byte_idx * 8 + bit_shift);
            }

            // 4. 掩码左移一位，判断下一个bit
            bit_cmp = bit_cmp << 1;
        }
    }

    // 5. 释放缓冲区引用，允许buf层回收复用
    buf_release(bitmap_buf);

    printf("\n===================== Bitmap Print Over =====================\n\n");
}