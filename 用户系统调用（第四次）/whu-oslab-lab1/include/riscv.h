#ifndef __RISCV_H__
#define __RISCV_H__

#include "common.h"

// -------------------------- 核心寄存器操作 --------------------------

// 读取当前CPU的hartid（mhartid寄存器）
static inline uint64 r_mhartid() {
    uint64 x;
    asm volatile("csrr %0, mhartid" : "=r"(x));
    return x;
}

// 读取/写入线程指针（tp寄存器，用于存储核心ID）
static inline uint64 r_tp() {
    uint64 x;
    asm volatile("mv %0, tp" : "=r"(x));
    return x;
}
static inline void w_tp(uint64 x) {
    asm volatile("mv tp, %0" : : "r"(x));
}

// 读取栈指针（sp寄存器）
static inline uint64 r_sp() {
    uint64 x;
    asm volatile("mv %0, sp" : "=r"(x));
    return x;
}

// 读取返回地址（ra寄存器）
static inline uint64 r_ra() {
    uint64 x;
    asm volatile("mv %0, ra" : "=r"(x));
    return x;
}

// -------------------------- Machine模式寄存器 --------------------------

// Machine Status Register (mstatus)
#define MSTATUS_MPP_MASK (3L << 11)  // 前序模式掩码
#define MSTATUS_MPP_M    (3L << 11)  // 前序模式为M-mode
#define MSTATUS_MPP_S    (1L << 11)  // 前序模式为S-mode
#define MSTATUS_MPP_U    (0L << 11)  // 前序模式为U-mode
#define MSTATUS_MIE      (1L << 3)   // M-mode中断使能位
#define MSTATUS_SIE      (1L << 1)   // S-mode中断使能位（全局）

static inline uint64 r_mstatus() {
    uint64 x;
    asm volatile("csrr %0, mstatus" : "=r"(x));
    return x;
}
static inline void w_mstatus(uint64 x) {
    asm volatile("csrw mstatus, %0" : : "r"(x));
}

// Machine Exception Program Counter (mepc)
static inline uint64 r_mepc() {
    uint64 x;
    asm volatile("csrr %0, mepc" : "=r"(x));
    return x;
}
static inline void w_mepc(uint64 x) {
    asm volatile("csrw mepc, %0" : : "r"(x));
}

// Machine Trap Vector Base Address (mtvec)
static inline uint64 r_mtvec() {
    uint64 x;
    asm volatile("csrr %0, mtvec" : "=r"(x));
    return x;
}
static inline void w_mtvec(uint64 x) {
    asm volatile("csrw mtvec, %0" : : "r"(x));
}

// Machine Interrupt Enable (mie)
#define MIE_MEIE (1L << 11)  // M-mode外部中断使能
#define MIE_MTIE (1L << 7)   // M-mode时钟中断使能
#define MIE_MSIE (1L << 3)   // M-mode软件中断使能

static inline uint64 r_mie() {
    uint64 x;
    asm volatile("csrr %0, mie" : "=r"(x));
    return x;
}
static inline void w_mie(uint64 x) {
    asm volatile("csrw mie, %0" : : "r"(x));
}

// Machine Scratch Register (mscratch)
static inline uint64 r_mscratch() {
    uint64 x;
    asm volatile("csrr %0, mscratch" : "=r"(x));
    return x;
}
static inline void w_mscratch(uint64 x) {
    asm volatile("csrw mscratch, %0" : : "r"(x));
}

// Machine Exception Delegation (medeleg)
// 委托异常给S-mode处理（bit对应异常类型）
#define MEDELEG_STORE_PAGE_FAULT (1L << 15)
#define MEDELEG_LOAD_PAGE_FAULT  (1L << 13)
#define MEDELEG_INST_PAGE_FAULT  (1L << 12)
#define MEDELEG_MMODE_ECALL      (1L << 11)
#define MEDELEG_SMODE_ECALL      (1L << 9)
#define MEDELEG_UMODE_ECALL      (1L << 8)

static inline uint64 r_medeleg() {
    uint64 x;
    asm volatile("csrr %0, medeleg" : "=r"(x));
    return x;
}
static inline void w_medeleg(uint64 x) {
    asm volatile("csrw medeleg, %0" : : "r"(x));
}

// Machine Interrupt Delegation (mideleg)
// 委托中断给S-mode处理（bit对应中断类型）
#define MIDELEG_SEIE (1L << 9)  // 外部中断委托
#define MIDELEG_STIE (1L << 5)  // 时钟中断委托
#define MIDELEG_SSIE (1L << 1)  // 软件中断委托

static inline uint64 r_mideleg() {
    uint64 x;
    asm volatile("csrr %0, mideleg" : "=r"(x));
    return x;
}
static inline void w_mideleg(uint64 x) {
    asm volatile("csrw mideleg, %0" : : "r"(x));
}

// Machine Counter-Enable (mcounteren)
static inline uint64 r_mcounteren() {
    uint64 x;
    asm volatile("csrr %0, mcounteren" : "=r"(x));
    return x;
}
static inline void w_mcounteren(uint64 x) {
    asm volatile("csrw mcounteren, %0" : : "r"(x));
}

