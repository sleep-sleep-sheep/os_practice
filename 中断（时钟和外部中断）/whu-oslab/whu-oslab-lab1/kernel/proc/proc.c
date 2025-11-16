// proc/proc.c
#include "proc/proc.h"   // 包含 cpu_t 结构体、mycpuid/mycpu 声明
#include "riscv.h"       // 包含 RISC-V 寄存器操作（r_mhartid、r_tp、w_tp）
#include "common.h"      // 包含 panic 等错误处理函数
#include "lib/print.h"  // 包含打印调试信息的函数（panic 内部会用到）
// 全局核心私有数据数组：每个核心对应一个 cpu_t（NCPU 是最大核心数，需在 common.h 中定义）
static cpu_t cpus[NCPU];

/**
 * @brief 获取当前核心的私有数据（cpu_t）
 * @return 当前核心的 cpu_t 指针，若核心ID非法则触发 panic
 */
cpu_t* mycpu(void) {
    // 1. 从 tp 寄存器读取当前核心ID（tp 需在核心启动时初始化）
    /*tp 寄存器在 RISC-V 中通常用于存储线程指针（thread pointer）
        在多核处理器中，可以将其用来存储当前核心的 ID（hartid）
        这样每个核心可以通过读取 tp 寄存器快速获取自己的 ID
        这里假设在系统启动时，已经将每个核心的 hartid 写入了对应核心的 tp 寄存器
        例如，核心0的tp=0，核心1的tp=1，以此类推
    */ 
    uint64 hartid = r_tp();  // r_tp() 是 riscv.h 中定义的“读tp寄存器”函数
    
    // 2. 检查核心ID合法性（避免数组越界）
    if (hartid >= NCPU) {
        //panic("mycpu: invalid hartid %lld (NCPU = %d)", hartid, NCPU);
    }
    
    // 3. 返回当前核心的私有数据（cpus数组按核心ID索引）
    return &cpus[hartid];
}

/**
 * @brief 获取当前核心的ID（hartid）
 * @return 当前核心的ID（0 为主核，1、2... 为从核），若非法则触发 panic
 */
int mycpuid(void) {
    // 1. 从 tp 寄存器读取核心ID（与 mycpu 逻辑一致，确保一致性）
    uint64 hartid = r_tp();
    
    // 2. 检查核心ID合法性
    if (hartid >= NCPU) {
        //panic("mycpuid: invalid hartid %lld (NCPU = %d)", hartid, NCPU);
    }
    
    // 3. 返回核心ID（转为int，假设 NCPU 不超过 int 范围）
    return (int)hartid;
}