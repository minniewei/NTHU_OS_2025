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
    // 忙迴圈：不呼叫 I/O，不主動 yield
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
    write(p[1], "A", 1); // 告訴父行程：我成功跑到了
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

  // 清理子行程（避免殭屍）
  // 殺掉可能還在跑的 spinner/timer
  // （如果已經結束，kill 不會有影響）
  kill(pid_aged);
  kill(pid_timer);
  for (int i = 0; i < N_SPINNERS + 2; i++)
    wait(0);

  exit(0);
}
