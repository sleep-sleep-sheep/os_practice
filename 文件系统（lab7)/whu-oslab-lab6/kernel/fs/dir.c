#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/inode.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "mem/vmem.h"
#include "lib/str.h"
#include "lib/print.h"
#include "proc/cpu.h"

// 简化假设：每个目录文件仅占用1个数据块，最多存储 BLOCK_SIZE/sizeof(dirent_t) 个目录项
// 注：DIR_NAME_LEN=30，sizeof(dirent_t)=32，BLOCK_SIZE=512时，最多支持16个目录项

// ---------------------- 目录项核心操作 ----------------------
/**
 * @brief 查询目录中指定名称的目录项
 * @param pip 目录对应的inode指针（已上锁）
 * @param name 要查询的目录项名称
 * @return 成功返回目录项对应的inode_num，失败返回INODE_NUM_UNUSED
 * @note 调用者必须持有pip的睡眠锁，且pip必须是目录类型（FT_DIR）
 */
uint16 dir_search_entry(inode_t *pip, char *name)
{
    // 1. 并发安全与类型校验
    assert(sleeplock_holding(&pip->slk), "dir_search_entry: not holding inode sleep lock");
    assert(pip->type == FT_DIR, "dir_search_entry: inode is not a directory");
    assert(name != NULL && name[0] != 0, "dir_search_entry: invalid directory name");

    dirent_t *de;
    buf_t *dir_buf = NULL;

    // 2. 读取目录对应的唯一数据块
    dir_buf = buf_read(pip->addrs[0]);
    assert(dir_buf != NULL, "dir_search_entry: read directory block failed");

    // 3. 遍历所有目录项，查找匹配名称
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t *)(dir_buf->data + offset);
        // 跳过空目录项，匹配非空目录项名称
        if (de->name[0] != 0 && strncmp(de->name, name, DIR_NAME_LEN) == 0) {
            uint16 target_inum = de->inode_num;
            buf_release(dir_buf);
            return target_inum;
        }
    }

    // 4. 未找到匹配目录项，返回失败
    buf_release(dir_buf);
    return INODE_NUM_UNUSED;
}

/**
 * @brief 向目录中添加一个新目录项
 * @param pip 目录对应的inode指针（已上锁）
 * @param inode_num 新目录项对应的inode序号
 * @param name 新目录项的名称
 * @return 成功返回目录项在数据块中的偏移量，失败返回BLOCK_SIZE
 * @note 调用者必须持有pip的睡眠锁，且pip必须是目录类型（FT_DIR）
 */
uint32 dir_add_entry(inode_t *pip, uint16 inode_num, char *name)
{
    // 1. 并发安全与类型校验
    assert(sleeplock_holding(&pip->slk), "dir_add_entry: not holding inode sleep lock");
    assert(pip->type == FT_DIR, "dir_add_entry: inode is not a directory");
    assert(name != NULL && name[0] != 0, "dir_add_entry: invalid directory name");
    assert(inode_num != INODE_NUM_UNUSED, "dir_add_entry: invalid inode number");

    // 2. 检查重名：已存在同名目录项则返回失败
    if (dir_search_entry(pip, name) != INODE_NUM_UNUSED) {
        return BLOCK_SIZE;
    }

    dirent_t *de;
    buf_t *dir_buf = NULL;

    // 3. 读取目录对应的唯一数据块
    dir_buf = buf_read(pip->addrs[0]);
    assert(dir_buf != NULL, "dir_add_entry: read directory block failed");

    // 4. 遍历查找空闲目录项（名称为空或inode_num无效）
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t *)(dir_buf->data + offset);
        if (de->name[0] == 0 || de->inode_num == INODE_NUM_UNUSED) {
            // 5. 填写新目录项数据
            de->inode_num = inode_num;
            strncpy(de->name, name, DIR_NAME_LEN); // 截断过长名称，保证不越界

            // 6. 同步到磁盘并释放缓冲区
            buf_write(dir_buf);
            buf_release(dir_buf);

            // 7. 更新目录inode大小（记录有效数据长度）
            if (offset + sizeof(dirent_t) > pip->size) {
                pip->size = offset + sizeof(dirent_t);
                inode_rw(pip, true); // 同步inode元数据到磁盘
            }

            // 8. 返回成功的偏移量
            return offset;
        }
    }

    // 9. 无空闲目录项，返回失败
    buf_release(dir_buf);
    return BLOCK_SIZE;
}

