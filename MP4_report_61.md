# OS HW4 Report
## Trace Code
1. **Writing to a Large File** 
* 用戶程序調用 write()，控制流進入 sys_write()，進行參數驗證。
```c
ssize_t sys_write(int fd, const void *buf, size_t count) {
    // 驗證文件描述符
    if (!is_valid_fd(fd))
        return -EBADF; // 無效的文件描述符

    // 調用文件寫入函數
    return filewrite(fd, buf, count);
}
```
* 接著進入 filewrite()，進行文件相關的檢查和準備。
```c
ssize_t filewrite(int fd, const void *buf, size_t count) {
    struct file *f = get_file(fd);
    // 檢查文件是否可寫
    if (!(f->flags & O_WRONLY))
        return -EINVAL; // 無效的寫入請求

    // 實際寫入數據
    return writei(f->inode, buf, count);
}
```
* 進一步調用 writei()，將數據寫入文件系統。
```c
ssize_t writei(struct inode *inode, const void *buf, size_t count) {
    // 獲取文件的鎖
    lock_inode(inode);
    // 執行寫入操作
    ssize_t bytes_written = perform_write(inode, buf, count);
    unlock_inode(inode);
    return bytes_written;
}
```
* 最後，透過 bmap() 確保數據映射到正確的物理位置。
```c
block_t bmap(struct inode *inode, off_t block) {
    // 計算邏輯塊對應的物理塊
    return get_physical_block(inode, block);
}
```
2. **Deleting a Large File** 
* 用戶調用 rm，最終觸發 sys_unlink() 系統調用。sys_unlink() 通過文件路徑獲取 inode，並調用 unlink_inode() 刪除文件。
```c
int sys_unlink(const char *pathname) {
    struct inode *inode = get_inode(pathname);
    if (!inode)
        return -ENOENT; // 文件不存在

    // 調用刪除函數
    return unlink_inode(inode);
}
```
```c
int unlink_inode(struct inode *inode) {
    // 清空文件數據
    itrunc(inode);
    // 釋放 inode 的鎖和引用
    iunlockput(inode);
    return 0; // 刪除成功
}
```
* 在 unlink_inode() 中，調用 iunlockput() 釋放 inode 的鎖並減少引用計數。如果引用計數降為零，則會釋放 inode。
```c
void iunlockput(struct inode *inode) {
    unlock_inode(inode);
    // 減少引用計數
    if (--inode->ref_count == 0) {
        // 如果引用計數為零，則釋放 inode
        free_inode(inode);
    }
}
```
* 調用 itrunc() 清空文件內容，釋放佔用的磁碟空間。
```c
void itrunc(struct inode *inode) {
    // 清空文件的數據區
    clear_file_data(inode);
    // 可能需要更新 inode 的大小
    inode->size = 0;
}
```
## Question
1. **What is the current maximum file size xv6 can support? How many numbers of direct and indirect blocks are there?** 

* 根據以下定義，每個 inode 有 12 個直接塊和 0 個間接塊，因此總大小為 $12 \times 1024 = 12,288$ bytes。
```c
#define NDIRECT 12
```

2. **How many blocks increase when replacing a direct block with a doubly-indirect block?** 

* 用一個雙重間接塊替換一個直接塊，容量將增加 $256 \times 256 - 1 = 65,535$ blocks。

3. **The bmap and itrunc functions make use of the buffer cache via bread, brelse, and log_write. Explain the above function .**

