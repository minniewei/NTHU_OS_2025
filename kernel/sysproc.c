#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int n;
  struct proc *p = myproc();
  uint64 oldsz = p->sz;

  argint(0, &n);

  // n > 0 : just increase heap size
  if (n > 0)
  {
    uint64 newsz = oldsz + n;
    // Check for overflow or excessively large size if needed
    if (newsz < oldsz)
      return -1;
    if (newsz >= MAXVA)
      return -1;
    // Check heap limit (128MB)
    if (newsz > (128L << 20))
      return -1;
    p->sz = newsz;
    return oldsz;
  }

  // n == 0 : just return current size
  if (n == 0)
  {
    return oldsz;
  }

  // n < 0 : decrease heap size
  if (n < 0)
  {
    uint64 newsz = oldsz + n;
    if (newsz > oldsz)
      return -1;
    if (newsz < 0)
      return -1;

    // Actually deallocate pages here; only existing pages will be freed
    newsz = uvmdealloc(p->pagetable, oldsz, newsz);
    p->sz = newsz;
    return oldsz;
  }

  // Should not reach here
  return -1;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (killed(myproc()))
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

#ifdef LAB_PGTBL
int sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
