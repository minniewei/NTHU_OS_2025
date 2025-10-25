# OS25-mp2
## Setup  
```
# 將檔案從 branch os-mp1 clone 下來
git clone -b os25-mp2 https://git.lsalab.cs.nthu.edu.tw/os25/os25_team61_xv6

# 進到 os25_team61_xv6 資料夾
cd .\os25_team61_xv6\

# 開啟 docker
docker run -it --rm -v ${PWD}:/xv6 -v /xv6/mkfs -v ${PWD}/mkfs/mkfs.c:/xv6/mkfs/mkfs.c -w /xv6 --platform linux/amd64 dasbd72/xv6:amd64

# 執行測試檔 (./grade-mp1-public)
 ./grade-mp2-public
```
## Trace Code
### Setup timer interrupt
#### timerinit() 是在 machine mode 下執行的初始化程式。它會幫每個 CPU（也就是每個 hart）設定自己的 timer，並且告訴硬體過多久要觸發下一次 timer interrupt。接著它會把一些參數放進 mscratch，這是給後面 timervec 用的，最後設定中斷處理的入口（w_mtvec(timervec)），並開啟 machine mode 的中斷權限。

```
void
timerinit()
{
  // each CPU has a separate source of timer interrupts.
  int id = r_mhartid();

  // ask the CLINT for a timer interrupt.
  int interval = 10000; // cycles; about 1/10th second in qemu.
  *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;

  // prepare information in scratch[] for timervec.
  // scratch[0..2] : space for timervec to save registers.
  // scratch[3] : address of CLINT MTIMECMP register.
  // scratch[4] : desired interval (in cycles) between timer interrupts.
  uint64 *scratch = &timer_scratch[id][0];
  scratch[3] = CLINT_MTIMECMP(id);
  scratch[4] = interval;
  w_mscratch((uint64)scratch);

  // set the machine-mode trap handler.
  w_mtvec((uint64)timervec);

  // enable machine-mode interrupts.
  w_mstatus(r_mstatus() | MSTATUS_MIE);

  // enable machine-mode timer interrupts.
  w_mie(r_mie() | MIE_MTIE);
}
```
#### 當CPU 真的收到中斷訊號時，會進入 machine mode，跳到 kernel/kernelvec.S 裡的 timervec 這個標籤。這段程式主要做三件事：1. 先把暫存器（a1、a2、a3）暫存起來，避免資料被破壞。2. 幫我們排好下一次中斷（再把 interval 加上去）。3. 然後設定一個 Supervisor Software Interrupt (SSIP)，意思是通知作業系統的 S-mode該處理時脈中斷了。做完這些之後，它就用 mret 回到原本的模式。
```
.globl timervec
.align 4
timervec:
        # start.c has set up the memory that mscratch points to:
        # scratch[0,8,16] : register save area.
        # scratch[24] : address of CLINT's MTIMECMP register.
        # scratch[32] : desired interval between interrupts.
        
        csrrw a0, mscratch, a0
        sd a1, 0(a0)
        sd a2, 8(a0)
        sd a3, 16(a0)

        # schedule the next timer interrupt
        # by adding interval to mtimecmp.
        ld a1, 24(a0) # CLINT_MTIMECMP(hart)
        ld a2, 32(a0) # interval
        ld a3, 0(a1)
        add a3, a3, a2
        sd a3, 0(a1)

        # arrange for a supervisor software interrupt
        # after this handler returns.
        li a1, 2
        csrw sip, a1

        ld a3, 16(a0)
        ld a2, 8(a0)
        ld a1, 0(a0)
        csrrw a0, mscratch, a0

        mret
```
### User space interrupt handler
#### usertrapret() 會把 stvec 改回 user 中斷入口（trampoline 的 uservec）、設好 sepc/sstatus 等暫存器，並呼叫 trampoline 的 userret 回到 user mode。
```
void
usertrapret(void)
{
  struct proc *p = myproc();

  intr_off();  // 關中斷，避免切換過程被打斷

  // 設定 user mode 的中斷入口為 trampoline 的 uservec
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // 設定 trapframe 中的 kernel 相關資訊（回來時要用）
  p->trapframe->kernel_satp = r_satp();
  p->trapframe->kernel_sp = p->kstack + PGSIZE;
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();

  // 設定 sstatus 與 sepc，讓 sret 回到 user mode 時正確執行
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP;  // 清 SPP，代表回 user mode
  x |= SSTATUS_SPIE;  // 開啟 user mode 中斷
  w_sstatus(x);
  w_sepc(p->trapframe->epc);

  // 設定 user page table 並跳到 trampoline 的 userret（執行 sret 回 user mode）
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))fn)(satp);
}
```
#### uservec 會儲存使用者暫存器到目前行程的 trapframe，再呼叫 usertrap() 進入 C。
```
.globl uservec
uservec:
    # a0 存進 sscratch，之後要拿來當 TRAPFRAME 基底
    csrw sscratch, a0

    # 每個 process 都有自己的 trapframe，但位址相同
    li a0, TRAPFRAME

    # 儲存使用者暫存器到 trapframe
    sd ra, 40(a0)
    sd sp, 48(a0)
    ...
    sd t6, 280(a0)

    # 初始化 kernel stack 與 hartid
    ld sp, 8(a0)
    ld tp, 32(a0)

    # 取得 kernel_trap (即 usertrap() 函式位址)
    ld t0, 16(a0)

    # 切換到 kernel page table
    ld t1, 0(a0)
    sfence.vma zero, zero
    csrw satp, t1
    sfence.vma zero, zero

    # 跳到 usertrap()，進入 C 的 trap handler
    jr t0

```
#### usertrap()判斷是 syscall 還是裝置中斷還是都不是。若是裝置中斷，呼叫 devintr()。
```
void
usertrap(void)
{
  struct proc *p = myproc();
  int which_dev = 0;

  // 改 stvec 指向 kernelvec，因為現在進到 kernel 了
  w_stvec((uint64)kernelvec);

  // 儲存 user 程式的 PC
  p->trapframe->epc = r_sepc();

  // 判斷是哪一種中斷或例外
  if((which_dev = devintr()) != 0){
    // 如果是裝置中斷（例如 timer）
  } else {
    setkilled(p);
  }

  // 若是 timer 中斷（which_dev == 2），就讓出 CPU
  if(which_dev == 2)
    yield();

  // 準備回到 user mode
  usertrapret();
}
```
#### devintr() 讀 scause 判斷來源。若是timer（通常是 SSIP or STIP）就呼叫 clockintr()。
```
int
devintr(void)
{
  uint64 scause = r_scause();

  // Supervisor external interrupt（例如 UART、DISK）
  if((scause & 0x8000000000000000L) && (scause & 0xff) == 9){
    int irq = plic_claim();
    if(irq == UART0_IRQ)
      uartintr();
    else if(irq == VIRTIO0_IRQ)
      virtio_disk_intr();
    if(irq)
      plic_complete(irq);
    return 1;
  }

  // Software interrupt（來自 machine-mode timervec）
  else if(scause == 0x8000000000000001L){
    if(cpuid() == 0)
      clockintr();             // 呼叫 timer interrupt 處理函式

    // 清除 SSIP (Supervisor Software Interrupt Pending)
    w_sip(r_sip() & ~2);
    return 2;
  }

  return 0;
}
```
#### 每次中斷來了，ticks（系統時鐘）就加一。然後 wakeup(&ticks) 會喚醒那些在 sys_sleep() 裡等時間的 process。
```
void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}
```
### Kernel space interrupt handler
#### 當 CPU 在 kernel 模式下執行時發生中斷，CPU 就會直接跳到這裡。這段程式會做三件事：1. 先把暫存器都存起來，避免中斷打亂目前的執行狀態。2. 呼叫 kerneltrap()，讓 C 函式去判斷中斷的來源、做對應的處理（像是呼叫 devintr()、clockintr()）。3. 處理完中斷後，再把暫存器還原，用 sret 回到中斷發生前的地方，讓系統繼續執行原本的 kernel 代碼。
```
.globl kernelvec
kernelvec:
    # 在堆疊上騰出空間，準備存暫存器
    addi sp, sp, -256

    # 儲存所有暫存器（節錄）
    sd ra, 0(sp)
    sd sp, 8(sp)
    ...
    sd t6, 240(sp)

    # 呼叫 C 的中斷處理函式
    call kerneltrap

    # 還原暫存器（節錄）
    ld ra, 0(sp)
    ld sp, 8(sp)
    ...
    ld t6, 240(sp)

    addi sp, sp, 256

    # 回到中斷前的 kernel 指令
    sret
```
#### kerneltrap() 會先讀取目前的暫存器狀態（sepc, sstatus），然後呼叫 devintr() 判斷是哪一種中斷，如果是 timer interrupt（which_dev == 2），就呼叫 yield() 把 CPU 讓出來，讓 scheduler 能執行別的 process。

