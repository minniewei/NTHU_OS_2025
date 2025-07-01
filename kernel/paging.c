#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "proc.h"

/* Page fault handler */
int handle_pgfault()
{
    /* Find the address that caused the fault */
    /* uint64 va = r_stval(); */

    /* mp2 TODO */
    panic("not implemented yet\n");
}
