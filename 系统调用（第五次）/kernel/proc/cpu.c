#include "proc/cpu.h"
#include "riscv.h"

static cpu_t cpus[NCPU];

cpu_t* mycpu(void)
{
    int id =r_mhartid();
    return &cpus[id];
}

int mycpuid(void) 
{
    return r_mhartid();
}

proc_t* myproc(void)
{
    return mycpu()->proc;
}