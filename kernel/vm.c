#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "proc.h"
#include "vm.h"

// Use disk for swap instead of memory
// We use ROOTDEV (device 1) for swap operations

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// Initialize the one kernel_pagetable
void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if (size == 0)
    panic("mappages: size");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

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

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

/* Helper function to print the flags of a PTE */
static void vmprint_flags(pte_t pte)
{
  // Valid bit (in memory)
  if (pte & PTE_V)
    printf(" V");
  // Readable
  if (pte & PTE_R)
    printf(" R");
  // Writable
  if (pte & PTE_W)
    printf(" W");
  // Executable
  if (pte & PTE_X)
    printf(" X");
  // User accessible
  if (pte & PTE_U)
    printf(" U");
  // Swapped out (on disk)
  if (pte & PTE_S)
    printf(" S");
}

/* Construct a virtual address by combining va_prefix with the index i at the specified level. */
static inline uint64 va_with_index(uint64 va_prefix, int level, int i)
{
  int shift = PGSHIFT + 9 * level;
  uint64 keep_high = ~((1ULL << (shift + 9)) - 1);
  return (va_prefix & keep_high) | ((uint64)i << shift);
}

/* Recursive helper function to print the page table entries. */
static void vmprint_walk(pagetable_t pt, int level, uint64 va_prefix)
{
  if (level < 0)
    return;

  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pt[i];
    // Skip if PTE is completely empty (not valid and not swapped)
    if ((pte & PTE_V) == 0 && (pte & PTE_S) == 0)
      continue;

    // A valid PTE may either map to a physical page (leaf) or point to a lower-level page table (non-leaf), depending on whether the R/W/X bits are set.
    uint64 va_nofs = va_with_index(va_prefix, level, i);
    uint64 pa_nofs = PTE2PA(pte);

    // Indentation: print 2 * (3 - level) spaces
    for (int s = 0; s < 2 * (3 - level); s++)
      printf(" ");

    // Print according to the specified format
    printf("%d: pte=%p va=%p pa=%p", i, (void *)pte, (void *)va_nofs, (void *)pa_nofs);

    // If swapped, show block number
    if (pte & PTE_S)
    {
      uint64 blockno = pa_nofs >> 12; // Extract block number from PTE
      printf(" blockno=%p", (void *)blockno);
    }

    vmprint_flags(pte);
    printf("\n");

    // Recursive call for non-leaf PTEs (only if valid)
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      vmprint_walk((pagetable_t)pa_nofs, level - 1, va_nofs);
    }
  }
}

/* Print multi layer page table. */
void vmprint(pagetable_t pagetable)
{
  printf("page table 0x%p\n", pagetable);
  vmprint_walk(pagetable, 2, 0);
}

/* Map pages to physical memory or swap space. */
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
    // Swap out pages in the region to disk
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
      *pte = BLOCKNO2PTE(blockno) | flags;
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