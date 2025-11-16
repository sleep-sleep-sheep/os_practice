// 字符串和内存操作函数头文件
// 声明内存操作、字符串处理等基础工具函数

#ifndef STR_H
#define STR_H  // 防止头文件重复包含

#include "common.h"  // 引入自定义类型（uint, uchar等）

// 内存操作函数声明
void* memset(void *dst, int c, uint32 n);
int   memcmp(const void *v1, const void *v2, uint32 n);
void* memmove(void *dst, const void *src, uint32 n);
void* memcpy(void *dst, const void *src, uint32 n);

// 字符串操作函数声明
int   strncmp(const char *p, const char *q, uint32 n);
char* strncpy(char *s, const char *t, int n);
char* safestrcpy(char *s, const char *t, int n);
int   strlen(const char *s);

#endif  // STR_H