* bmap 函數的主要作用是將邏輯塊號映射到物理塊號。在需要讀取或寫入某個邏輯塊時，bmap 會查找其對應的物理塊。如果該物理塊不在緩衝區快取中，bmap 會使用 bread 函數從磁碟讀取該塊並將其放入緩衝區快取，這樣可以提高讀取效率，因為未來對該塊的訪問將直接從緩衝區快取中獲取。使用完畢後，通過 brelse 函數釋放該緩衝區，以便其他操作能夠使用。
```c
int bmap(uint dev, uint block) {
    struct superblock sb;
    // 獲取超級區塊信息
    readsb(dev, &sb);
    
    // 根據邏輯塊號計算物理塊號
    uint physical_block = block; // 替換為實際計算

    // 使用緩衝區快取讀取物理塊
    struct buf *b = bread(dev, physical_block);

    // 返回物理塊號
    return physical_block;
}
```
* itrunc 函數用於截斷文件，即減少文件的大小。當文件需要被截斷時，itrunc 會首先確定需要刪除的塊，然後通過 bmap 獲取這些塊的物理地址，並使用 bread 讀取相關的塊。接著，通過更新 inode 的信息，將文件的大小設置為新的值，最後，使用 log_write 將這些更改寫入日誌，以確保數據的一致性。
```c
void itrunc(struct inode *ip, uint new_size) {
    uint block;
    // 更新 inode 的大小
    ip->size = new_size;

    // 確定需要截斷的塊
    for (block = new_size / BSIZE; block < ip->size / BSIZE; block++) {
        // 使用 bmap 獲取物理塊
        uint physical_block = bmap(ip->dev, block);
        // 釋放塊
        bfree(ip->dev, physical_block);
    }
    
    // 寫入日誌以確保一致性
    log_write(ip);
}
```
4. **Explain the importance of calling brelse after you are done with a buffer from bread.**

* 在使用 bread 函數讀取緩衝區後，調用 brelse 函數釋放該緩衝區是非常重要的，因為這有助於有效管理系統資源。每次調用 bread 時，系統會佔用內存，如果不及時釋放，將導致內存洩漏，影響系統的穩定性和性能。通過釋放緩衝區，緩衝區快取能夠更有效地管理數據，並根據需要重新使用或替換這些釋放的緩衝區。此外，及時釋放緩衝區還能幫助確保數據的一致性，特別是在寫入操作後。因此，正確調用 brelse 是確保系統資源有效管理和性能優化的關鍵步驟。

5. **To ensure crash safety, xv6 uses a write-ahead log. Trace the lifecycle of a file system transaction when creating a new file. Start from a system call like sys_open() in kernel/sysfile.c that calls create(). Explain the roles of begin_op(), log_write(), and end_op() from kernel/log.c. Describe what gets written to the on-disk log and what constitutes a "commit". Finally, explain how the recovery code (recover_from_log) uses the log to ensure the operation is atomic after a crash. We strongly encourage you to read the logging part(8.4) in xv6 handbook to get familiar with loggin mechanism.**

* 用戶通過調用 sys_open() 來創建一個新文件。這個系統調用首先會檢查該文件是否已存在，然後進入文件創建的邏輯，最終調用 create() 函數來處理實際的文件創建。
```c
int sys_open(void) {
    char *path;
    int fd, omode;

    if (argstr(0, &path) < 0 || argint(1, &omode) < 0)
        return -1;
    if (omode & O_CREATE) {
        return create(path);  // 調用 create() 函數創建新文件
    }
    // 其他打開模式處理...
}
```
* 在 create() 函數中，首先調用 begin_op()。這個函數的作用是開始一個新的事務，並確保對日誌的獨佔訪問。通過獲取日誌鎖，begin_op() 保證在這個事務的整個過程中不會有其他事務干擾，從而維持操作的原子性。
```c
int create(char *path) {
    struct inode *ip;
    begin_op();  // 開始事務

    // 分配一個新的 inode
    ip = ialloc(ROOTDEV);
    if (ip == 0) {
        end_op(); // 結束事務
        return -1;
    }
    
    // 更新目錄
    if (dirlink(ip, path) < 0) {
        iput(ip);
        end_op(); // 結束事務
        return -1;
    }

    // 記錄變更到日誌
    log_write(ip);
    log_write(...);  // 其他元數據的寫入
    
    end_op();  // 結束事務
    return 0;  // 成功創建文件
}
```
```c
void begin_op(void) {
    acquire(&log.lock);
    // 其他初始化...
}
```
* 接下來，create() 函數會執行一系列操作來創建文件。在此過程中，所有需要寫入的數據（如 inode、目錄條目等）都會使用 log_write() 函數寫入到日誌中，而不是直接寫入到磁碟。這樣做的目的是確保即使在寫入過程中發生崩潰，系統仍然能夠通過日誌恢復到一致的狀態。
```c
void log_write(struct inode *ip) {
    // 將 inode 的變更寫入日誌
    // 實際寫入操作...
}
```
* 當所有的變更都已經寫入日誌後，調用 end_op() 函數來結束事務。這個函數會釋放對日誌的鎖，並將日誌中的變更正式寫入到磁碟上，實現“提交”操作。這樣，所有先前的變更將被確保持久化，並且用戶可以在系統崩潰後再次訪問新創建的文件。
```c
void end_op(void) {
    // 提交日誌中的變更到磁碟
    release(&log.lock);
}
```
* 在 end_op() 完成後，所有在事務中記錄的變更都已經寫入到磁碟，這標誌著事務的成功提交。此時，該文件的創建被認為是成功的，並且系統會將這些變更反映在文件系統的結構中。
    - 在 xv6 文件系統中，當進行文件創建等操作時，相關的變更會被寫入到磁碟日誌中，這些變更包括新分配的 inode 的元數據、目錄條目的更新以及數據區塊的修改。這種寫前日誌機制確保所有操作首先被記錄到日誌中，以便在系統崩潰時能夠恢復一致性。而“提交”則是將日誌中的所有變更持久化到磁碟的過程，具體包括將日誌內容寫入磁碟、更新文件系統結構（如目錄的變更）以及釋放日誌鎖。只有當這些步驟全部完成後，該操作才被認為是成功提交，確保用戶能夠安全地訪問新創建的文件，而不必擔心數據不一致或丟失的問題。這種機制有效地保證了文件系統操作的原子性和持久性。