/**
 * @brief 从目录中删除指定名称的目录项
 * @param pip 目录对应的inode指针（已上锁）
 * @param name 要删除的目录项名称
 * @return 成功返回目录项对应的inode_num，失败返回INODE_NUM_UNUSED
 * @note 调用者必须持有pip的睡眠锁，且pip必须是目录类型（FT_DIR）
 */
uint16 dir_delete_entry(inode_t *pip, char *name)
{
    // 1. 并发安全与类型校验
    assert(sleeplock_holding(&pip->slk), "dir_delete_entry: not holding inode sleep lock");
    assert(pip->type == FT_DIR, "dir_delete_entry: inode is not a directory");
    assert(name != NULL && name[0] != 0, "dir_delete_entry: invalid directory name");

    dirent_t *de;
    buf_t *dir_buf = NULL;

    // 2. 读取目录对应的唯一数据块
    dir_buf = buf_read(pip->addrs[0]);
    assert(dir_buf != NULL, "dir_delete_entry: read directory block failed");

    // 3. 遍历查找匹配目录项
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t *)(dir_buf->data + offset);
        if (de->name[0] != 0 && strncmp(de->name, name, DIR_NAME_LEN) == 0) {
            uint16 target_inum = de->inode_num;

            // 4. 清空目录项（标记为空闲）
            memset(de, 0, sizeof(dirent_t));

            // 5. 同步到磁盘并释放缓冲区
            buf_write(dir_buf);
            buf_release(dir_buf);

            // 6. 返回成功的inode_num
            return target_inum;
        }
    }

    // 7. 未找到匹配目录项，返回失败
    buf_release(dir_buf);
    return INODE_NUM_UNUSED;
}

/**
 * @brief 读取目录中的所有有效目录项到目标缓冲区
 * @param pip 目录对应的inode指针（已上锁）
 * @param len 目标缓冲区的最大字节数
 * @param dst 目标缓冲区地址（用户态/内核态）
 * @param user true=用户态缓冲区，false=内核态缓冲区
 * @return 实际读取的字节数（sizeof(dirent_t)的整数倍）
 * @note 调用者必须持有pip的睡眠锁，且pip必须是目录类型（FT_DIR）
 */
uint32 dir_get_entries(inode_t* pip, uint32 len, void* dst, bool user)
{
    // 1. 并发安全与参数校验
    assert(sleeplock_holding(&pip->slk), "dir_get_entries: not holding inode sleep lock");
    assert(pip->type == FT_DIR, "dir_get_entries: inode is not a directory");
    assert(dst != NULL, "dir_get_entries: invalid NULL destination buffer");
    if (len == 0) return 0;

    uint32 total_read = 0;
    dirent_t *de;
    buf_t *dir_buf = NULL;

    // 2. 读取目录对应的唯一数据块
    dir_buf = buf_read(pip->addrs[0]);
    assert(dir_buf != NULL, "dir_get_entries: read directory block failed");

    // 3. 遍历目录项，复制有效项到目标缓冲区
    for (uint32 offset = 0; offset < BLOCK_SIZE && total_read < len; offset += sizeof(dirent_t)) {
        de = (dirent_t *)(dir_buf->data + offset);

        // 4. 跳过空目录项，复制有效目录项
        if (de->name[0] != 0 && de->inode_num != INODE_NUM_UNUSED) {
            // 检查剩余缓冲区空间，避免越界
            if (total_read + sizeof(dirent_t) > len) {
                break;
            }

            // 5. 区分用户态/内核态缓冲区拷贝
            if (user) {
                uvm_copyout(myproc()->pgtbl, (uint64)dst + total_read,
                           (uint64)de, sizeof(dirent_t));
            } else {
                memmove((char*)dst + total_read, de, sizeof(dirent_t));
            }

            // 6. 更新已读取字节数
            total_read += sizeof(dirent_t);
        }
    }

    // 7. 释放缓冲区，返回实际读取字节数
    buf_release(dir_buf);
    return total_read;
}

/**
 * @brief 切换当前进程的工作目录
 * @param path 目标目录路径（绝对路径/相对路径）
 * @return 0表示成功，-1表示失败
 */
