// 这个头文件通常认为其他.h文件都应该include
#ifndef __COMMON_H__
#define __COMMON_H__

// 类型定义

typedef char                   int8;
typedef short                  int16;
typedef int                    int32;
typedef long long              int64;
typedef unsigned char          uint8; 
typedef unsigned short         uint16;
typedef unsigned int           uint32;
typedef unsigned long long     uint64;

typedef unsigned long long         reg; 
typedef enum {false = 0, true = 1} bool;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define NCPU 1 // 最大CPU核心数（需根据实际硬件调整）

//#define PGSIZE 4096 // 每页字节数
#define PGSHIFT 12  // 页内偏移位数
#define PG_ROUND_UP(sz)  (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))  // 向上页对齐（分配内存时用）
#define PG_ROUND_DOWN(va) (((uint64)(va)) & ~(PGSIZE - 1))      // 向下页对齐（取页基址时用）


#endif