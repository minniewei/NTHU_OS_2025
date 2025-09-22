#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h" // 檔名在 xv6-riscv 通常是 kernel/sysinfo.h

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
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
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

// (HW1-1) implement trace system call
uint64
sys_trace(void)
{
  int mask;
  argint(0, &mask);
  myproc()->trace_mask = mask; // 存到這個 process 的 trace_mask
  return 0;
}

// (HW1-2) implement sysinfo system call
uint64
sys_sysinfo(void)
{
  uint64 uaddr;
  struct sysinfo si;

  // 這版 argaddr 不回傳值，直接把參數取出
  argaddr(0, &uaddr);

  si.freemem = getfreemem();
  si.nproc = getnproc();

  // 只有 copyout 會回報錯誤
  if (copyout(myproc()->pagetable, uaddr, (char *)&si, sizeof(si)) < 0)
    return -1;

  return 0;
}