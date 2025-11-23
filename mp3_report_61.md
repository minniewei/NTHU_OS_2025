
# OS25-mp3
## SetUp 
```
# 將檔案從 branch os-mp1 clone 下來
git clone -b os25-mp3 https://git.lsalab.cs.nthu.edu.tw/os25/os25_team61_xv6

# 進到 os25_team61_xv6 資料夾
cd .\os25_team61_xv6\

# 開啟 docker
docker run -it --rm -v ${PWD}:/xv6 -v /xv6/mkfs -v ${PWD}/mkfs/mkfs.c:/xv6/mkfs/mkfs.c -w /xv6 --platform linux/amd64 dasbd72/xv6:amd64

# 執行測試檔 (./grade-mp1-public)
 ./grade-mp3-public
```
## Trace Code
### How does xv6 run a user program?
#### 1. kernel/main.c/main()
#### 在 xv6 裡，核心啟動後執行 main()，做完初始化硬體、設定中斷、建立 CPU 結構後，會呼叫 userinit() 來建立系統中的第一個 user process。這個 process 的內容只有極小一段 initcode（位在 user/initcode.S），它負責呼叫 exec("/init") 把自己變成真正的 user 程式 /init。接著 /init 再進一步啟動 shell 或其他 user program。當 scheduler 開始運作後，只要有 RUNNABLE 的 process，它就會被 CPU 選中執行，trapframe 的 epc 會被載入，CPU 就會跳回 user 模式，開始跑 user code。整個流程中，核心透過 trap、系統呼叫（syscall）、page table 切換等機制，不斷在 user mode 與 kernel mode 之間切換，最終讓一個 user 程式能從磁碟載入、映射、並在 CPU 上真正執行。
#### 2. kernel/proc.c/scheduler()
#### 每顆 CPU 進入 scheduler() 的無窮迴圈後，會一直開啟中斷、掃過整個 proc[] process table，對每一個 proc 先拿 lock，檢查它的 state 是否是 RUNNABLE，如果是，就把它改成 RUNNING，把 c->proc 指向這個 process，然後呼叫 swtch(&c->context, &p->context) 進行 context switch：CPU 會從目前的 scheduler context 切換到該 process 的 kernel context，接著再透過 trap 返回 user mode，從這個 process 的 trapframe 裡的 epc 開始執行對應的 user 指令。當這個 process 因為系統呼叫、timer interrupt、yield()、sleep() 或 exit() 回到 kernel 並結束這一輪執行後，控制權會再切回 scheduler，swtch 回來之後繼續從下一個 for 迴圈往下掃，尋找下一個 RUNNABLE 的行程，一直反覆進行，所以多個 user program 就能在單顆或多顆 CPU 上輪流執行。
#### 3. kernel/switch.S
#### kernel/switch.S 是用來實作核心內部 context switch 的地方，它定義了 swtch 這個函式，負責在排程器的 context跟某個 process 的 kernel context之間切換。當 scheduler() 決定要讓某個 RUNNABLE 的 process 開始跑時，會呼叫 swtch(&c->context, &p->context)：switch.S 會先把目前 CPU 上正在執行的那一段 kernel code（可能是 scheduler 本人）的暫存器狀態存到舊的 context 裡，再從新的 context 裡把暫存器內容載回來，於是 CPU 從此刻開始就變成在執行另一段 kernel 流程。如果那個流程最後透過 usertrapret()/sret 回到 user-mode，就會接著跑對應的 user program。
#### 4. kernel/proc.c/forkret()
#### 當我們在 fork() 裡建立一個新的行程時，核心會在 allocproc() 裡先把這個 process 的 kernel context 設好，其中最關鍵的一行是：
```
// 在 allocproc() 裡
memset(&p->context, 0, sizeof(p->context));
p->context.ra = (uint64)forkret;      // 第一次被 swtch() 回來時，會從 forkret 開始跑
p->context.sp = p->kstack + PGSIZE;
```
#### 這代表：這個 process 第一次被 scheduler 挑中、呼叫 swtch(&c->context, &p->context) 時，ret 之後不是回到某個原本在跑的函式，而是直接跳到 forkret()。
```
void
forkret(void)
{
  static int first = 1;
  struct proc *p = myproc();

  // 這時還握著 p->lock（是 scheduler 在 context switch 前拿的）
  release(&p->lock);
  
  // 只在「第一次有 process 真正開始跑」時執行。
  if(first){
    first = 0;
    // 例如：fsinit(ROOTDEV) 或 loginit() 之類的東西
  }

  // 最後一步：回到 user-mode，從 trapframe->epc 開始跑 user 程式
  usertrapret();
}
```
#### 當一個剛 fork 出來的新 process 第一次被排到 CPU 上時，scheduler 還幫它拿著 p->lock，避免 race condition。forkret() 做的第一件事就是把這個 lock 放掉，讓 process 未來可以正常被其他 CPU 或系統呼叫存取。接著，它會執行一些只需要跑一次的系統初始化（在某些 xv6 版本是檔案系統 log 的初始化），確保這些操作是在有 process context 的情況下完成，而不是在還沒有 process 的早期 boot 階段。最後， usertrapret()這個函式會根據目前 process 的 trapframe，設定好 satp（page table）、sstatus（切回 user mode）、sepc（user PC = trapframe->epc），然後執行 sret，讓 CPU 正式跳回 user space，從對應的 user 指令開始執行。

