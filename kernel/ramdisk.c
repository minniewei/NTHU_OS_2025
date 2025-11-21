//
// ramdisk for swap space
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple in-memory swap space (2MB = 512 blocks of 4KB each)
#define SWAPSIZE (512 * BSIZE)
static char swapspace[SWAPSIZE];

void ramdiskinit(void)
{
  // Initialize swap space to zero
  memset(swapspace, 0, SWAPSIZE);
}

// If B_DIRTY is set, write buf to swap, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from swap, set B_VALID.
void ramdiskrw(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("ramdiskrw: buf not locked");

  // Check if block number is within swap space
  if (b->blockno >= SWAPSIZE / BSIZE)
    panic("ramdiskrw: blockno too big");

  uint64 offset = b->blockno * BSIZE;
  char *addr = swapspace + offset;

  // Always perform the operation - either write or read
  if (b->disk == 1)
  {
    // write to swap
    memmove(addr, b->data, BSIZE);
    b->disk = 0;
  }
  else
  {
    // read from swap
    memmove(b->data, addr, BSIZE);
  }
  b->valid = 1;
}
