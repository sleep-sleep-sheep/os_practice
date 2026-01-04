#ifndef __PMEM_H__
#define __PMEM_H__

#include "common.h"

// 来自kernel.ld
extern char KERNEL_DATA[];
extern char ALLOC_BEGIN[];
extern char ALLOC_END[];
/*跨越 Linker 和 Compiler 的边界传递地址信息,如果设置为uint64C 编译器会认为：
ALLOC_BEGIN 是一个存放在某处的 uint64 变量。
链接器告诉编译器，这个变量存储在地址 X。
编译器会去地址 X 读取 8个字节的内容。 但这不对！ 我们不想要地址 X 里的内容（那里可能是乱码），我们要的是 X 这个地址本身
*/

void  pmem_init(void);
void* pmem_alloc(bool in_kernel);
void  pmem_free(uint64 page, bool in_kernel);

#endif