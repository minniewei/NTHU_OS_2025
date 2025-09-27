# OS25-mp1
## Setup
```
# 將檔案從 branch os-mp1 clone 下來
git clone -b os25-mp1 https://git.lsalab.cs.nthu.edu.tw/os25/os25_team61_xv6

# 進到 os25_team61_xv6 資料夾
cd .\os25_team61_xv6\

# 開啟 docker
docker run -it --rm -v ${PWD}:/xv6 -v /xv6/mkfs -v ${PWD}/mkfs/mkfs.c:/xv6/mkfs/mkfs.c -w /xv6 --platform linux/amd64 dasbd72/xv6:amd64

# 執行測試檔 (./grade-mp1-public)
 ./grade-mp1-public
```
## Implement 
### System Call Trace
#### 在 Makefile 當中新增來源 
```
$U/_trace\
```
#### 撰寫 user/trace.c，並提供 trace System call 的使用範例 (作業提供)
```
#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;
  char *nargv[MAXARG];

  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }

  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i];
  }
  exec(nargv[0], nargv);
  exit(0);
}
```
#### 在 user/user.h 中宣告 trace 函式，作為 user/trace.c 的 user interface
```
int trace(int);
```
#### user/usys.pl
###### 讀取 kernel/syscall.h 的 system call list，並自動生成 usys.S。讓 compiler 知道要怎麼進到 kernel space 使用 system call。
```
entry("trace");
```
#### 在 kernel/proc.h 的 proc struct 當中新增 mask 
```
 int trace_mask;
```
#### 在 kernel/proc.c 的 fork function 當中加入
```
np->trace_mask = p->trace_mask;
```
#### 在 kernel/syscall.h 當中新增定義 
```
#define SYS_trace 22
```
#### 在 kernel/syscall.c 當中新增定義及相關處理 
```
extern uint64 sys_trace(void);
```
###### 在 system call number<->array 對應表當中新增
```
[SYS_trace] sys_trace
```
###### 新增 system call number<->name 對應表
```
static char *syscall_names[] = {
    [SYS_fork] "fork",
    [SYS_exit] "exit",
    [SYS_wait] "wait",
    [SYS_pipe] "pipe",
    [SYS_read] "read",
    [SYS_kill] "kill",
    [SYS_exec] "exec",
    [SYS_fstat] "fstat",
    [SYS_chdir] "chdir",
    [SYS_dup] "dup",
    [SYS_getpid] "getpid",
    [SYS_sbrk] "sbrk",
    [SYS_sleep] "sleep",
    [SYS_uptime] "uptime",
    [SYS_open] "open",
    [SYS_write] "write",
    [SYS_mknod] "mknod",
    [SYS_unlink] "unlink",
    [SYS_link] "link",
    [SYS_mkdir] "mkdir",
    [SYS_close] "close",
    [SYS_trace] "trace",
};
```
###### 更改 syscall function
```
  if (num > 0 && num < NELEM(syscalls) && syscalls[num] && syscall_names[num])
  {
    p->trapframe->a0 = syscalls[num]();
    if ((p->trace_mask >> num) & 1)
    {
      const char *name = syscall_names[num];
      printf("%d: syscall %s -> %d\n", p->pid, name, (int)p->trapframe->a0);
    }
  }
  else
  {
    printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
```
#### 在 kernel/sysproc.c 當中新增 function sys_trace 
```
uint64
sys_trace(void)
{
  int mask;
  argint(0, &mask);
  myproc()->trace_mask = mask; // 存到這個 process 的 trace_mask
  return 0;
}
```
### System Call Sysinfo
## Experiment
### 執行 ./grade-mp1-public
### 執行 ./grade-mp1-bonus
```
#!/usr/bin/env python3

import re
from gradelib import *

r = Runner(save("xv6.out"))

@test(1.25, "trace 0 grep")
def test_trace_0_grep():
    r.run_qemu(shell_script([
        'trace 0 grep hello README'
    ]))
    # no system call should be traced
    r.match(no=[".* syscall .*"])

@test(1.25, "trace 32 trace grep (bad args)")
def test_trace_32_trace_grep():
    r.run_qemu(shell_script([
        'trace 32 trace grep hello README'
    ]))
    # should print usage message
    r.match('^Usage: trace mask command')
    # no system call should be traced
    r.match(no=[".* syscall .*"])

@test(1.25, "trace 32 trace grep (good args)")
def test_trace_32_trace_grep():
    r.run_qemu(shell_script([
        'trace 32 trace 32 grep hello README'
    ]))
    r.match('^\\d+: syscall read -> 1023')
    r.match('^\\d+: syscall read -> 961')
    r.match('^\\d+: syscall read -> 321')
    r.match('^\\d+: syscall read -> 0')

@test(1.25, " trace 4194304 trace grep")
def test_trace_4194304_trace_grep():
    r.run_qemu(shell_script([
        'trace 4194304 trace 32 grep hello README'
    ]))
    r.match('^\\d+: syscall trace -> 0')
    r.match('^\\d+: syscall read -> 1023')
    r.match('^\\d+: syscall read -> 961')
    r.match('^\\d+: syscall read -> 321')
    r.match('^\\d+: syscall read -> 0')

run_tests()
```
#### Explanation
* Test1: trace 0 grep hello README
&emsp; Goal: 測試 mask 為 0 時，process 是否會 crush 掉
&emsp; Output: 無任何輸出
* Test2 : trace 32 trace grep hello README
&emsp; Goal: 測試 trace 能不能追蹤 trace，並發現追蹤的 function 有錯誤
&emsp; Output: 第二個 trace 缺少 mask，輸出 Usage: trace mask command
* Test3 : trace 32 trace 32 grep hello README
&emsp; Goal: 測試 trace 能不能追蹤 trace ，並印出 grep 
&emsp; Output: 同 trace 32 grep hello README
* Test4 : trace 4194304 trace 32 grep hello README
&emsp; Goal: 測試 trace 能不能追蹤 trace ，並印出 trace 
&emsp; Output: 結果大致同 trace 32 grep hello README，但在其前多一行 syscall trace -> 0

#### Result
![image](https://hackmd.io/_uploads/rkkeRp42lg.png)


## Reference