#### 5. kernel/trap.c/usertrapret()
#### usertrapret() 是每次 kernel 處理完系統呼叫或中斷之後，準備要回去 user-mode 跑 user program 的最後一步。當一個 user process 因為 syscall、timer interrupt 或 page fault 進到 usertrap() 裡，核心把事情處理完（例如執行 sys_read、sys_exec、排程等等），最後就會呼叫 usertrapret()。這個函式做的事就是幫這個 process 把下次再進來 kernel 時要用到的資料寫進 trapframe，把硬體的中斷入口指回 usertrap（透過 trampoline），再準備好要切換到 user pagetable 的 satp 值，最後跳進 trampoline.S 裡的 userret，由組語去設定 CPU 的狀態（sstatus、sepc、satp），真的執行 sret 回到 user-mode，從 p->trapframe->epc 指向的那條 user 指令繼續往下跑。
```
void
usertrapret(void)
{
  struct proc *p = myproc();

  intr_off();  // 要切回 user-mode 了，先關掉中斷

  // 之後若再從 user-mode trap 進來，要跳到 trampoline 裡的 usertrap() wrapper
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // 把 kernel 之後要用的資訊寫進 trapframe，留給 trampoline 用
  p->trapframe->kernel_satp  = r_satp();            // 現在的 kernel page table
  p->trapframe->kernel_sp    = p->kstack + PGSIZE;  // 這個 process 的 kernel stack top
  p->trapframe->kernel_trap  = (uint64)usertrap;    // 之後從 user trap 回來要呼叫的 C 函式
  p->trapframe->kernel_hartid = r_tp();             // CPU id，用來找對應的 cpu 結構

  // 準備好切到 user pagetable 的 satp
  uint64 satp = MAKE_SATP(p->pagetable);

  // 跳到 trampoline.S 的 userret(trapframe_va, satp)
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);
}
```
#### usertrapret() 先關中斷，避免在切換回 user 的途中又被打斷；接著用 w_stvec() 把 trap vector 指到 user-mode 專用的 uservec（在 trampoline.S 裡），這樣下次 user 再發生 trap，就會先跳到 trampoline，再轉呼叫 usertrap()。然後它把 kernel 之後要用到的資訊，例如回到 kernel 要用哪個 page table (kernel_satp)、要用哪個 kernel stack (kernel_sp)、trap 時要跳回哪個 C 函式 (kernel_trap)、是哪一顆 CPU (kernel_hartid)，全部塞進 p->trapframe 裡。最後，它算出這個 process 的 user pagetable 對應的 satp，呼叫 trampoline 上的 userret(TRAPFRAME, satp)。在 trampoline.S 的 userret 裡，組語會做幾件事：從 trapframe 讀出 epc 填到 sepc，調整 sstatus 把 SPP 設成 user、開啟中斷位、切換 satp 到 user pagetable，然後執行 sret。sret 一執行完，CPU 就正式回到 user-mode，而且會從原本 p->trapframe->epc 指向的 user 指令繼續跑。
#### 6. kernel/trampoline.S/userret: (Note: In userinit function, we put initcode into the user page table)
#### kernel/trampoline.S 裡的 userret 就是真正把 CPU 從 kernel 模式推回去 user 模式、開始執行 initcode（或之後的 /init 等 user 程式）的最後一步。前面在 userinit() 時，核心已經把 initcode[] 塞進 user page table 的 VA=0，p->trapframe->epc 也設成 0；後面在 usertrapret() 裡，核心算好這個 process 的 user pagetable 對應的 satp，並呼叫 trampoline 上的 userret(TRAPFRAME, satp)。userret 是一小段位在固定高位址 TRAMPOLINE 的組語，它會先把 satp 暫存起來，從 TRAPFRAME 把 user 的暫存器內容載回來，把 trapframe->epc 寫進 sepc，再調整 sstatus 把 SPP 設成 user、開啟 user-mode 中斷，最後再把 satp 寫成 user page table，真正完成 page table 切換，然後執行 sret。
```
userret:
  # a0 = TRAPFRAME, a1 = satp(for user)
  csrw satp, a1           # 切到 user pagetable
  sfence.vma              # 清 TLB

  ld t0, 112(a0)          # t0 = epc in trapframe
  csrw sepc, t0           # sepc = user PC

  ld t0, 0(a0)            # 載回 ra, sp, s0~s11 等暫存器
  ...
  ld sp, 8(a0)
  ...

  csrr t0, sstatus
  li   t1, ~SSTATUS_SPP
  and  t0, t0, t1         # SPP=User
  ori  t0, t0, SSTATUS_SPIE
  csrw sstatus, t0

  sret                    # 跳回 user-mode，從 sepc（也就是 initcode）開始跑
```
#### 這樣一來，CPU 就從跑在 trampoline 這一小段 kernel 組語變成跑在 user page table + user-mode 的 VA=0，也就是 initcode 的第一條指令。initcode 會再呼叫 exec("/init")，載入真正的 ELF /init；之後每次 user 再因為 syscall 或中斷進 kernel、處理完又要回去跑 user code時，流程一樣會經過 usertrapret() → trampoline.S:userret → sret，不斷在 user program 和 kernel 之間切換，整個 xv6 的 user-space 就會被跑起來並維持運作。
#### 7. kernel/exec.c/exec()
#### kernel/exec.c 的 exec() 就是真正把一個程式從檔案系統載入到記憶體，取代目前這個 process 的 user 空間，讓它變成在跑新程式。當 initcode 或 shell 呼叫 exec("/init", argv)、exec("sh", argv) 時，核心就會進到 exec()：打開對應的 ELF 檔，建立一個新的 user pagetable，依照 ELF 的程式區段把程式碼與資料段載入對應的虛擬位址，接著在 user 空間頂端做出一個 user stack，把 argv 字串和指標排好，最後把 process 的 pagetable、sz、以及 trapframe->epc（user PC）與 trapframe->sp（user stack）全部換成新的設定。從這一刻開始，雖然 pid 沒變、還是同一個 process 結構，但它的 user 記憶體內容與執行起點都被完全替換成那個你想跑的 user program。之後當排程器選到這個 process，經過 usertrapret() → trampoline.S:userret → sret 回到 user-mode 時，CPU 就會從 trapframe->epc = elf.entry 指向的 entry point 開始跑這個 ELF 程式。
```
int exec(char *path, char **argv)
{
  struct elfhdr elf;
  struct proghdr ph;
  struct inode *ip;
  pagetable_t pagetable = 0;
  struct proc *p = myproc();

  begin_op();
  ip = namei(path);      // 找到檔名對應的 inode
  ilock(ip);
  readi(ip, 0, (uint64)&elf, 0, sizeof(elf));  // 讀 ELF header
  if(elf.magic != ELF_MAGIC) goto bad;

  pagetable = proc_pagetable(p);  // 建新的 user pagetable
  sz = 0;
  for(... 每個 program header ...){
    if(ph.type != ELF_PROG_LOAD) continue;
    sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, PTE_W);
    sz = sz1;
    loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz); // 把程式載入到對應 VA
  }
  ...
}

```
#### 這裡 uvmalloc() 會在新的 pagetable 裡幫你把 [0, sz) 這個範圍的虛擬位址對應到實體頁，loadseg() 則把 ELF 檔裡對應 offset 的內容讀進來。也就是說，這一步決定了這個程式在 user 虛擬位址空間的長相。
#### 接著是做 user stack + 設定 entry point 和 argv：
```
sz = PGROUNDUP(sz);
// 另外分兩頁當 stack，其中一頁當 guard page
sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE, PTE_W);
sz = sz1;
uvmclear(pagetable, sz-2*PGSIZE);   // guard page 不給 user 存取
sp = sz;
stackbase = sp - PGSIZE;

// 把 argv[i] 字串一個個搬到 user stack 上，記錄每個字串的位址到 ustack[]
for(argc = 0; argv[argc]; argc++){
  sp -= strlen(argv[argc]) + 1;
  sp &= ~0xf;  // 16-byte 對齊
  copyout(pagetable, sp, argv[argc], strlen(argv[argc])+1);
  ustack[argc] = sp;
}
ustack[argc] = 0;  // argv[argc] = 0

// 再把假的 return address, argc, argv 指標陣列塞到 stack 上
sp -= (argc+1+1) * sizeof(uint64);
sp &= ~0xf;
copyout(pagetable, sp, (char*)ustack, (argc+1+1)*sizeof(uint64);

// 最後設定 trapframe：PC 與 SP
p->trapframe->epc = elf.entry;  // ELF entry point
p->trapframe->sp  = sp;
p->trapframe->a1  = sp;         // 傳給 user main 的 argv 位址
```
#### 這一段做完，等於幫新程式準備好 main(argc, argv) 要看的 stack 內容，並把 CPU 將來要從哪一行 user 指令開始跑（elf.entry）記錄在 trapframe->epc 裡。
#### 最後一步就是正式切換到新程式：
```
oldpagetable = p->pagetable;
oldsz = p->sz;
p->pagetable = pagetable;   // 換成新的 address space
p->sz = sz;
proc_freepagetable(oldpagetable, oldsz);  // 把舊程式的 user memory 釋放掉

// 更新 p->name，用於 ps/debug
for(last = s = path; *s; s++)
  if(*s == '/')
    last = s+1;
safestrcpy(p->name, last, sizeof(p->name));
```
#### 從這裡開始，這個 process 的 user 空間就完全變成剛剛載入的那個 ELF 程式，之後不管是第一次由 forkret() → usertrapret() 跳回 user，還是從 syscall / interrupt 處理完再回去，CPU 都會根據這個新的 pagetable 和 trapframe->epc / sp，從這個新程式的 entry point 開始執行。
### How does xv6 allocate physical memory and map it into the process’s virtual address space?
#### 1. user/user.h/sbrk()
#### 在 xv6 裡，sbrk() 是 user program 要長出更多 heap 時用的介面，但它本身其實只是一個系統呼叫包裝：在 user 端呼叫 sbrk(n)，會透過 ecall 進 kernel 的 sys_sbrk()，kernel 再呼叫 growproc(n)，growproc() 會用 uvmalloc() 從實體記憶體配置一頁一頁的 page（用 kalloc() 拿到 physical page），然後用 mappages() 把這些 physical page 映射到該 process 的虛擬位址空間尾端（從原本的 p->sz 往上長），最後更新 p->sz。
#### user 端在 user/user.h 只看到宣告：
```
int sbrk(int);
```
#### 真正的 syscall stub 在 user/usys.S 會把 SYS_sbrk 放到 a7，n 放到 a0，然後 ecall。進 kernel 之後，kernel/sysproc.c 裡的 sys_sbrk() 長這樣：
```
uint64
sys_sbrk(void)
{
  int addr;
  int n;
  struct proc *p = myproc();

  if(argint(0, &n) < 0)
    return -1;
  addr = p->sz;          // 回傳原本 heap 的結尾給 user
  if(growproc(n) < 0)    // 把位址空間從 sz 長到 sz+n
    return -1;
  return addr;
}
```
#### growproc() 在 kernel/proc.c 的邏輯是如果 n>0，就呼叫 uvmalloc() 往上長，n<0 就 uvmdealloc() 往下縮：
```
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
      return -1;
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}
```
#### 而真正配置實體記憶體並寫 page table的是 kernel/vm.c 的 uvmalloc()：
```
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;
  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();                  // 從 free list 拿一頁 physical memory
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);          // 把這一頁清成 0
    if(mappages(pagetable, a, PGSIZE,
                (uint64)mem, PTE_R | PTE_W | PTE_U | xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a + PGSIZE, oldsz);
      return 0;
    }
  }
  return newsz;
}
```
#### 這裡 kalloc() 從 kernel 維護的 free list 裡拿一個未使用的 physical page，mappages() 則在 process 的 page table 裡建立 PTE：把虛擬位址 a 對應到剛拿到的 mem（physical address），並設好 PTE_V | PTE_U | PTE_R | PTE_W 等旗標。完成後，對 user 來說，只是 sbrk(n) 回傳了一個原本 break 的位置，但實際上底下已經幫你把 heap 後面的那一段虛擬位址區間，對應到了新的實體頁，之後你在那一段 VA 上讀寫，就會打到剛配置好的 physical memory。
#### 2. user/usys.pl
#### user/usys.pl 是幫所有 user-level 系統呼叫（包含 sbrk）自動產生組語 stub，讓 user 程式可以用 C 函式的形式呼叫內核服務。就是實際在 kernel 裡配實體頁、更新 page table 的，是 kalloc() / uvmalloc() / mappages()；而 usys.pl 做的事情，是在 user 端幫你接上 syscalls 那一頭，讓 sbrk() 這個呼叫真的能觸發 kernel 這一整串動作。
#### 在 user/usys.pl 裡會看到一串這樣的宣告：
```
entry("fork");
entry("exit");
entry("wait");
entry("pipe");
...
entry("sbrk");
entry("sleep");
entry("uptime");
```
#### 這個 Perl 腳本跑的時候，會為每個 entry("名字") 產生對應的 user-space 組語函式，輸出到 user/usys.S。以 sbrk 為例，它會變成這樣的 RISC-V stub：
```
.global sbrk
sbrk:
  li a7, SYS_sbrk   # 把系統呼叫號碼放到 a7
  ecall             # 觸發 trap 進 kernel
  ret               # 回傳值留在 a0，直接回到 user C 程式
```
#### 然後在 user/user.h 裡有：
```
int sbrk(int);
```
#### 所以當 user 程式寫：
```
char *p = sbrk(4096);
```
#### 編譯後實際執行的是上面那段組語：把 SYS_sbrk 放進 a7，把參數 4096 放在 a0，執行 ecall，CPU 就從 user-mode trap 進 kernel 的 uservec → usertrap() → sys_sbrk()；sys_sbrk() 再呼叫 growproc()，growproc() 用 uvmalloc() 去用 kalloc() 拿新的 physical page，然後 memset(mem, 0, PGSIZE) 清空，用 mappages() 在這個 process 的 page table 裡建立「VA 尾端 → 這個 PA」的 PTE，最後再把舊的 p->sz 當作回傳值透過 a0 帶回 user，ret 之後你在 C 裡看到的就是原本 heap 的結尾位址。
#### 3. kernel/sysproc.c/sys_sbrk()
#### 使用者程式呼叫 sbrk(n) 之後會透過系統呼叫跳進核心，而核心收到這個請求時，就會進入 sys_sbrk()。在這個函式裡，核心先把 user 傳進來的參數 n 取出來，再讀取目前行程的 p->sz，也就是這個 process 的 heap 末端（break）。這個舊的 break 在系統呼叫完成後會回傳給使用者，因此 sys_sbrk() 會先把它保留下來。接著，sys_sbrk() 呼叫 growproc(n)，growproc() 在核心內部會進一步呼叫 uvmalloc()，而 uvmalloc() 會使用 kalloc() 從自由實體記憶體清單取出新的物理頁，清成全零，並透過 mappages() 把這些頁面映射到行程的虛擬位址空間的尾端。也就是說，分配物理記憶體和加入 page table都是在 uvmalloc() 裡完成的，而不是 sys_sbrk() 親自處理。當 growproc() 成功把位址空間擴增後，行程的 p->sz 會更新成新的大小，而 sys_sbrk() 會把剛開始記下的舊 break 當作回傳值回到 user-side，如同標準 UNIX 系列系統的 sbrk() 行為。
#### 4. kernel/proc.c/growproc()
#### kernel/proc.c 裡的 growproc() 就是系統呼叫 sbrk() 進核心後，負責真的去長／縮這個 process 位址空間的那一層包裝。sys_sbrk() 把 user 傳進來的 n 交給 growproc(n)，而 growproc() 會依照 n 的正負號決定要往上長還是往下縮：如果 n > 0，它會呼叫 uvmalloc(p->pagetable, sz, sz + n, PTE_W)，從目前的 p->sz 開始，一頁一頁地向高位址擴張；uvmalloc() 內部會用 kalloc() 從 free list 拿一頁實體記憶體、用 memset 清成 0，再呼叫 mappages() 在這個行程的 page table 裡新增 PTE，把那一段新長出來的虛擬位址（原本 sz 之後那一段）對應到剛拿到的 physical page，並設定好 PTE_R | PTE_W | PTE_U 等權限。成功後，uvmalloc() 回傳新的結尾 newsz，growproc() 就把 p->sz 更新成這個 newsz，讓這個 process 的合法 user 虛擬位址範圍真的變大。
```
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
      return -1;
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}
```
#### 如果 n < 0，growproc() 則改呼叫 uvmdealloc()，它會計算要釋放多少頁，呼叫 uvmunmap() 把這些虛擬頁對應的 PTE 清掉，並在 do_free=1 的情況下用 kfree() 把對應的 physical page 歸還給核心，然後再把 p->sz 往回縮。這樣一來，growproc() 本身並不直接操作實體記憶體，而是站在以 process 為單位的角度，把我要長／縮 n bytes轉換成呼叫 uvmalloc 或 uvmdealloc 在這個 process 的 pagetable 上配置／釋放頁面，讓底層的 kalloc()、kfree() 和 mappages() 真的去完成配實體記憶體並映射到這個行程虛擬位址空間這件事。
#### 5. kernel/vm.c/uvmalloc()
#### 當某個行程希望擴張自己的虛擬記憶體空間（例如來自 sbrk()），核心的 growproc() 就會呼叫 uvmalloc()，請它把原本的記憶體終點 oldsz 延伸到 newsz。uvmalloc() 會從 PGROUNDUP(oldsz) 開始，一頁一頁往上走，每遇到一個新需要佔據的頁面，就呼叫 kalloc() 取得一頁真正的物理記憶體，並用 memset 清成 0，確保 user process 不會意外讀到其他行程留下的資料。接著，它會呼叫 mappages() 在這個 process 的 pagetable 裡建立一條 VA→PA 的映射，把剛剛拿到的物理頁放到對應的虛擬位址底下，並附上 user-mode 能使用的必要權限，包括 PTE_R、PTE_W、PTE_U。只要每一頁順利配置並映射，uvmalloc() 就會回傳新的大小 newsz；如果在某一頁失敗（例如 kalloc() 拿不到實體記憶體），它就會回收已配置的那部分再返回失敗。這意味著 uvmalloc() 不只是配置物理記憶體，而是完整負責把一段虛擬位址變成在 page table 裡有映射、有權限、且真的能被 user code 使用的記憶體。
```
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(uint64 a = oldsz; a < newsz; a += PGSIZE){
    char *mem = kalloc();          // 配一頁實體 memory
    if(mem == 0)
      return 0;
    memset(mem, 0, PGSIZE);        // 清除舊內容（安全考量）
    if(mappages(pagetable, a, PGSIZE, (uint64)mem,
                PTE_R | PTE_W | PTE_U | xperm) != 0){
      kfree(mem);                  // 映射失敗要回收
      return 0;
    }
  }
  return newsz;
}
```
#### 在這段程式碼中，可以看到 xv6 的虛擬記憶體配置其實是這三件事的組合：1.向 kalloc() 索取實體頁面 → 這是 xv6 的 physical memory allocator。2.把頁面清成 0 → 避免資訊洩漏，保證 deterministic 行為。3.用 mappages() 更新 pagetable → 讓 VA 能合法存取到剛拿到的 PA。這是 User-level memory allocation（例如 malloc()）背後最底層的機制。

