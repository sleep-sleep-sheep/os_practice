#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/file.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"

// 设备列表(存储各类设备的读写接口，全局可见)
dev_t devlist[N_DEV];

// 文件表（ftable）配置：最大32个文件同时打开
#define N_FILE 32
file_t ftable[N_FILE];    // 全局文件表
spinlock_t lk_ftable;     // 保护文件表的自旋锁（引用计数、文件查找/分配）

// ---------------------- 基础初始化 ----------------------
/**
 * @brief 初始化文件表（ftable）和设备列表（devlist）
 * @note 必须在文件系统初始化后、进程创建前调用
 */
void file_init()
{
    // 1. 初始化文件表全局自旋锁
    spinlock_init(&lk_ftable, "ftable");

    // 2. 遍历初始化文件表中的所有文件项（空闲状态）
    for (int i = 0; i < N_FILE; i++) {
        file_t* file = &ftable[i];
        file->ref = 0;                // 引用计数初始化为0（空闲）
        file->type = FD_UNUSED;       // 类型标记为未使用
        file->readable = false;       // 默认不可读
        file->writable = false;       // 默认不可写
        file->major = 0;              // 默认主设备号为0
        file->offset = 0;             // 默认偏移量为0
        file->ip = NULL;              // 默认无关联inode
    }

    // 3. 遍历初始化设备列表（默认无读写接口）
    for (int i = 0; i < N_DEV; i++) {
        devlist[i].read = NULL;       // 默认无读接口
        devlist[i].write = NULL;      // 默认无写接口
    }
}

// ---------------------- 文件项分配与释放 ----------------------
/**
 * @brief 在文件表（ftable）中分配一个空闲文件项
 * @return 空闲文件项指针（引用计数初始化为1）
 * @note 无空闲文件项时panic报错
 */
file_t* file_alloc()
{
    // 1. 获取文件表自旋锁，保护文件表遍历和修改
    spinlock_acquire(&lk_ftable);

    // 2. 遍历文件表，查找第一个空闲文件项（引用计数为0）
    for (int i = 0; i < N_FILE; i++) {
        file_t* file = &ftable[i];
        if (file->ref == 0) {
            // 3. 初始化空闲文件项的核心字段
            file->ref = 1;                // 引用计数置1（标记被使用）
            file->type = FD_UNUSED;       // 类型暂标记为未使用
            file->readable = false;       // 默认不可读
            file->writable = false;       // 默认不可写
            file->major = 0;              // 默认主设备号为0
            file->offset = 0;             // 默认偏移量为0
            file->ip = NULL;              // 默认无关联inode

            // 4. 释放自旋锁，返回分配的文件项
            spinlock_release(&lk_ftable);
            return file;
        }
    }

    // 5. 无空闲文件项，报错退出
    spinlock_release(&lk_ftable);
    panic("file_alloc: no free file in ftable");
    return NULL;  // 不可达，仅满足函数返回值要求
}

/**
 * @brief 关闭文件并释放文件项引用（引用计数-1）
 * @param file 要关闭的文件项指针
 */
void file_close(file_t* file)
{
    assert(file != NULL, "file_close: invalid NULL file pointer");

    // 1. 获取文件表自旋锁，保护引用计数修改
    spinlock_acquire(&lk_ftable);

    // 2. 校验引用计数合法性（避免重复关闭）
    if (file->ref < 1) {
        spinlock_release(&lk_ftable);
        panic("file_close: file ref count is less than 1 (double close)");
    }

    // 3. 引用计数-1
    file->ref--;

    // 4. 若引用计数归0（无任何进程使用），释放关联资源
    if (file->ref == 0) {
        inode_t* ip = file->ip;  // 保存关联inode，后续释放

        // 4.1 重置文件项字段，标记为空闲
        file->type = FD_UNUSED;
        file->readable = false;
        file->writable = false;
        file->major = 0;
        file->offset = 0;
        file->ip = NULL;

        // 4.2 释放自旋锁（后续操作不涉及文件表）
        spinlock_release(&lk_ftable);

        // 4.3 释放关联的inode（若存在）
        if (ip != NULL) {
            inode_free(ip);
        }
    } else {
        // 5. 引用计数仍大于0，直接释放自旋锁
        spinlock_release(&lk_ftable);
    }
}

// ---------------------- 文件创建与打开 ----------------------
/**
 * @brief 创建设备文件（供初始化进程创建控制台等设备）
 * @param path 设备文件路径
 * @param major 主设备号
 * @param minor 次设备号
 * @return 创建设备文件对应的文件项指针，失败返回NULL
 */