```
void 
kerneltrap(void)
{
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  int which_dev = devintr();   // 判斷是哪一種中斷

  // 如果是 timer 中斷，讓出 CPU
  if(which_dev == 2 && myproc() && myproc()->state == RUNNING)
    yield();

  // 還原暫存的狀態
  w_sepc(sepc);
  w_sstatus(sstatus);
}
```
#### devintr() 讀 scause 判斷來源。若是timer（通常是 SSIP or STIP）就呼叫 clockintr()。
```
int
devintr(void)
{
  uint64 scause = r_scause();

  // Supervisor external interrupt（例如 UART、DISK）
  if((scause & 0x8000000000000000L) && (scause & 0xff) == 9){
    int irq = plic_claim();
    if(irq == UART0_IRQ)
      uartintr();
    else if(irq == VIRTIO0_IRQ)
      virtio_disk_intr();
    if(irq)
      plic_complete(irq);
    return 1;
  }

  // Software interrupt（來自 machine-mode timervec）
  else if(scause == 0x8000000000000001L){
    if(cpuid() == 0)
      clockintr();             // 呼叫 timer interrupt 處理函式

    // 清除 SSIP (Supervisor Software Interrupt Pending)
    w_sip(r_sip() & ~2);
    return 2;
  }

  return 0;
}
```
#### 每次中斷來了，ticks（系統時鐘）就加一。接著 wakeup(&ticks) 會喚醒那些在 sys_sleep() 裡等時間的 process。
```
void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}
```
### the mapping relationship 
```
| xv6 狀態(`enum procstate`) |Lecture 狀態 | 說明                       
| ------------------------- | ---------- | --------------------       
| `USED`                    | New        | 剛建立，尚未進入 ready queue  
| `RUNNABLE`                | Ready      | 等待被 CPU 選中執行           
| `RUNNING`                 | Running    | 目前佔用 CPU 執行            
| `SLEEPING`                | Waiting    | 等待事件或 I/O 完成          
| `ZOMBIE`                  | Terminated | 執行完畢，等待父行程回收       

```
### New -> Ready
#### userinit() 會呼叫 allocproc() 去 process table 找出一個空位（UNUSED），並分配 kernel stack 給這個新 process。這時候 process 的狀態會從 UNUSED → USED。接著 userinit() 幫這個 process 建立好使用者空間，包括 page table、stack、初始程式碼 initcode 等。這些設好之後，process 的狀態被設成 RUNNABLE。最後呼叫 pushreadylist（p），把這個 process 放入 ready queue，等 scheduler 來挑。
```
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  procstatelog(p);
  pushreadylist(p);

  release(&p->lock);
}
```
```
static struct proc*
allocproc(void)
{
  struct proc *p;

  // 尋找一個 UNUSED 的 process slot
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED)
      goto found;
    else
      release(&p->lock);
  }
  return 0;

found:
  p->pid = allocpid();        // 分配唯一的 process ID
  p->state = USED;

  // 分配 trapframe 給這個行程（用來儲存 user 狀態）
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 建立一個新的 page table
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 設定 context，讓 process 未來從 forkret 開始執行
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}
```
### New -> Ready (via fork)
#### 當使用者呼叫 fork() 時，會進到 kernel 裡的 sys_fork()，接著呼叫內部的 fork()。然後fork() 會去呼叫 allocproc()，分配 trapframe、page table，設定基本的初始狀態（state = USED）。當初始化完成後，會把子 process 的狀態設成 RUNNABLE，再呼叫 pushreadylist(np) 把它加進 ready queue。
```
uint64
sys_fork(void)
{
  return fork();
}
```
```
// proc.c
int fork(void)
{
  struct proc *np;
  struct proc *p = myproc();

  // 建立新的 process 結構
  if((np = allocproc()) == 0)
    return -1;

  np->priority = 149;           // 初始化優先權
  np->statelogenabled = 0;      // 關閉狀態記錄（預設）

  // 複製父行程的使用者記憶體
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  *(np->trapframe) = *(p->trapframe); // 複製暫存器狀態
  np->trapframe->a0 = 0;              // 子 fork() 回傳 0

  // 複製開啟的檔案與工作目錄
  for(int i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  // 設定父子關係
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // 狀態改為 RUNNABLE 並放入 ready queue
  acquire(&np->lock);
  procstatelog(np);             // 記錄狀態變化
  np->state = RUNNABLE;
  procstatelog(np);
  pushreadylist(np);            // 加入 ready queue
  release(&np->lock);

  return np->pid;
}
```
### Running -> Ready
#### process 在 usertrap() 或 kerneltrap() 裡收到 timer interrupt、或主動呼叫 sys_yield() 時，會進入 yield()。yield() 裡面的 pushreadylist（p） 會把目前的 process 放回 ready queue，然後把狀態改成 RUNNABLE。
```
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  procstatelog(p);
  pushreadylist(p);
  sched();
  release(&p->lock);
}
```
#### 　sched() 會呼叫 swtch()，把目前 process 的暫存器狀態儲存下來，並切換到 CPU 的 scheduler context。之後 CPU 就會執行 scheduler，從 ready queue 選出下一個要執行的 process。
```
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}
```
```
# kernel/switch.S
.globl swtch
swtch:
    sd ra, 0(a0)
    sd sp, 8(a0)
    ...
    ld ra, 0(a1)
    ld sp, 8(a1)
    ...
    ret
```
### Running -> Waiting
#### 使用者透過系統呼叫 sleep(n) 要求暫停。Kernel 拿到 n（等待的時間），用 ticks 作為計時的基準。進入迴圈檢查是否時間到了，還沒到就呼叫 sleep(&ticks, &tickslock)。
```
uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);     // 睡覺
  }
  release(&tickslock);
  return 0;
}
```
#### sleep()會把目前 process 狀態改成 SLEEPING，指定它正在等待的事件（chan = &ticks），釋放原本的鎖（例如 tickslock），然後呼叫 sched() 把 CPU 交出去。
```
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  struct channel *cn;
  struct proclistnode *pn;

  acquire(&p->lock);

  // 找到或建立一個對應的 channel（等待的事件）
  if((cn = findchannel(chan)) == 0 && (cn = allocchannel(chan)) == 0)
    panic("sleep: allocchannel");

  release(lk);                   // 釋放外部鎖（例如 tickslock）

  // 進入睡眠狀態
  p->chan = chan;
  p->state = SLEEPING;
  procstatelog(p);               // 紀錄狀態變化

  // 將 process 加入該 channel 的等待佇列
  if((pn = allocproclistnode(p)) == 0)
    panic("sleep: allocproclistnode");
  pushbackproclist(&cn->pl, pn); // 加入 channel list
  release(&cn->lock);

  sched();                       // 交出 CPU（context switch）

  // 被喚醒後的清理
  p->chan = 0;
  release(&p->lock);
  acquire(lk);
}
```
#### 　sched() 會呼叫 swtch()，把目前 process 的暫存器狀態儲存下來，並切換到 CPU 的 scheduler context。之後 CPU 就會執行 scheduler，從 ready queue 選出下一個要執行的 process。
```
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}
```
### Waiting -> Ready
#### 　每當 timer interrupt 發生時，系統會進入 clockintr()，把 ticks 加一，接著呼叫 wakeup(&ticks)，表示那些在等待的 process 該起床了。
```
void
clockintr()
{
  acquire(&tickslock);
  ticks++;                  // 每次時鐘中斷 ticks++
  wakeup(&ticks);           // 喚醒等待時間到的行程
  release(&tickslock);
}
```
#### wakeup() 會找到對應的 channel（例如 &ticks），掃描這個 channel 裡所有正在等待的 process。若某個 process 的 state == SLEEPING 且 chan == &ticks，就把它的狀態改成 RUNNABLE，然後呼叫 procstatelog（p）記錄狀態改變，最後用 pushreadylist（p） 加入 ready queue。
```
void
wakeup(void *chan)
{
  struct proc *p;
  struct channel *cn;
  struct proclistnode *pn;

  // 找出對應的 channel
  if((cn = findchannel(chan)) == 0)
    return;

  // 依序取出該 channel 內所有等待的 process
  while((pn = popfrontproclist(&cn->pl)) != 0){
    p = pn->p;
    freeproclistnode(pn);

    acquire(&p->lock);
    if(p == myproc())
      panic("wakeup: wakeup self");
    if(p->state != SLEEPING)
      panic("wakeup: not sleeping");
    if(p->chan != chan)
      panic("wakeup: wrong channel");

    p->state = RUNNABLE;        // 改為可執行狀態
    procstatelog(p);            // 記錄狀態改變
    pushreadylist(p);           // 放回 ready queue
    release(&p->lock);
  }

  // channel 清空後釋放
  cn->used = 0;
  release(&cn->lock);
}
```
### Running -> Terminated
#### 當一個 process 呼叫 exit()時，會先關閉所有開啟的檔案和工作目錄。如果這個 process 有子 process ，就把它們的 parent 改成系統的 initproc。有些父 process 可能正在呼叫 wait() 等子 process 結束，wakeup(p->parent) 就會通知父 process 說子 process 結束了，可以收屍了。然後去設定自己的 state = ZOMBIE。最後呼叫 sched() 交出 CPU。
```
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // 關閉所有開啟的檔案
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  // 關閉工作目錄
  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 把子 process 交給 init
  reparent(p);

  // 喚醒可能在 wait() 的父 process 
  wakeup(p->parent);

  acquire(&p->lock);
  p->xstate = status;        // 儲存退出碼
  p->state = ZOMBIE;         // 設為終止狀態
  procstatelog(p);           // 記錄狀態改變
  release(&wait_lock);

  // 交出 CPU，不再回來
  sched();
  panic("zombie exit");
}
```
#### 　sched() 會呼叫 swtch()，把目前 process 的暫存器狀態儲存下來，並切換到 CPU 的 scheduler context。之後 CPU 就會執行 scheduler，從 ready queue 選出下一個要執行的 process。
```
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}
```
### Ready -> Running
#### 每個 CPU 上都會有一個獨立的 scheduler()，它不斷地在找誰可以執行。popreadylist() 會取出最前面的 RUNNABLE process，如果沒有，就繼續等待。如果有的話，就將 process 狀態改為 RUNNING，呼叫 procstatelog（p）記下狀態轉換。swtch(&c->context, &p->context) 會保存 CPU 目前的 context，然後載入這個 process 的 context，這時 CPU 就開始執行它的程式碼。等 process 主動放棄 CPU（例如呼叫 yield()、sleep() 或結束 exit()），會再透過另一個 swtch() 回到這裡，scheduler 再選下一個 process 接著跑。
```
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    // 開啟中斷，避免系統卡死
    intr_on();

    // 從 ready queue 取出一個可執行的行程
    if((p = popreadylist()) == 0)
      continue;

    acquire(&p->lock);
    if(p->state != RUNNABLE)
      panic("scheduler: p->state != RUNNABLE");

    // 記錄 process 開始執行的時間
    p->startrunningticks = ticks;

    // 切換狀態為 RUNNING
    p->state = RUNNING;
    c->proc = p;
    procstatelog(p);

    // 進行 context switch，交出 CPU 給該 process
    swtch(&c->context, &p->context);

    // 當該 process 結束或被搶走 CPU 時回來
    c->proc = 0;
    release(&p->lock);
  }
}
```