* 如果在文件創建過程中系統發生了崩潰，recover_from_log 函數將被調用來恢復操作。這個函數會檢查日誌中未提交的操作，並根據日誌的內容來確定哪些操作需要重試或忽略。具體來說，若日誌中有關於新文件創建的記錄，系統會重新執行這個操作；如果沒有記錄，則表示該操作未被執行，系統會將其忽略。這樣，系統能夠保持一致性，確保文件系統的穩定性。
```c
void recover_from_log(void) {
    // 讀取日誌，檢查未提交的操作
    // 根據日誌內容執行恢復
}
```

7. **Explain the roles of the in-memory inode functions iget() and iput(). Describe the purpose of the reference count (ref) in struct inode**

* iget() 函數的主要功能是從磁碟中加載指定的 inode，並返回其在內存中的指針，並在此過程中增加該 inode 的引用計數，以表示有新的引用指向它。
* iput() 函數用於釋放對 inode 的引用，減少引用計數；如果計數減至零，表示沒有進程再使用該 inode，則可以將其寫回磁碟並釋放內存資源。
* struct inode 中的引用計數（ref）主要用於追蹤有多少進程或操作正在使用該 inode，從而有效管理內存資源、確保數據一致性，並支持並發訪問。這種引用計數機制能防止在仍有進程使用某個 inode 時意外釋放其資源，確保系統的穩定性和數據的完整性。
## Implementation
1. **bmap**
* 處理直接區塊的分配。
```c
  // 1) 直接區塊 (0 - 10)
  if (bn < NDIRECT)
  {
    if ((addr = ip->addrs[bn]) == 0)
    {
      addr = balloc(ip->dev);
      if (addr == 0)
        return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;
```
* 管理一級間接區塊的分配。首先，它檢查是否已分配一級間接索引塊，若尚未分配，則使用 balloc 函數分配新區塊並立即清零以防止垃圾指標。接著，它讀取該一級間接區塊的內容，檢查特定塊的地址是否為 0，若是，則分配新的數據塊並更新對應的地址。最後，程式碼釋放讀取的緩衝區，以確保資源的有效管理。
```c
  // 2) 一級間接區塊 (原本的 addrs[11])
  if (bn < NINDIRECT)
  {
    // 如果一級間接索引塊還沒分配
    if ((addr = ip->addrs[NDIRECT]) == 0)
    {
      addr = balloc(ip->dev);
      if (addr == 0)
        return 0;

      // 【關鍵】分配後立刻清零，防止垃圾指標
      struct buf *zbp = bread(ip->dev, addr);
      memset(zbp->data, 0, BSIZE);
      log_write(zbp);
      brelse(zbp);

      ip->addrs[NDIRECT] = addr;
    }

    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *)bp->data;
    if ((addr = a[bn]) == 0)
    {
      addr = balloc(ip->dev);
      if (addr != 0)
      {
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;
```
* 負責管理雙重間接區塊的分配。首先，它檢查請求的塊 bn 是否在雙重間接區塊的有效範圍內。如果未分配雙重間接索引塊，程式碼將使用 balloc 函數進行分配，並立即清零以防止垃圾指標。接著，透過 bread 函數讀取該雙重間接區塊的內容，並將其數據視為 uint 類型的指針。最後，程式碼計算第一層和第二層索引，以便在雙重間接區塊中精確定位所請求的數據塊。
```c
  // 3) 雙重間接區塊 (原本的 addrs[12])
  if (bn < NDBL)
  {
    int dbl_idx = NDIRECT + NSINGLE;

    // 3-1. 確保雙重間接一級索引塊存在 (addrs[12])
    if ((addr = ip->addrs[dbl_idx]) == 0)
    {
      addr = balloc(ip->dev);
      if (addr == 0)
        return 0;

      // 【關鍵】分配後立刻清零
      struct buf *zbp = bread(ip->dev, addr);
      memset(zbp->data, 0, BSIZE);
      log_write(zbp);
      brelse(zbp);

      ip->addrs[dbl_idx] = addr;
    }

    bp = bread(ip->dev, ip->addrs[dbl_idx]);
    a = (uint *)bp->data;
    uint i1 = bn / NINDIRECT; // 第一層索引
    uint i2 = bn % NINDIRECT; // 第二層索引
```
* 確保二級索引塊存在，如果尚未分配，則分配一個新的索引塊並立即清零，然後更新第一層索引陣列以包含新分配的二級索引塊的地址，最後釋放第一層索引塊的緩衝區。
```c
    // 3-2. 確保二級索引塊存在
    uint saddr = a[i1];
    if (saddr == 0)
    {
      saddr = balloc(ip->dev);
      if (saddr == 0)
      {
        brelse(bp);
        return 0;
      }
      // 【關鍵】分配後立刻清零
      struct buf *zbp = bread(ip->dev, saddr);
      memset(zbp->data, 0, BSIZE);
      log_write(zbp);
      brelse(zbp);

      a[i1] = saddr;
      log_write(bp);
    }
    brelse(bp); // 釋放第一層索引塊
```
* 從設備中讀取二級索引塊，並尋找最終的資料塊。首先，它利用 bread 函數讀取二級索引塊，並檢查對應的地址 a[i2] 是否為 0，若為 0，則表示該資料塊尚未分配。接著，程式碼調用 balloc 函數分配一個新的資料塊，並在成功分配後將其地址存儲在二級索引塊中，並記錄該修改。最後，釋放二級索引塊的緩衝區，並返回資料塊的地址。
```c
    // 3-3. 讀取二級索引塊並找到最終資料塊
    bp = bread(ip->dev, saddr);
    a = (uint *)bp->data;
    if ((addr = a[i2]) == 0)
    {
      addr = balloc(ip->dev);
      if (addr != 0)
      {
        a[i2] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
```
2. **itrunc**
* 釋放直接塊，通過迴圈檢查每個直接地址，若存在則調用 bfree 函數釋放該塊，並將相應的地址設置為 0。
```c
  // 1) Free direct blocks
  // -----------------------
  for (i = 0; i < NDIRECT; i++)
  {
    if (ip->addrs[i])
    {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
```
* 釋放單級間接塊（當 NSINGLE 為 1 時）。首先，它檢查 NSINGLE 是否為真，並確認 ip->addrs[NDIRECT] 是否存在。若條件滿足，則使用 bread 函數讀取單級間接塊，並將數據指針賦值給 a1。接著，通過迴圈遍歷單級間接塊中的每個地址，釋放存在的地址並將其設置為 0。最後，釋放單級間接塊的緩衝區，並釋放單級間接塊本身，將 ip->addrs[NDIRECT] 設置為 0。這樣做確保了與單級間接塊相關的所有資源被正確釋放並重置。
```c
  // 2) Free singly-indirect blocks (if NSINGLE=1)
  // inode slot: addrs[NDIRECT]
  // -----------------------
  if (NSINGLE && ip->addrs[NDIRECT])
  {
    bp1 = bread(ip->dev, ip->addrs[NDIRECT]);
    a1 = (uint *)bp1->data;
    for (j = 0; j < NINDIRECT; j++)
    {
      if (a1[j])
      {
        bfree(ip->dev, a1[j]);
        a1[j] = 0;
      }
    }
    brelse(bp1);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
```
* 釋放雙級間接塊（當 NDOUBLE 大於或等於 1 時）。它首先檢查雙級間接塊的存在性，然後遍歷每個雙級間接塊的地址，對每個存在的雙級間接塊進行讀取。接著，對每個雙級間接塊中的單級間接塊進行遍歷，釋放其內部的所有數據塊，並將相關地址設置為 0。最後，釋放雙級間接塊本身並將其地址設置為 0，並重置 inode 的大小以確保所有相關資源被釋放。
```c
  // 3) Free doubly-indirect blocks (NDOUBLE >= 1)
  // inode slots start at: dbl_base = NDIRECT + NSINGLE
  // Each doubly block points to NINDIRECT singly-indirect blocks,
  // each singly block points to NINDIRECT data blocks.
  // -----------------------
  if (NDOUBLE)
  {
    int dbl_base = NDIRECT + NSINGLE;

    for (i = 0; i < NDOUBLE; i++)
    {
      uint daddr = ip->addrs[dbl_base + i];
      if (daddr == 0)
        continue;

      // Read doubly-indirect block
      bp1 = bread(ip->dev, daddr);
      a1 = (uint *)bp1->data;

      for (j = 0; j < NINDIRECT; j++)
      {
        uint saddr = a1[j];
        if (saddr == 0)
          continue;
        // Read singly-indirect block
        bp2 = bread(ip->dev, saddr);
        a2 = (uint *)bp2->data;
        for (k = 0; k < NINDIRECT; k++)
        {
          if (a2[k])
          {
            bfree(ip->dev, a2[k]);
            a2[k] = 0;
          }
        }
        brelse(bp2);
        bfree(ip->dev, saddr);
        a1[j] = 0;
      }
      brelse(bp1);
      bfree(ip->dev, daddr);
      ip->addrs[dbl_base + i] = 0;
    }
  }
  ip->size = 0;
  iupdate(ip);
```
3. **namex**
* 檢查在處理路徑的最後一個元素時是否應該跟隨符號鏈接。當 notfollow_last 參數被設置為真且當前查找的 next 元素是符號鏈接，並且已經到達路徑的結尾時，程式碼將不會跟隨這個符號鏈接，而是直接返回該符號鏈接的 inode。
```c
    // If notfollow_last is set and this is the last path element,
    // do not follow symbolic links.
    if (notfollow_last && next->type == T_SYMLINK && *path == '\0')
    {
      iunlockput(ip);
      return next;
    }
```
* 處理符號鏈接，確保在遍歷路徑時能正確解析符號鏈接的目標。當 next 是符號鏈接且路徑尚未結束時，程式碼會檢查深度以防止無限循環，若深度達到5則返回0。接著，它鎖定符號鏈接 inode，讀取其目標路徑，並在讀取成功後釋放鎖。如果目標 inode 查找成功，則更新 next 為新的目標 inode，繼續處理，直到路徑結束或不再是符號鏈接。這樣的設計確保了符號鏈接的正確解析和循環檢測。
```c
    // --  Deal with Symbolic Links  --
    while (next->type == T_SYMLINK && *path != '\0')
    {
      if (depth >= 5)
      { // 循環偵測
        iput(next);
        return 0;
      }
      depth++;

      ilock(next);
      char target[MAXPATH];
      // Get the target path from the symbolic link inode
      if (readi(next, 0, (uint64)target, 0, MAXPATH) <= 0)
      {
        iunlockput(next);
        return 0;
      }
      iunlockput(next);

      // Re-parse the symbolic link path from the root directory
      // only considers absolute paths
      struct inode *temp = namei(target);
      iput(next);
      if (temp == 0)
        return 0;
      next = temp;
    }
```
4. **sys_open**
* 追蹤符號鏈接，以便在文件系統中正確解析其目標路徑。首先，程式碼初始化一個深度計數器以防止遞迴過深。當當前 inode 是符號鏈接時，若指定了 O_NOFOLLOW 標誌，則釋放 inode 鎖並返回 -2。接著，若達到深度限制（5），則釋放鎖並返回 -3，避免環狀連結。在成功讀取符號鏈接的目標路徑後，程式碼釋放當前 inode 的鎖和引用，然後使用 namei 函數尋找目標路徑的 inode。如果成功找到，則鎖定新的 inode，並增加深度計數。這一過程持續進行，直到不再是符號鏈接。
```c
  // --- Symbolic Link 追蹤邏輯 ---
  int depth = 0;
  while (ip->type == T_SYMLINK)
  {
    // 如果指定 O_NOFOLLOW 且目前是連結檔，回傳 -2
    if (omode & O_NOFOLLOW)
    {
      iunlockput(ip);
      end_op();
      return -2;
    }

    // 檢查遞迴深度，防止環狀連結 (Cycle)
    if (depth >= 5)
    {
      iunlockput(ip);
      end_op();
      return -3;
    }

    char target[MAXPATH];
    // 從 inode 資料區塊讀取目標路徑
    int r = readi(ip, 0, (uint64)target, 0, MAXPATH - 1);
    if (r <= 0)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
    target[r] = '\0'; // 確保字串結尾

    // 關鍵：釋放目前的 inode 鎖與引用，才能搜尋下一個路徑
    iunlockput(ip);

    // 尋找目標路徑的 inode
    if ((ip = namei(target)) == 0)
    {
      end_op();
      return -1;
    }
    ilock(ip);
    depth++;
  }
  // --- 追蹤結束 ---
```
5. **sys_chdir**
* 當當前 inode 是符號鏈接時，程式碼首先檢查深度是否超過5，以防止過深的遞迴，若超過則釋放當前 inode 的鎖並返回 -1。然後，程式碼增加深度計數並讀取符號鏈接的目標路徑，確保在讀取成功的情況下將其以空字符結尾。接著，為避免死鎖，程式碼釋放當前 inode 的鎖，然後使用 namei 函數重新解析目標路徑。如果找到目標 inode，則重新鎖定它，並繼續處理，直到不再是符號鏈接。
```c
  while (ip->type == T_SYMLINK)
  {
    if (depth >= 5)
    { // 深度限制為 5
      iunlockput(ip);
      end_op();
      return -1;
    }
    depth++;

    char target[MAXPATH];
    // readi 需要 inode 被 locked
    int n = readi(ip, 0, (uint64)target, 0, MAXPATH);
    if (n <= 0)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
    target[n] = '\0';

    // 必須先解鎖當前連結，才能 namei 下一個路徑，否則會 Deadlock
    iunlockput(ip);

    // 重新解析目標路徑
    if ((ip = namei(target)) == 0)
    {
      end_op();
      return -1;
    }
    ilock(ip);
  }
```
6. **sys_symlink**
* 定義了一個名為 sys_symlink 的函數，用於在文件系統中創建一個符號鏈接（symbolic link）。首先，函數從參數中獲取目標路徑和鏈接路徑，如果失敗則返回 -1。接著，函數開始一個操作，並使用 create 函數在指定的路徑上創建一個類型為 T_SYMLINK 的新 inode。如果創建失敗，將結束操作並返回 -1。然後，函數計算目標路徑的長度，並將其寫入新創建的 inode 中，確保寫入的字節數與目標路徑的長度一致。如果寫入失敗，則釋放當前 inode 的鎖並返回 -1。最後，釋放 inode 的鎖並結束操作，成功時返回 0。
```c
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;
  int n;

  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();

  // create a new inode of type T_SYMLINK at "path"
  ip = create(path, T_SYMLINK, 0, 0);
  if (ip == 0)
  {
    end_op();
    return -1;
  }
  n = strlen(target) + 1;
  if (writei(ip, 0, (uint64)target, 0, n) != n)
  {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlockput(ip);

  end_op();
  return 0;
}
```
## Experiment
![螢幕擷取畫面 2025-12-21 145842](https://hackmd.io/_uploads/H1Q7MPSmbx.png)
