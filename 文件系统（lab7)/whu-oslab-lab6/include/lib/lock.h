#ifndef __LOCK_H__
#define __LOCK_H__

#include "common.h"

typedef struct spinlock {
    int locked;
    char* name;
    int cpuid;
} spinlock_t;

void push_off();
void pop_off();

void spinlock_init(spinlock_t* lk, char* name);
void spinlock_acquire(spinlock_t* lk);
void spinlock_release(spinlock_t* lk);
bool spinlock_holding(spinlock_t* lk); 


// 睡眠锁
typedef struct sleeplock {
    int locked;         // 锁是否被持有
    spinlock_t lk;      // 保护睡眠锁的自旋锁
    char* name;         // 锁名称
    int pid;            // 持有锁的进程ID
} sleeplock_t;

void sleeplock_init(sleeplock_t* lk, char* name);
void sleeplock_acquire(sleeplock_t* lk);
void sleeplock_release(sleeplock_t* lk);
bool sleeplock_holding(sleeplock_t* lk);

#endif