## Implementation
#### Print Page Table
###### (vm.c) 透過遞迴函式 vmprint_walk()，從最上層 pagetable 開始往下走，依序顯示：PTE、VA、PA、Flag
```
void vmprint(pagetable_t pagetable)
{
  printf("page table 0x%p\n", pagetable);
  vmprint_walk(pagetable, 2, 0);
}
```
```
static void vmprint_walk(pagetable_t pt, int level, uint64 va_prefix)
{
  if (level < 0)
    return;
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pt[i];
    if ((pte & PTE_V) == 0)
      continue;
    
    uint64 va_nofs = va_with_index(va_prefix, level, i);
    uint64 pa_nofs = PTE2PA(pte);
    for (int s = 0; s < 2 * (3 - level); s++)
      printf(" ");
    printf("%d: pte=0x%p va=0x%p pa=0x%p", i, pte, va_nofs, pa_nofs);printf("%d: pte=%p va=%p pa=%p", i, (void *)pte, (void *)va_nofs, (void *)pa_nofs);%
    vmprint_flags(pte);
    printf("\n");
    if ((pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      vmprint_walk((pagetable_t)pa_nofs, level - 1, va_nofs);
    }
  }
}
```
```
# 組合 va_prefix 跟 index i ，找到對應的 VA
static inline uint64 va_with_index(uint64 va_prefix, int level, int i)
{
  int shift = PGSHIFT + 9 * level;
  uint64 keep_high = ~((1ULL << (shift + 9)) - 1);
  return (va_prefix & keep_high) | ((uint64)i << shift);
}
```
```
/* 顯示每個 PTE 的 Flag
   V: valid or invalid
   R: Readable
   W: Writable
   X: Executable
   U: User-accessible
*/
static void vmprint_flags(pte_t pte)
{
  printf(" V");
  if (pte & PTE_R)
    printf(" R");
  if (pte & PTE_W)
    printf(" W");
  if (pte & PTE_X)
    printf(" X");
  if (pte & PTE_U)
    printf(" U");
}
```
#### Add a read only share page
###### (proc.h) 在 proc struct 當中新增 struct usyscall *usyscall ，用來指向 usyscall page。
```
struct usyscall *usyscall;
```
###### (proc.c) 在 allocproc function 當中初始化 usyscall
```
  p->usyscall = (struct usyscall *)kalloc();
  if (p->usyscall == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  memset((void *)p->usyscall, 0, PGSIZE);
  p->usyscall->pid = p->pid;
```
###### (proc.c) 在 freeproc function 當中消除 usyscall 相關設定
```
  if (p->usyscall)
  {
    kfree((void *)p->usyscall);
    p->usyscall = 0;
  }
```
#### Generate a Page Fault
###### (sysproc.c) 更改 sbrk system 使其滿足 lazy allocation 策略
```
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
    if (newsz > oldsz) // overflow check
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
```
###### (trap.c) 當發生 usertrap 時，透過 r_scause() == 13 || 15 來檢查該為 VA 是否尚分配 PA，假設是，呼叫 handle_pgfault
```
  else if ((scause == 13 || scause == 15))
  {
    uint64 va = r_stval();
    if (handle_pgfault(p->pagetable, va) < 0)
    {
      // If the operation fails, treat it as an illegal access and kill the process.
      p->killed = 1;
    }
  }
```
###### (paging.c) 新增 handle_pgfault 的邏輯
```
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
```
###### (vm.c) 呼叫 uvmunmap 釋放 physical memory 時，忽略 unallocated page
```
  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    /*
      walk() starts from the root page table and traverses the multi-level page-table structure using the indices from va until it reaches the PTE.
      -- PTE exists → return pte_t *
      -- Required page-table page is missing and alloc == 0 → return 0
      -- Required page-table page is missing and alloc == 1 → allocate a new page-table page and continue
     */
    pte = walk(pagetable, a, 0);
    // In the case of lazy allocation, this page might never have been mapped.
    if (pte == 0)
    {
      continue;
    }
    // The PTE exists but is not valid, indicating no corresponding physical page.
    if ((*pte & PTE_V) == 0)
    {
      continue;
    }
    //  This is a non-leaf PTE (points to the next level); user space should not call like this.
    if (PTE_FLAGS(*pte) == PTE_V)
    {
      panic("uvmunmap: not a leaf");
    }
    // free the physical memory if required.
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}
```
#### Demand Paging and Swapping
###### (paging.c) 當呼叫 handle_pgfault 時要確認是否該 page swap out 還是 unallocated
```
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
```
###### (vm.c) mdvise function 根據輸入進來的參數 advice 決定要 default、swap-in、swap-out
```
int madvise(uint64 base, uint64 len, int advice)
{
  struct proc *p = myproc();

  // Check if the memory region is valid
  if (base + len > p->sz)
  {
    return -1;
  }

  if (advice == MADV_NORMAL)
  {
    // No special treatment
    return 0;
  }
  else if (advice == MADV_DONTNEED)
  {
    // Swap out pages in the region
    uint64 va_start = PGROUNDDOWN(base);
    uint64 va_end = PGROUNDUP(base + len);

    for (uint64 va = va_start; va < va_end; va += PGSIZE)
    {
      pte_t *pte = walk(p->pagetable, va, 0);

      // Skip if PTE doesn't exist or page not valid
      if (pte == 0 || (*pte & PTE_V) == 0)
      {
        continue;
      }

      // Get physical address
      uint64 pa = PTE2PA(*pte);


      // Allocate a block on disk for swap
      begin_op();
      uint blockno = balloc_page(ROOTDEV);
      if (blockno == 0)
      {
        end_op();
        return -1; // Out of swap space
      }

      // Write page to disk
      write_page_to_disk(ROOTDEV, (char *)pa, blockno);
      end_op();

      // Free physical memory
      kfree((void *)pa);

      // Update PTE: clear V bit, set S bit, store block number
      uint64 flags = PTE_FLAGS(*pte);
      flags &= ~PTE_V; // Clear valid bit
      flags |= PTE_S;  // Set swapped bit
      *pte = ((uint64)blockno << 12) | flags;
    }

    return 0;
  }
  else if (advice == MADV_WILLNEED)
  {
    // Swap in pages or allocate new pages
    uint64 va_start = PGROUNDDOWN(base);
    uint64 va_end = PGROUNDUP(base + len);

    for (uint64 va = va_start; va < va_end; va += PGSIZE)
    {
      pte_t *pte = walk(p->pagetable, va, 0);

      if (pte == 0)
      {
        // Page table entry doesn't exist - allocate new page
        char *mem = kalloc();
        if (mem == 0)
        {
          return -1;
        }
        memset(mem, 0, PGSIZE);

        pte = walk(p->pagetable, va, 1);
        if (pte == 0)
        {
          kfree(mem);
          return -1;
        }
        *pte = PA2PTE(mem) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_U;
      }
      else if (*pte & PTE_S)
      {
        // Page is swapped - swap in
        uint64 blockno = (*pte >> 12) & 0xFFFFFFFFF;
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
      }
      else if ((*pte & PTE_V) == 0)
      {
        // Page not valid and not swapped - allocate new
        char *mem = kalloc();
        if (mem == 0)
        {
          return -1;
        }
        memset(mem, 0, PGSIZE);
        *pte = PA2PTE(mem) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_U;
      }
      // else page is already in memory, do nothing
    }

    return 0;
  }

  return -1;
}
```
## Experiment
![image](https://hackmd.io/_uploads/B1xM9NkWWe.png)
![image](https://hackmd.io/_uploads/By44qV1WWg.png)
![image](https://hackmd.io/_uploads/r1LD9EJbZg.png)
![image](https://hackmd.io/_uploads/r169cNJbWe.png)
![image](https://hackmd.io/_uploads/rySC541-bx.png)
#### bonus
![image](https://hackmd.io/_uploads/SyJu2Ny-bx.png)
![image](https://hackmd.io/_uploads/HyMCnVkZ-x.png)
![image](https://hackmd.io/_uploads/Syoe6Nk-bx.png)