file_t* file_create_dev(char* path, uint16 major, uint16 minor)
{
    assert(path != NULL, "file_create_dev: invalid NULL path");
    assert(major < N_DEV, "file_create_dev: major device number out of range");

    // 1. 根据路径创建设备类型inode（FT_DEVICE）
    inode_t* ip = path_create_inode(path, FT_DEVICE, major, minor);
    if (ip == NULL) {
        printf("file_create_dev: create inode for path %s failed\n", path);
        return NULL;
    }

    // 2. 分配文件表项
    file_t* file = file_alloc();
    if (file == NULL) {
        inode_free(ip);
        return NULL;
    }

    // 3. 初始化设备文件项字段
    file->type = FD_DEVICE;    // 标记为设备文件
    file->readable = true;     // 设备文件默认可读
    file->writable = true;     // 设备文件默认可写
    file->major = major;       // 绑定主设备号
    file->ip = ip;             // 关联设备inode

    // 4. 返回创建的设备文件项
    return file;
}

/**
 * @brief 打开一个文件（支持普通文件、目录、设备文件）
 * @param path 文件路径
 * @param open_mode 打开模式（MODE_CREATE/MODE_READ/MODE_WRITE）
 * @return 打开文件对应的文件项指针，失败返回NULL
 */
file_t* file_open(char* path, uint32 open_mode)
{
    assert(path != NULL, "file_open: invalid NULL path");

    inode_t* ip = NULL;
    file_t* file = NULL;

    // 1. 根据打开模式获取/创建inode
    if (open_mode & MODE_CREATE) {
        // 模式包含创建：文件不存在则创建（默认创建普通文件FT_FILE）
        ip = path_create_inode(path, FT_FILE, 0, 0);
    } else {
        // 模式不包含创建：仅查找已有文件的inode
        ip = path_to_inode(path);
    }

    // 2. 校验inode获取/创建结果
    if (ip == NULL) {
        printf("file_open: get inode for path %s failed\n", path);
        return NULL;
    }

    // 3. 上锁保护inode元数据操作
    inode_lock(ip);

    // 4. 分配文件表项
    file = file_alloc();
    if (file == NULL) {
        inode_unlock(ip);
        inode_free(ip);
        return NULL;
    }

    // 5. 根据inode类型初始化文件项类型
    if (ip->type == FT_DIR) {
        file->type = FD_DIR;        // 标记为目录文件
    } else if (ip->type == FT_DEVICE) {
        file->type = FD_DEVICE;     // 标记为设备文件
        file->major = ip->major;    // 绑定主设备号
    } else {
        file->type = FD_FILE;       // 标记为普通文件
    }

    // 6. 根据打开模式设置读写权限
    file->readable = (open_mode & MODE_READ) ? true : false;
    file->writable = (open_mode & MODE_WRITE) ? true : false;

    // 7. 初始化文件项其他字段
    file->offset = 0;             // 初始偏移量为0
    file->ip = ip;                // 关联inode

    // 8. 解锁inode，返回打开的文件项
    inode_unlock(ip);
    return file;
}

// ---------------------- 文件读写操作 ----------------------
/**
 * @brief 从文件中读取数据
 * @param file 已打开的文件项指针
 * @param len 要读取的字节数
 * @param dst 目标缓冲区地址（用户态/内核态）
 * @param user true=用户态缓冲区，false=内核态缓冲区
 * @return 实际读取的字节数，失败返回0
 */
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool user)
{
    assert(file != NULL, "file_read: invalid NULL file pointer");
    if (len == 0) return 0;

    // 1. 校验文件可读权限
    if (!file->readable) {
        printf("file_read: file does not have read permission\n");
        return 0;
    }

    uint32 ret_bytes = 0;

    // 2. 设备文件：调用对应设备的读接口
    if (file->type == FD_DEVICE) {
        if (file->major < N_DEV && devlist[file->major].read != NULL) {
            ret_bytes = devlist[file->major].read(len, dst, user);
        }
    }
    // 3. 普通文件/目录：调用inode数据读取接口
    else if (file->type == FD_FILE || file->type == FD_DIR) {
        if (file->ip == NULL) return 0;

        inode_lock(file->ip);
        // 从当前偏移量开始读取数据
        ret_bytes = inode_read_data(file->ip, file->offset, len, (void*)dst, user);
        // 更新文件偏移量（向后移动实际读取的字节数）
        file->offset += ret_bytes;
        inode_unlock(file->ip);
    }

    // 4. 返回实际读取的字节数
    return ret_bytes;
}

/**
 * @brief 向文件中写入数据
 * @param file 已打开的文件项指针
 * @param len 要写入的字节数
 * @param src 源缓冲区地址（用户态/内核态）
 * @param user true=用户态缓冲区，false=内核态缓冲区
 * @return 实际写入的字节数，失败返回0
 */
