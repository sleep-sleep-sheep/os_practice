#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/dir.h"
#include "lib/str.h"
#include "lib/print.h"

// 超级块全局变量（你的原有代码）
super_block_t sb;
#define FS_MAGIC 0x12345678
#define SB_BLOCK_NUM 0

// 1. 补全文件读写测试所需的全局数组（2*BLOCK_SIZE）
char str[2 * BLOCK_SIZE];
char tmp[2 * BLOCK_SIZE];

// 2. 补全blockcmp函数（比较2*BLOCK_SIZE内存区域是否一致）
bool blockcmp(const char* buf1, const char* buf2) {
    for (int i = 0; i < 2 * BLOCK_SIZE; i++) {
        if (buf1[i] != buf2[i]) {
            return false;
        }
    }
    return true;
}

// 原有sb_print函数（保留）
static void sb_print()
{
    printf("\nsuper block information:\n");
    printf("magic = %x\n", sb.magic);
    printf("block size = %d\n", sb.block_size);
    printf("inode blocks = %d\n", sb.inode_blocks);
    printf("data blocks = %d\n", sb.data_blocks);
    printf("total blocks = %d\n", sb.total_blocks);
    printf("inode bitmap start = %d\n", sb.inode_bitmap_start);
    printf("inode start = %d\n", sb.inode_start);
    printf("data bitmap start = %d\n", sb.data_bitmap_start);
    printf("data start = %d\n", sb.data_start);
}

void fs_init()
{
    // ========== 前置：文件系统基础初始化（你的原有代码，保留） ==========
    buf_init();
    buf_t* buf = buf_read(SB_BLOCK_NUM);
    memmove(&sb, buf->data, sizeof(sb));
    assert(sb.magic == FS_MAGIC, "fs_init: magic error");
    assert(sb.block_size == BLOCK_SIZE, "fs_init: block size mismatch");
    buf_release(buf);
    sb_print();

    // ========== 测试1：文件读写测试（先执行，完整释放资源） ==========
    printf("\n=====================================");
    printf("\n开始：文件读写测试");
    printf("\n=====================================");
    inode_init(); // 初始化inode模块（测试1专用）
    uint32 ret = 0;

    // 步骤1：初始化测试数组str
    for (int i = 0; i < 2 * BLOCK_SIZE; i++) {
        str[i] = i;
    }

    // 步骤2：创建文件inode并加锁
    inode_t* nip = inode_create(FT_FILE, 0, 0);
    assert(nip != NULL, "fs_init: create inode fail");
    inode_lock(nip);

    // 步骤3：第一次查看inode初始状态
    inode_print(nip);

    // 步骤4：第一次写入（偏移0，长度BLOCK_SIZE/2）
    ret = inode_write_data(nip, 0, BLOCK_SIZE / 2, str, false);
    assert(ret == BLOCK_SIZE / 2, "fs_init: first write fail");

    // 步骤5：第二次写入（关键修正：计算剩余写入长度，避免断言失败）
    uint32 second_write_len = (2 * BLOCK_SIZE) - (BLOCK_SIZE / 2);
    ret = inode_write_data(nip, BLOCK_SIZE / 2, second_write_len, str + BLOCK_SIZE / 2, false);
    assert(ret == second_write_len, "fs_init: second write fail");

    // 步骤6：一次性读取完整数据到tmp
    ret = inode_read_data(nip, 0, 2 * BLOCK_SIZE, tmp, false);
    assert(ret == 2 * BLOCK_SIZE, "fs_init: read data fail");

    // 步骤7：第二次查看inode写入后状态
    inode_print(nip);

    // 步骤8：释放inode资源（解锁+释放，无残留）
    inode_unlock_free(nip);

    // 步骤9：验证读写结果并打印
    if (blockcmp(tmp, str) == true) {
        printf("\n[文件读写测试] 成功！\n");
    } else {
        printf("\n[文件读写测试] 失败！\n");
    }

    // ========== 测试2：路径/目录/文件测试（无残留，顺序执行） ==========
    printf("\n=====================================");
    printf("\n开始：路径/目录/文件测试");
    printf("\n=====================================");
    // 重新初始化inode模块（清理测试1的残留状态，可选，根据你的实现调整）
    inode_init();

    // 步骤1：创建所需inode（根目录+2个子目录+1个文件）
    inode_t* ip_root = inode_alloc(INODE_ROOT); // 根目录inode
    inode_t* ip_user = inode_create(FT_DIR, 0, 0); // user目录inode
    inode_t* ip_work = inode_create(FT_DIR, 0, 0); // work目录inode
    inode_t* ip_hello = inode_create(FT_FILE, 0, 0); // hello.txt文件inode
    // 校验inode创建结果
    assert(ip_root != NULL, "fs_init: alloc root inode fail");
    assert(ip_user != NULL, "fs_init: create user dir fail");
    assert(ip_work != NULL, "fs_init: create work dir fail");
    assert(ip_hello != NULL, "fs_init: create hello.txt fail");

    // 步骤2：所有inode加锁（保证操作安全）
    inode_lock(ip_root);
    inode_lock(ip_user);
    inode_lock(ip_work);
    inode_lock(ip_hello);

    // 步骤3：创建多级目录项（/ → user → work → hello.txt）
    dir_add_entry(ip_root, ip_user->inode_num, "user");
    dir_add_entry(ip_user, ip_work->inode_num, "work");
    dir_add_entry(ip_work, ip_hello->inode_num, "hello.txt");

    // 步骤4：向hello.txt写入数据（"hello world"）
    ret = inode_write_data(ip_hello, 0, 11, "hello world", false);
    assert(ret == 11, "fs_init: write hello.txt fail");

    // 步骤5：解锁inode（仅解锁，不释放，后续路径查找还需使用）
    inode_unlock(ip_hello);
    inode_unlock(ip_work);
    inode_unlock(ip_user);
    inode_unlock(ip_root);

    // 步骤6：路径查找（测试path_to_pinode和path_to_inode）
    char* test_path = "/user/work/hello.txt";
    char file_name[DIR_NAME_LEN] = {0}; // 存储最后一个文件名
    inode_t* tmp_pinode = path_to_pinode(test_path, file_name);
    inode_t* tmp_inode = path_to_inode(test_path);

    // 步骤7：校验路径查找结果
    assert(tmp_pinode != NULL, "fs_init: path_to_pinode return NULL");
    assert(tmp_inode != NULL, "fs_init: path_to_inode return NULL");
    printf("\n[路径测试] 找到文件名：%s\n", file_name);

    // 步骤8：打印tmp_pinode信息并释放
    inode_lock(tmp_pinode);
    printf("\n[tmp_pinode 信息]");
    inode_print(tmp_pinode);
    inode_unlock_free(tmp_pinode);

    // 步骤9：打印tmp_inode信息，读取文件内容并释放
    inode_lock(tmp_inode);
    printf("\n[tmp_inode 信息]");
    inode_print(tmp_inode);
    // 读取hello.txt内容
    char read_buf[12] = {0};
    ret = inode_read_data(tmp_inode, 0, tmp_inode->size, read_buf, false);
    assert(ret == 11, "fs_init: read hello.txt fail");
    printf("\n[文件读取测试] hello.txt 内容：%s\n", read_buf);
    inode_unlock_free(tmp_inode);

    // ========== 测试结束：打印最终结果 ==========
    printf("\n=====================================");
    printf("\n所有测试执行完毕！\n");
    printf("=====================================\n");

    // 调试用：暂停查看结果（可注释，后续删除）
    while (1);
}