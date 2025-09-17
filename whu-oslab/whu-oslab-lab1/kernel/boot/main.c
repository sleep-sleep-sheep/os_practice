#include "riscv.h"
#include "lib/print.h"
#include "proc/proc.h"  // 用于获取当前CPU核心ID (mycpuid())
#include "common.h"
volatile static int started = 0;

// 你原来的main函数：现在承担单核心的业务逻辑
int main() {
    int myid = mycpuid();  // 获取当前核心ID
    printf("cpu %d is booting!\n",myid);  // 打印单核心启动信息
    while (1);  // 业务循环（比如后续加任务调度）
}