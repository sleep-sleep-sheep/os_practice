// 标准输出和报错机制
#include <stdarg.h>
#include "lib/print.h"
#include "lib/lock.h"
#include "dev/uart.h"
#include "riscv.h"  // 用于中断控制（intr_off()）
#include "common.h" // 用于 bool 类型定义

volatile int panicked = 0;  // 标记是否已触发 panic（避免递归 panic）
static spinlock_t print_lk; // 保护 UART 输出的自旋锁
static char digits[] = "0123456789abcdef"; // 进制转换用的字符表


/**
 * @brief 初始化打印模块（初始化自旋锁）
 * 需在 UART 初始化（uart_init()）之后调用
 */
void print_init(void) {
    spinlock_init(&print_lk, "print_lock"); // 初始化打印锁，命名为 "print_lock"（调试用）
}


/**
 * @brief 内部辅助函数：将整数按指定进制打印到 UART
 * @param num 要打印的整数（支持无符号 64 位）
 * @param base 进制（如 10 表示十进制，16 表示十六进制）
 */
static void print_int(uint64 num, int base) {
    char buf[64];  // 存储转换后的字符（64 位整数最大需要 20 位十进制，足够容纳）
    int idx = 0;

    // 特殊情况：num 为 0 时直接打印 '0'
    if (num == 0) {
        buf[idx++] = '0';
    } else {
        // 按进制转换（逆序存储到 buf）
        while (num > 0) {
            buf[idx++] = digits[num % base]; // 取余得到当前位，对应 digits 中的字符
            num = num / base;                // 整除推进到下一位
        }
    }

    // 逆序输出（因为转换时是从低位到高位存储）
    while (idx > 0) {
        uart_putc_sync(buf[--idx]); // 调用 UART 同步发送函数（已加锁保护）
    }
}


/**
 * @brief 内部辅助函数：打印字符串到 UART
 * @param s 要打印的字符串（需以 '\0' 结尾）
 */
static void print_str(const char *s) {
    if (s == NULL) {
        s = "(null)"; // 处理 NULL 字符串，避免崩溃
    }
    // 遍历字符串，逐个字符发送
    while (*s != '\0') {
        uart_putc_sync(*s++);
    }
}


/**
 * @brief 格式化打印函数（支持 %d、%x、%lx、%p、%s）
 * @param fmt 格式化字符串
 * @param ... 可变参数（对应 fmt 中的格式符）
 */
void printf(const char *fmt, ...) {
    // 1. 获取可变参数列表
    va_list args;
    va_start(args, fmt);

    // 2. 加锁保护 UART 输出（多核环境下避免乱码）
    spinlock_acquire(&print_lk);

    // 3. 解析格式化字符串
    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            // 非格式符：直接打印当前字符
            uart_putc_sync(fmt[i]);
            continue;
        }

        // 遇到格式符 '%'，解析后续的类型（支持 %l 修饰符）
        i++; // 跳过 '%'，查看下一个字符
        // 新增：判断是否有 'l' 修饰符（如 %lx）
        int is_long = 0;
        if (fmt[i] == 'l') {
            is_long = 1;  // 标记为 long 类型
            i++;          // 跳过 'l'，继续解析后续格式符
        }

        // 根据格式符处理（支持 %x 和 %lx 共用逻辑）
        switch (fmt[i]) {
            case 'd': {
                // 十进制整数：读取 int/long 类型，转为 uint64 处理（支持负数）
                long num;
                if (is_long) {
                    num = va_arg(args, long); // 处理 %ld（long 类型）
                } else {
                    num = va_arg(args, int);  // 处理 %d（int 类型）
                }
                if (num < 0) {
                    uart_putc_sync('-'); // 负数先打印 '-'
                    print_int((uint64)(-num), 10);
                } else {
                    print_int((uint64)num, 10);
                }
                break;
            }
            case 'x':
                // 十六进制整数（小写）：读取 uint64 类型（%x 和 %lx 统一处理，因 print_int 已支持 64 位）
                print_int(va_arg(args, uint64), 16);
                break;
            case 'p':
                // 指针：等同于十六进制，先打印 "0x" 前缀
                uart_putc_sync('0');
                uart_putc_sync('x');
                print_int((uint64)va_arg(args, void*), 16);
                break;
            case 's':
                // 字符串：读取 char* 类型（%ls 与 %s 逻辑一致，无需额外处理）
                print_str(va_arg(args, char*));
                break;
            default:
                // 不支持的格式符：回溯并打印 "%+修饰符+未知字符"（如 %l?）
                if (is_long) {
                    uart_putc_sync('l'); // 补回 'l' 修饰符
                    i--;                  // 回溯索引，确保后续打印未知字符正确
                }
                uart_putc_sync('%');
                uart_putc_sync(fmt[i]);
                break;
        }
    }

    // 4. 释放锁，结束可变参数列表
    spinlock_release(&print_lk);
    va_end(args);
}


/**
 * @brief 恐慌函数（程序遇到致命错误时调用）
 * 功能：打印错误信息、禁用中断、标记 panicked、死循环
 * @param s 错误信息
 */
void panic(const char *s) {
    // 避免递归 panic（如果在 panic 过程中再次调用 panic，直接死循环）
    if (panicked) {
        goto dead;
    }
    panicked = 1; // 标记已触发 panic

    // 1. 禁用所有中断（防止 panic 过程中被打断）
    intr_off();

    // 2. 打印 panic 信息（加锁确保输出完整，且仅申请一次锁）
    spinlock_acquire(&print_lk);
    // 直接用 print_str 打印，不调用 printf（避免二次申请锁）
    print_str("\npanic: ");  // 替代 printf("\npanic: ");
    print_str(s);             // 打印具体错误信息
    print_str("\n");          // 替代 printf("\n");（换行）
    spinlock_release(&print_lk);

    // 3. 死循环（不再继续执行程序）
dead:
    while (1) {
        // 可选：添加调试信息（同样用 print_str，避免 printf）
        // 示例：print_str("hart x: panic loop\n");
    }
}

/**
 * @brief 断言函数（验证条件是否成立，不成立则触发 panic）
 * @param condition 要验证的条件（true 表示正常，false 表示断言失败）
 * @param warning 断言失败时的提示信息
 */
void assert(bool condition, const char* warning) {
    if (!condition) {
        // 条件不成立：打印断言位置（如果 warning 包含文件名和行号会更友好）
        panic(warning);
    }
}  