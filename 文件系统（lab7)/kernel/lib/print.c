#include <stdarg.h>
#include "lib/print.h"
#include "lib/lock.h"
#include "dev/uart.h"

// 系统崩溃标志（volatile保证多核可见）
volatile int panicked = 0;

static spinlock_t print_lk;
static char digits[] = "0123456789abcdef";

void print_init(void)
{
    spinlock_init(&print_lk, "print");
}

// 打印整数（支持不同进制和符号）
static void printint(long long xx, int base, int sign)
{
    char buf[20];
    int i;
    unsigned long long x;
    // sign = 1表示需要考虑符号
    if (sign && (sign = (xx < 0)))
        x = -xx;
    else
        x = xx;
    
    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);
    
    if (sign)
        buf[i++] = '-';
    
    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

// 打印指针（16进制，带0x前缀）
static void printptr(unsigned long long x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < 16; i++, x <<= 4)
        uart_putc_sync(digits[x >> 60]);
}

// 主要的 printf 实现
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, c;
    char *s;
    
    spinlock_acquire(&print_lk);  // 获取锁，防止输出交错
    /*这是一个防御性编程习惯。char 在某些机器上是有符号的。
    如果不加这个，如果字符编码超过 127，可能会被当成负数处理，导致意外错误。
    这里强制把它看作 0-255 的无符号数*/
    va_start(ap, fmt);
    for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
        if (c != '%') {
            uart_putc_sync(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if (c == 0)
            break;
        switch (c) {
        case 'd':
            printint(va_arg(ap, int), 10, 1);
            break;
        case 'x':
            printint(va_arg(ap, int), 16, 0);
            break;
        case 'p':
            printptr(va_arg(ap, unsigned long long));
            break;
        case 's':
            if ((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for (; *s; s++)
                uart_putc_sync(*s);
            break;
        case '%':
            uart_putc_sync('%');
            break;
        default:
            uart_putc_sync('%');
            uart_putc_sync(c);
            break;
        }
    }
    va_end(ap);
    
    spinlock_release(&print_lk);  // 释放锁
}

void panic(const char *s)
{
    panicked = 1;
    printf("panic: %s\n", s);
    while (1)
        ;
}

void assert(bool condition, const char* warning)
{
    if (!condition) {
        panic(warning);
    }
}