// 读取时间计数器（time寄存器，用于时钟）
static inline uint64 r_time() {
    uint64 x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

// -------------------------- Supervisor模式寄存器 --------------------------

// Supervisor Status Register (sstatus)
#define SSTATUS_SPP  (1L << 8)  // 前序模式（1=S-mode，0=U-mode）
#define SSTATUS_SPIE (1L << 5)  // 前序中断使能（保存中断前的SIE状态）
#define SSTATUS_UPIE (1L << 4)  // 用户模式前序中断使能
#define SSTATUS_SIE  (1L << 1)  // S-mode中断使能（全局）
#define SSTATUS_UIE  (1L << 0)  // U-mode中断使能

static inline uint64 r_sstatus() {
    uint64 x;
    asm volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}
static inline void w_sstatus(uint64 x) {
    asm volatile("csrw sstatus, %0" : : "r"(x));
}

// Supervisor Exception Program Counter (sepc)
static inline uint64 r_sepc() {
    uint64 x;
    asm volatile("csrr %0, sepc" : "=r"(x));
    return x;
}
static inline void w_sepc(uint64 x) {
    asm volatile("csrw sepc, %0" : : "r"(x));
}

// Supervisor Trap Vector Base Address (stvec)
static inline uint64 r_stvec() {
    uint64 x;
    asm volatile("csrr %0, stvec" : "=r"(x));
    return x;
}
static inline void w_stvec(uint64 x) {
    asm volatile("csrw stvec, %0" : : "r"(x));
}

// Supervisor Interrupt Pending (sip)
#define SIP_SEIP (1L << 9)  // 外部中断 pending 位
#define SIP_STIP (1L << 5)  // 时钟中断 pending 位
#define SIP_SSIP (1L << 1)  // 软件中断 pending 位

static inline uint64 r_sip() {
    uint64 x;
    asm volatile("csrr %0, sip" : "=r"(x));
    return x;
}
static inline void w_sip(uint64 x) {
    asm volatile("csrw sip, %0" : : "r"(x));
}

// Supervisor Interrupt Enable (sie)
#define SIE_SEIE (1L << 9)  // 外部中断使能
#define SIE_STIE (1L << 5)  // 时钟中断使能
#define SIE_SSIE (1L << 1)  // 软件中断使能

static inline uint64 r_sie() {
    uint64 x;
    asm volatile("csrr %0, sie" : "=r"(x));
    return x;
}
static inline void w_sie(uint64 x) {
    asm volatile("csrw sie, %0" : : "r"(x));
}

// Supervisor Scratch Register (sscratch)
static inline uint64 r_sscratch() {
    uint64 x;
    asm volatile("csrr %0, sscratch" : "=r"(x));
    return x;
}
static inline void w_sscratch(uint64 x) {
    asm volatile("csrw sscratch, %0" : : "r"(x));
}

// Supervisor Trap Cause (scause)
static inline uint64 r_scause() {
    uint64 x;
    asm volatile("csrr %0, scause" : "=r"(x));
    return x;
}

// Supervisor Trap Value (stval)
static inline uint64 r_stval() {
    uint64 x;
    asm volatile("csrr %0, stval" : "=r"(x));
    return x;
}

// Supervisor Address Translation and Protection (satp)
#define SATP_SV39 (8L << 60)  // SV39页表模式
//#define MAKE_SATP(pt) (SATP_SV39 | (((uint64)(pt)) >> 12))  // 构造satp值

static inline uint64 r_satp() {
    uint64 x;
    asm volatile("csrr %0, satp" : "=r"(x));
    return x;
}
static inline void w_satp(uint64 x) {
    asm volatile("csrw satp, %0" : : "r"(x));
}

// -------------------------- 中断控制函数 --------------------------

// 使能S-mode中断（全局）
static inline void intr_on() {
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// 禁用S-mode中断（全局）
static inline void intr_off() {
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

// 检查S-mode中断是否使能
static inline int intr_get() {
    return (r_sstatus() & SSTATUS_SIE) != 0;
}

// 刷新TLB（页表缓存）
static inline void sfence_vma() {
    asm volatile("sfence.vma zero, zero");  // 刷新所有TLB条目
}



#endif  // __RISCV_H__
/*
// 内存管理相关

#define PGSIZE 4096 // bytes per page
#define PGSHIFT 12  // bits of offset within a page

#define PG_ROUND_UP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
#define PG_ROUND_DOWN(a) (((a)) & ~(PGSIZE-1))

#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // 1 -> user can access

// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK          0x1FF // 9 bits
#define PXSHIFT(level)  (PGSHIFT+(9*(level)))
#define PX(level, va)   ((((uint64) (va)) >> PXSHIFT(level)) & PXMASK)

// one beyond the highest possible virtual address.
// MAXVA is actually one bit less than the max allowed by
// Sv39, to avoid having to sign-extend virtual addresses
// that have the high bit set.
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))


*/



// =====================================
//  Machine-level CSR 读写函数补全
// =====================================

// 读写 mcause
static inline uint64 r_mcause() {
    uint64 x;
    asm volatile("csrr %0, mcause" : "=r"(x));
    return x;
}

// 读写 mtval
static inline uint64 r_mtval() {
    uint64 x;
    asm volatile("csrr %0, mtval" : "=r"(x));
    return x;
}



static inline void w_stimecmp(uint64 x) {
  // asm volatile("csrw stimecmp, %0" : : "r" (x));
  asm volatile("csrw 0x14d, %0" : : "r"(x));
}

static inline void w_menvcfg(uint64 x) {
  // asm volatile("csrw menvcfg, %0" : : "r" (x));
  asm volatile("csrw 0x30a, %0" : : "r"(x));
}
static inline uint64 r_menvcfg() {
  uint64 x;
  // asm volatile("csrr %0, menvcfg" : "=r" (x) );
  asm volatile("csrr %0, 0x30a" : "=r"(x));
  return x;
}