uint32 dir_change(char* path)
{
    assert(path != NULL, "dir_change: invalid NULL path");

    // 1. 查找路径对应的inode
    inode_t* ip = path_to_inode(path);
    if (ip == NULL) {
        printf("dir_change: cannot find directory %s\n", path);
        return (uint32)-1;
    }

    // 2. 上锁校验是否为目录类型
    inode_lock(ip);
    if (ip->type != FT_DIR) {
        inode_unlock_free(ip);
        printf("dir_change: %s is not a directory\n", path);
        return (uint32)-1;
    }

    // 3. 解锁inode（后续仅保存引用，不操作元数据）
    inode_unlock(ip);

    // 4. 更新当前进程的工作目录
    proc_t* p = myproc();
    if (p->cwd != NULL) {
        inode_free(p->cwd); // 释放旧工作目录的引用
    }
    p->cwd = ip; // 绑定新工作目录的引用

    // 5. 返回成功
    return 0;
}

/**
 * @brief 打印目录中的所有有效目录项（调试用）
 * @param pip 目录对应的inode指针（已上锁）
 * @note 调用者必须持有pip的睡眠锁，且pip必须是目录类型（FT_DIR）
 */
void dir_print(inode_t *pip)
{
    assert(sleeplock_holding(&pip->slk), "dir_print: lock");
    assert(pip->type == FT_DIR, "dir_print: not a directory");

    printf("\ninode_num = %d dirents:\n", pip->inode_num);

    dirent_t *de;
    buf_t *buf = buf_read(pip->addrs[0]);
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t))
    {
        de = (dirent_t *)(buf->data + offset);
        if (de->name[0] != 0)
            printf("inum = %d dirent = %s\n", de->inode_num, de->name);
    }
    buf_release(buf);
}

// ---------------------- 路径解析核心操作 ----------------------
/**
 * @brief 解析路径中的下一个元素，剥离分隔符'/'
 * @param path 待解析的路径字符串
 * @param name 用于存储解析出的路径元素名称
 * @return 剩余路径字符串指针，无剩余元素返回0
 * @example skipelem("a/bb/c", name) → 返回"bb/c"，name="a"
 */