uint32 file_write(file_t* file, uint32 len, uint64 src, bool user)
{
    assert(file != NULL, "file_write: invalid NULL file pointer");
    if (len == 0) return 0;

    // 1. 校验文件可写权限
    if (!file->writable) {
        printf("file_write: file does not have write permission\n");
        return 0;
    }

    uint32 ret_bytes = 0;

    // 2. 设备文件：调用对应设备的写接口
    if (file->type == FD_DEVICE) {
        if (file->major < N_DEV && devlist[file->major].write != NULL) {
            ret_bytes = devlist[file->major].write(len, src, user);
        }
    }
    // 3. 普通文件：调用inode数据写入接口（目录不支持写入）
    else if (file->type == FD_FILE) {
        if (file->ip == NULL) return 0;

        inode_lock(file->ip);
        // 从当前偏移量开始写入数据
        ret_bytes = inode_write_data(file->ip, file->offset, len, (void*)src, user);
        // 更新文件偏移量（向后移动实际写入的字节数）
        file->offset += ret_bytes;
        inode_unlock(file->ip);
    }

    // 4. 返回实际写入的字节数
    return ret_bytes;
}

// ---------------------- 文件偏移量调整 ----------------------
// 偏移量调整标志定义
#define LSEEK_SET 0  // file->offset = offset（绝对偏移）
#define LSEEK_ADD 1  // file->offset += offset（相对增加）
#define LSEEK_SUB 2  // file->offset -= offset（相对减少）

/**
 * @brief 修改普通文件的读写偏移量（仅支持FD_FILE类型）
 * @param file 已打开的文件项指针
 * @param offset 偏移量参数
 * @param flags 调整标志（LSEEK_SET/LSEEK_ADD/LSEEK_SUB）
 * @return 调整后的偏移量，失败返回-1（无符号数表现为0xFFFFFFFF）
 */
uint32 file_lseek(file_t* file, uint32 offset, int flags)
{
    assert(file != NULL, "file_lseek: invalid NULL file pointer");

    // 1. 仅支持普通文件（FD_FILE）
    if (file->type != FD_FILE) {
        printf("file_lseek: only support FD_FILE type\n");
        return (uint32)-1;
    }

    // 2. 根据标志调整偏移量
    switch (flags) {
        case LSEEK_SET:
            // 绝对偏移：直接设置为指定值
            file->offset = offset;
            break;
        case LSEEK_ADD:
            // 相对增加：当前偏移量加上指定值
            file->offset += offset;
            break;
        case LSEEK_SUB:
            // 相对减少：当前偏移量减去指定值（不小于0）
            if (file->offset >= offset) {
                file->offset -= offset;
            } else {
                file->offset = 0;
            }
            break;
        default:
            // 无效标志，返回失败
            printf("file_lseek: invalid flags %d\n", flags);
            return (uint32)-1;
    }

    // 3. 返回调整后的偏移量
    return file->offset;
}

// ---------------------- 文件引用复制 ----------------------
/**
 * @brief 复制文件引用（引用计数+1）
 * @param file 已打开的文件项指针
 * @return 原文件项指针（引用计数已增加）
 */
file_t* file_dup(file_t* file)
{
    assert(file != NULL, "file_dup: invalid NULL file pointer");

    // 1. 获取文件表自旋锁，保护引用计数修改
    spinlock_acquire(&lk_ftable);

    // 2. 校验引用计数合法性
    assert(file->ref > 0, "file_dup: file ref count is zero (invalid file)");

    // 3. 引用计数+1
    file->ref++;

    // 4. 释放自旋锁，返回原文件项指针
    spinlock_release(&lk_ftable);
    return file;
}

// ---------------------- 文件状态查询 ----------------------
/**
 * @brief 获取文件状态信息（普通文件/目录）
 * @param file 已打开的文件项指针
 * @param addr 用户态缓冲区地址（用于存储文件状态）
 * @return 0表示成功，-1表示失败
 */
int file_stat(file_t* file, uint64 addr)
{
    assert(file != NULL, "file_stat: invalid NULL file pointer");
    assert(addr != 0, "file_stat: invalid zero address");

    file_state_t state;

    // 1. 仅支持普通文件和目录
    if (file->type == FD_FILE || file->type == FD_DIR) {
        if (file->ip == NULL) return -1;

        // 2. 上锁读取inode元数据，填充文件状态
        inode_lock(file->ip);
        state.type = file->ip->type;
        state.inode_num = file->ip->inode_num;
        state.nlink = file->ip->nlink;
        state.size = file->ip->size;
        inode_unlock(file->ip);

        // 3. 将状态信息拷贝到用户态缓冲区
        uvm_copyout(myproc()->pgtbl, addr, (uint64)&state, sizeof(file_state_t));

        // 4. 返回成功
        return 0;
    }

    // 5. 不支持的文件类型，返回失败
    printf("file_stat: unsupported file type %d\n", file->type);
    return -1;
}