## Implement
### ----- 定義變數與函式 -----
#### (proc.h) 定義 struct proclist(L3)、sortedproclist(L1、L2)
```
struct proclist {
  int size;
  struct proclistnode buf[2];  // head and tail sentinel nodes
  struct proclistnode *head;
  struct proclistnode *tail;
  struct spinlock lock;
};

struct sortedproclist {
  int size;
  struct proclistnode buf[2];  // head and tail sentinel nodes
  struct proclistnode *head;
  struct proclistnode *tail;
  int (*cmp)(struct proc *, struct proc *);
  struct spinlock lock;
};
```
#### (proc.h) 在 proc struct 當中加入額外屬性以實踐 multilevel feedback queue scheduler9
```
int queue_level;  // 該 process 的 qeueu_level
int Ti;           // 當前 CPU time 執行的 ticks
int T;            // 預估當前 CPU time 會執行的 ticks
```
#### (proc.c) 定義 L1、L2、L3 ready queue 
```
struct sortedproclist l1_ready;
struct sortedproclist l2_ready;
struct proclist l3_ready;
```
#### (defs.c) 新增 findsortedproclist、pushsortedproclist、removesortedproclist，提供 L1/L2 sorted queue 操作介面
```
struct proclistnode *findsortedproclist(struct sortedproclist *spl,
                                        struct proc *p);
void pushsortedproclist(struct sortedproclist *spl, struct proclistnode *pn);
void removesortedproclist(struct sortedproclist *spl, struct proclistnode *pn);
```
#### (proc.c) 實作 findsortedproclist、pushsortedproclist、removesortedproclist
```
struct proclistnode *findsortedproclist(struct sortedproclist *spl,
                                        struct proc *p) {
  struct proclistnode *tmp, *pn;
  acquire(&spl->lock);
  pn = 0;
  for (tmp = spl->head->next; tmp != spl->tail && pn == 0; tmp = tmp->next) {
    if (tmp->p == p) {
      pn = tmp;
    }
  }
  release(&spl->lock);
  return pn;
}
```
```
void pushsortedproclist(struct sortedproclist *pl, struct proclistnode *pn) {
  struct proclistnode *pn1;
  acquire(&pl->lock);
  pl->size++;
  for (pn1 = pl->head->next; pn1 != pl->tail; pn1 = pn1->next) {
    if (pl->cmp(pn->p, pn1->p) > 0) {
      break;
    }
  }
  pn->next = pn1;
  pn->prev = pn1->prev;
  pn1->prev->next = pn;
  pn1->prev = pn;
  release(&pl->lock);
}
```
```
void removesortedproclist(struct sortedproclist *spl, struct proclistnode *pn) {
  acquire(&spl->lock);
  spl->size--;
  pn->prev->next = pn->next;
  pn->next->prev = pn->prev;
  release(&spl->lock);
}
```
#### (proc.c) 實作 pushreadylist，會依照 process 的 priority 放到適合的 ready list 當中
```
void pushreadylist(struct proc *p) {
  struct proclistnode *pn;
  if ((pn = allocproclistnode(p)) == 0) {
    panic("pushreadylist: allocproclistnode");
  }
  p->queue_level = prio_to_queue(p->priority);
  p->wait_ticks = 0;

  switch (p->queue_level) {
  case 1:
    pushsortedproclist(&l1_ready, pn);
    break;
  case 2:
    pushsortedproclist(&l2_ready, pn);
    break;
  case 3:
    pushbackproclist(&l3_ready, pn);
    break;
  default:
    panic("pushreadylist: invalid queue level");
  }
}
```
#### (proc.c) 定義 L1 sorted queue 的排序邏輯
```
static inline int l1_key(struct proc *p) { return p->Ti - p->T; }
int l1_cmp(struct proc *a, struct proc *b) {
  int ka = l1_key(a), kb = l1_key(b);
  if (ka != kb)
    return (kb - ka);
  return (b->pid - a->pid);
}

static inline int l1_cmp_top(struct proc *p) {
  return cmptopsortedproclist(&l1_ready, p);
}
```
#### (proc.c) 定義 L2 sorted queue 的排序邏輯
```
int l2_cmp(struct proc *a, struct proc *b) {
  if (a->priority != b->priority)
    return (a->priority - b->priority);
  return (b->pid - a->pid);
}
```
#### (proc.c) 定義 prio_to_qmeue 決定不同 priority 的 process 要放在哪一個 queue
```
static inline int prio_to_queue(int prio) {
  if (prio >= 100)
    return 1;
  if (prio >= 50)
    return 2;
  return 3;
}
```
### ----- process 初始化過程 -----
#### (proc.c) 在 allocproc() 中對 process 結構中的 新增 queue_level、wait_ticks、Ti 與 T 進行初始化。
```
p->queue_level = 3;
p->wait_ticks = 0;
p->Ti = 0;
p->T = 0;
```
#### (proc.c) 在 userinit() 中呼叫 allocproc() 建立 new process，並在初始化完成後將其狀態設為 RUNNABLE，再放入 run queue。
```
void userinit(void) {
  struct proc *p;
  
  # 呼叫 allocproc
  p = allocproc();
  initproc = p;

  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  p->trapframe->epc = 0;
  p->trapframe->sp = PGSIZE;

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
  
  # 將新 process 設定成 RUNNALBE 並丟到 ready queue
  p->state = RUNNABLE;
  procstatelog(p);
  pushreadylist(p);

  release(&p->lock);
}
```
### ----- timer interrupt 過程 -----
#### (trap.c) 當 CPU 經過一段時間，就會發生 timer interrupt，並進到 kerneltrap()，而在 kerneltrap 當中會透過自訂義的 implicityield() 決定是否要發生 preemptive。
###### kerneltrap 實作
```
void kerneltrap() {
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0) {
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    implicityield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}
```
###### implicityield 實作
```
void implicityield(void) {
  struct proc *p = myproc();
  if (!p)
    return;

  // Update T for L1 processes (accumulate running time)
  if (p->queue_level == 1 && p->state == RUNNING) {
    p->T += 1;
  }

  // Handle cross-queue preemption
  // If there is an L1 task and the current process is not L1, yield the CPU
  if (sizesortedproclist(&l1_ready) > 0 && p->queue_level != 1) {
    yield();
    return;
  }
  // If there is no L1 task and there is an L2 task, and the current process
  // is L3, yield the CPU
  if (sizesortedproclist(&l1_ready) == 0 && sizesortedproclist(&l2_ready) > 0 &&
      p->queue_level == 3) {
    yield();
    return;
  }
  // L1 preemptive SJF: yield if a better candidate (at the head) exists than
  // the current process
  if (p->queue_level == 1) {
    if (l1_cmp_top(p) < 0) {
      yield();
      return;
    }
  }
  // L2 Queue: Non-preemptive Priority Scheduling
  // L2 processes do not preempt each other
  // Only L1 processes can preempt L2 processes
  // L3 Round-Robin: time quantum = 10 ticks
  if (p->queue_level == 3) {
    if (ticks - p->startrunningticks >= 10) {
      yield();
      return;
    }
  }
}
```
#### (prop.c) 若發生 preemptive，原本的 process 會進入 sleep 狀態。在將該 process 轉入 sleep 前，系統需先更新其Ti。
```
  int actual_runtime = ticks - p->startrunningticks;
  p->T += actual_runtime; // Accumulate total runtime for this burst

  // Update estimated burst time: Ti = floor((T + Ti_prev) / 2)
  if (p->Ti == 0) {
    p->Ti = p->T; // First burst: estimate = actual
  } else {
    p->Ti = (p->T + p->Ti) / 2; // Update estimate
  }

  p->T = 0; // Reset T for next burst
```
### ----- 每個 Tick 週期中的 Aging 機制 -----
#### aging function
```
void aging(void) {
  int newlevel, oldlevel;
  // Call once every tick
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state != RUNNABLE)
      continue;

    p->wait_ticks++;
    if (p->wait_ticks < 20)
      continue;

    p->wait_ticks = 0;
    if (p->priority < 149) {
      // enhance priority
      p->priority++;
    }

    oldlevel = p->queue_level;
    newlevel = prio_to_queue(p->priority);
    if (newlevel == oldlevel)
      continue;
    else {
      p->queue_level = newlevel;
      struct proclistnode *pn = 0;
      switch (oldlevel) {
      case 1:
        pn = findsortedproclist(&l1_ready, p);
        if (!pn)
          break;
        removesortedproclist(&l1_ready, pn);
        freeproclistnode(pn);
        break;
      case 2:
        pn = findsortedproclist(&l2_ready, p);
        if (!pn)
          break;
        removesortedproclist(&l2_ready, pn);
        freeproclistnode(pn);
        break;
      case 3:
        pn = findproclist(&l3_ready, p);
        if (!pn)
          break;
        removeproclist(&l3_ready, pn);
        freeproclistnode(pn);
        break;
      }
      pushreadylist(p);
    }
  }
}
```
```
void clockintr() {
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
  
  // 每經過一個 ticks 就呼叫一次 aging function
  aging();
}
```