static char *skip_element(char *path, char *name)
{
    while(*path == '/') path++;
    if(*path == 0) return 0;

    char *s = path;
    while (*path != '/' && *path != 0)
        path++;

    int len = path - s;
    if (len >= DIR_NAME_LEN) {
        memmove(name, s, DIR_NAME_LEN);
    } else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
 * @brief 查找路径对应的inode（或其父目录inode）
 * @param path 待解析的路径字符串
 * @param name 用于存储最后一个路径元素名称
 * @param find_parent true=查找父目录inode，false=查找路径本身inode
 * @return 成功返回inode指针，失败返回NULL
 */
static inode_t* search_inode(char* path, char* name, bool find_parent)
{
    assert(path != NULL, "search_inode: invalid NULL path");
    assert(name != NULL, "search_inode: invalid NULL name buffer");

    inode_t* ip = NULL;
    inode_t* next_ip = NULL;

    // 1. 确定起始目录（绝对路径→根目录，相对路径→当前工作目录）
    if (*path == '/') {
        ip = inode_alloc(INODE_ROOT); // 根目录inode序号固定为0
    } else {
        proc_t* p = myproc();
        if (p->cwd != NULL) {
            ip = inode_dup(p->cwd); // 复制当前工作目录引用
        } else {
            ip = inode_alloc(INODE_ROOT); // 无当前目录，默认根目录
        }
    }
    if (ip == NULL) return NULL;

    // 2. 逐段解析路径元素
    while ((path = skip_element(path, name)) != 0) {
        inode_lock(ip);

        // 3. 校验当前inode是否为目录（路径中间元素必须是目录）
        if (ip->type != FT_DIR) {
            inode_unlock_free(ip);
            return NULL;
        }

        // 4. 查找父目录：若已到达最后一个路径元素，直接返回当前目录
        if (find_parent && *path == '\0') {
            inode_unlock(ip);
            return ip;
        }

        // 5. 在当前目录中查找下一个路径元素对应的inode
        uint16 inum = dir_search_entry(ip, name);
        if (inum == INODE_NUM_UNUSED) {
            inode_unlock_free(ip);
            return NULL;
        }

        // 6. 切换到下一个inode，释放当前inode引用
        next_ip = inode_alloc(inum);
        inode_unlock_free(ip);
        ip = next_ip;
        if (ip == NULL) return NULL;
    }

    // 7. 查找父目录失败（路径为空或无有效父目录）
    if (find_parent) {
        inode_free(ip);
        return NULL;
    }

    // 8. 返回路径对应的inode
    return ip;
}

/**
 * @brief 查找路径对应的inode
 * @param path 待解析的路径字符串（绝对路径/相对路径）
 * @return 成功返回inode指针，失败返回NULL
 */
inode_t* path_to_inode(char* path)
{
    char name[DIR_NAME_LEN];
    return search_inode(path, name, false);
}

/**
 * @brief 查找路径对应的inode的父目录inode
 * @param path 待解析的路径字符串（绝对路径/相对路径）
 * @param name 用于存储路径最后一个元素的名称
 * @return 成功返回父目录inode指针，失败返回NULL
 */
inode_t* path_to_pinode(char* path, char* name)
{
    assert(name != NULL, "path_to_pinode: invalid NULL name buffer");
    return search_inode(path, name, true);
}

/**
 * @brief 查找或创建路径对应的inode
 * @param path 待解析的路径字符串（绝对路径/相对路径）
 * @param type 要创建的inode类型（FT_DIR/FT_FILE/FT_DEVICE）
 * @param major 主设备号（设备文件使用，普通文件填0）
 * @param minor 次设备号（设备文件使用，普通文件填0）
 * @return 成功返回inode指针，失败返回NULL
 */
inode_t* path_create_inode(char* path, uint16 type, uint16 major, uint16 minor)
{
    assert(path != NULL, "path_create_inode: invalid NULL path");
    assert(type >= FT_UNUSED && type <= FT_DEVICE, "path_create_inode: invalid inode type");

    char name[DIR_NAME_LEN];
    inode_t* pip = NULL;
    inode_t* ip = NULL;

    // 1. 获取父目录inode
    pip = path_to_pinode(path, name);
    if (pip == NULL) {
        printf("path_create_inode: cannot find parent directory for %s\n", path);
        return NULL;
    }

    inode_lock(pip);

    // 2. 检查目标inode是否已存在
    uint16 inum = dir_search_entry(pip, name);
    if (inum != INODE_NUM_UNUSED) {
        // 已存在，返回对应inode（校验类型匹配性）
        inode_unlock_free(pip);
        ip = inode_alloc(inum);
        inode_lock(ip);
        if (type != FT_DIR && ip->type == FT_DIR) {
            // 普通文件/设备文件 与 已存在的目录 类型冲突
            inode_unlock_free(ip);
            return NULL;
        }
        inode_unlock(ip);
        return ip;
    }

    // 3. 不存在，创建新inode
    ip = inode_create(type, major, minor);
    if (ip == NULL) {
        inode_unlock_free(pip);
        printf("path_create_inode: cannot create new inode for %s\n", path);
        return NULL;
    }

    // 4. 向父目录中添加新目录项
    if (dir_add_entry(pip, ip->inode_num, name) == BLOCK_SIZE) {
        // 添加失败，释放新创建的inode
        inode_lock(ip);
        ip->nlink = 0;
        inode_unlock_free(ip);
        inode_unlock_free(pip);
        printf("path_create_inode: cannot add dirent for %s (no space or duplicate)\n", path);
        return NULL;
    }

    // 5. 目录类型特殊处理：添加.（当前目录）和..（父目录）
    if (type == FT_DIR) {
        inode_lock(ip);
        dir_add_entry(ip, ip->inode_num, ".");    // 绑定当前目录inode
        dir_add_entry(ip, pip->inode_num, "..");  // 绑定父目录inode
        inode_unlock(ip);

        // 父目录链接数+1（目录的..会引用父目录）
        pip->nlink++;
        inode_rw(pip, true); // 同步父目录元数据到磁盘
    }

    // 6. 释放父目录引用，返回新创建的inode
    inode_unlock_free(pip);
    return ip;
}

/**
 * @brief 为已有文件创建硬链接（目录不支持链接）
 * @param old_path 源文件路径
 * @param new_path 新链接文件路径
 * @return 0表示成功，-1表示失败
 */
uint32 path_link(char* old_path, char* new_path)
{
    assert(old_path != NULL, "path_link: invalid NULL old path");
    assert(new_path != NULL, "path_link: invalid NULL new path");

    char name[DIR_NAME_LEN];
    inode_t* ip = NULL;
    inode_t* pip = NULL;

    // 1. 获取源文件inode
    ip = path_to_inode(old_path);
    if (ip == NULL) {
        printf("path_link: cannot find source file %s\n", old_path);
        return (uint32)-1;
    }

    inode_lock(ip);

    // 2. 校验：目录不支持硬链接
    if (ip->type == FT_DIR) {
        inode_unlock_free(ip);
        printf("path_link: directory %s cannot be linked\n", old_path);
        return (uint32)-1;
    }

    // 3. 获取新链接文件的父目录inode
    pip = path_to_pinode(new_path, name);
    if (pip == NULL) {
        inode_unlock_free(ip);
        printf("path_link: cannot find parent directory for %s\n", new_path);
        return (uint32)-1;
    }

    inode_lock(pip);

    // 4. 向父目录中添加新链接目录项
    if (dir_add_entry(pip, ip->inode_num, name) == BLOCK_SIZE) {
        inode_unlock_free(pip);
        inode_unlock_free(ip);
        printf("path_link: cannot add dirent for %s (no space or duplicate)\n", new_path);
        return (uint32)-1;
    }

    // 5. 源文件链接数+1，同步元数据到磁盘
    ip->nlink++;
    inode_rw(ip, true);

    // 6. 释放所有引用，返回成功
    inode_unlock_free(pip);
    inode_unlock_free(ip);
    return 0;
}

/**
 * @brief 检查目录unlink操作是否合法（是否为空目录，仅保留.和..）
 * @param ip 目录对应的inode指针（已上锁）
 * @return true表示合法，false表示非法
 * @note 调用者必须持有ip的睡眠锁，且ip必须是目录类型（FT_DIR）
 */
static bool check_unlink(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "check_unlink: slk");
    assert(ip->type == FT_DIR, "check_unlink: not a directory");

    uint8 tmp[sizeof(dirent_t) * 3];
    uint32 read_len;
    
    read_len = dir_get_entries(ip, sizeof(dirent_t) * 3, tmp, false);
    
    if(read_len == sizeof(dirent_t) * 3) {
        return false; // 包含超过2个有效目录项（非空目录）
    } else if(read_len == sizeof(dirent_t) * 2) {
        return true;  // 仅包含.和..（空目录）
    } else {
        panic("check_unlink: invalid read length");
        return false;
    }
}

