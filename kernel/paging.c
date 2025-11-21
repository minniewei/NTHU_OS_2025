#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "proc.h"
#include "vm.h"

// Access swap space defined in vm.c
extern char swap_space[];

int handle_pgfault(pagetable_t pagetable, uint64 va)
{
    struct proc *p = myproc();
    uint64 va0 = PGROUNDDOWN(va);

    // Check if fault VA is within valid user range
    if (va0 >= p->sz || va0 >= MAXVA)
    {
        return -1; // Illegal access
    }

    // Check if the page is swapped
    pte_t *pte = walk(pagetable, va0, 0);
    if (pte != 0 && (*pte & PTE_S))
    {
        // Page is swapped - swap in from memory swap space
        uint64 blockno = (*pte >> 12) & 0xFFFFFFFFF;
        uint64 flags = PTE_FLAGS(*pte);

        // Allocate physical memory
        char *mem = kalloc();
        if (mem == 0)
        {
            return -1;
        }

        // Copy from swap space (direct memory copy)
        memmove(mem, &swap_space[blockno * PGSIZE], PGSIZE);

        // Update PTE: set V bit, clear S bit
        flags &= ~PTE_S;
        flags |= PTE_V;
        *pte = PA2PTE(mem) | flags;

        return 0;
    }

    // Lazy allocation: allocate one page of physical memory
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