## Experiment
### 執行 ./grade-mp2-public
![image](https://hackmd.io/_uploads/BysWs3dAeg.png)
### 執行 ./grade-mp2-bonus
#### 為了測試aging，先讓系統變得很忙：首先 fork 出幾個spinner行程，它們只是不停地跑迴圈、不讓出 CPU，這樣 CPU 幾乎都被它們佔著。接著再 fork 出一個被測的行程（aged process），它一開始會先睡一小段時間，等它醒來的時候，理論上會被放在比較低的優先層。如果 aging 機制有實作好，那它應該會等一陣子之後被升上來、拿到 CPU 執行。反之，如果 aging 沒有生效，它就會永遠等不到機會。最後用一個簡單的 timeout 機制判斷成敗：如果在限定時間內這個行程成功執行並印出 AGING_PASS，就代表 aging 成功；反之就輸出 AGING_FAIL。

```
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N_SPINNERS   3      // 製造系統忙碌的 CPU-bound 程式數量
#define BUSY_TICKS   800    // 每個 spinner 忙碌多久
#define SLEEP_TICKS  10     // 被測行程先睡一下，醒來後會被放在低層級
#define TIMEOUT_TICKS 600   // 若 aging 沒在這時間內讓它跑到，視為失敗

static void spinner(void) {
  uint start = uptime();
  while (uptime() - start < BUSY_TICKS) {
    // busy waiting
  }
  exit(0);
}

int
main(void)
{
  int p[2];
  if (pipe(p) < 0) {
    printf("bonus_aging: pipe failed\n");
    exit(1);
  }

  // 建立多個 CPU-bound 程式，讓系統長期忙碌，避免被測程式輕易搶到 CPU
  for (int i = 0; i < N_SPINNERS; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("bonus_aging: fork spinner failed\n");
      exit(1);
    }
    if (pid == 0) {
      // child: spinner
      close(p[0]); close(p[1]);
      spinner();
    }
  }

  // 被測程式：先睡一小段，醒來後通常會被放在較低優先層
  int pid_aged = fork();
  if (pid_aged < 0) {
    printf("bonus_aging: fork aged failed\n");
    exit(1);
  }
  if (pid_aged == 0) {
    // child: aged process
    close(p[0]); // 只寫
    sleep(SLEEP_TICKS);
    // 能執行到這裡，代表已經醒來並被 scheduler 安排到 CPU。
    // 若 Aging 正常，即使被壓在低層，最終也會被提升而跑到。
    write(p[1], "A", 1); // 告訴父 process : 我成功跑到了
    close(p[1]);
    exit(0);
  }

  // 超時計時器：若 aging 沒在期限內讓 aged process 跑到，就先寫入 'T'
  int pid_timer = fork();
  if (pid_timer < 0) {
    printf("bonus_aging: fork timer failed\n");
    exit(1);
  }
  if (pid_timer == 0) {
    close(p[0]); // 只寫
    sleep(TIMEOUT_TICKS);
    write(p[1], "T", 1); // timeout
    close(p[1]);
    exit(0);
  }

  // parent: 等待第一個結果（A=pass，T=timeout）
  close(p[1]); // 只讀
  char c;
  int n = read(p[0], &c, 1);
  if (n == 1 && c == 'A') {
    printf("AGING_PASS\n");
  } else {
    printf("AGING_FAIL\n");
  }
  close(p[0]);

  // 清理子 process（避免殭屍）
  // 殺掉可能還在跑的 spinner/timer
  kill(pid_aged);
  kill(pid_timer);
  for (int i = 0; i < N_SPINNERS + 2; i++)
    wait(0);

  exit(0);
}
```
#### 測試程式
```
@test(5, "bonus: aging (long-waited process eventually runs)")
def test_aging():
    r.run_qemu(shell_script([
        'bonus_aging',
    ]))
    r.match('AGING_PASS')
```
#### Result
![image](https://hackmd.io/_uploads/BJiv1bYAxx.png)