/**
 * @brief 删除文件/目录的硬链接（删除目录项，更新链接数）
 * @param path 待删除链接的文件/目录路径
 * @return 0表示成功，-1表示失败
 */
uint32 path_unlink(char* path)
{
    assert(path != NULL, "path_unlink: invalid NULL path");

    char name[DIR_NAME_LEN];
    inode_t* pip = NULL;
    inode_t* ip = NULL;

    // 1. 获取父目录inode
    pip = path_to_pinode(path, name);
    if (pip == NULL) {
        printf("path_unlink: cannot find parent directory for %s\n", path);
        return (uint32)-1;
    }

    inode_lock(pip);

    // 2. 校验：禁止删除.和..目录项
    if (strncmp(name, ".", DIR_NAME_LEN) == 0 || 
        strncmp(name, "..", DIR_NAME_LEN) == 0) {
        inode_unlock_free(pip);
        printf("path_unlink: cannot delete . or ..\n");
        return (uint32)-1;
    }

    // 3. 查找待删除的目录项
    uint16 inum = dir_search_entry(pip, name);
    if (inum == INODE_NUM_UNUSED) {
        inode_unlock_free(pip);
        printf("path_unlink: cannot find %s\n", path);
        return (uint32)-1;
    }

    // 4. 获取目标inode并上锁
    ip = inode_alloc(inum);
    inode_lock(ip);

    // 5. 目录特殊处理：校验是否为空目录
    if (ip->type == FT_DIR) {
        if (!check_unlink(ip)) {
            inode_unlock_free(ip);
            inode_unlock_free(pip);
            printf("path_unlink: directory %s is not empty\n", path);
            return (uint32)-1;
        }
    }

    // 6. 从父目录中删除目录项
    dir_delete_entry(pip, name);

    // 7. 目录特殊处理：父目录链接数-1
    if (ip->type == FT_DIR) {
        pip->nlink--;
        inode_rw(pip, true); // 同步父目录元数据到磁盘
    }

    // 8. 释放父目录引用
    inode_unlock_free(pip);

    // 9. 目标inode链接数-1，同步元数据到磁盘
    ip->nlink--;
    inode_rw(ip, true);

    // 10. 释放目标inode引用，返回成功
    inode_unlock_free(ip);
    return 0;
}