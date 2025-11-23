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
        // Page is swapped on disk - swap in
        uint blockno = PTE2BLOCKNO(*pte);
        uint64 flags = PTE_FLAGS(*pte);

        // Allocate physical memory
        char *mem = kalloc();
        if (mem == 0)
        {
            return -1;
        }

        // Read page from disk
        begin_op();
        read_page_from_disk(ROOTDEV, mem, blockno);
        bfree_page(ROOTDEV, blockno);
        end_op();

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
