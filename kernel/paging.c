#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

int handle_pgfault(pagetable_t pagetable, uint64 va)
{
    struct proc *p = myproc();
    uint64 va0 = PGROUNDDOWN(va);

    // Check if fault VA is within valid user range
    if (va0 >= p->sz || va0 >= MAXVA)
    {
        return -1; // Illegal access
    }

    // Allocate one page of physical memory
    char *mem = kalloc();
    if (mem == 0)
    {
        return -1;
    }
    memset(mem, 0, PGSIZE);

    // Create mapping: VA → PA
    if (mappages(pagetable, va0, PGSIZE, (uint64)mem,
                 PTE_R | PTE_W | PTE_X | PTE_U) < 0)
    {
        kfree(mem);
        return -1;
    }

    return 0;
}
