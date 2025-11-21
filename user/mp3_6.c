#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MADV_NORMAL 0
#define MADV_WILLNEED 1
#define MADV_DONTNEED 2

char *testname = "madvise_swap_fault";

void err(char *why)
{
    printf("madvise_swap_fault: %s failed: %s, pid=%d\n",
           testname, why, getpid());
    exit(1);
}

int main(int argc, char *argv[])
{
    char *p;
    int i;

    printf("------------------------ Before Swap-Out ------------------------\n");
    vmprint();

    /* allocate a page */
    p = sbrk(PGSIZE);
    if (p == (char *)-1)
    {
        printf("sbrk failed\n");
        exit(1);
    }

    /* Write a test pattern into the page */
    for (i = 0; i < PGSIZE; i++)
    {
        p[i] = (char)(i & 0xFF);
    }

    printf("------------------------ After Swap-Out ------------------------\n");
    /* call MADV_DONTNEED to advise the OS to swap this page to disk */
    if (madvise(p, PGSIZE, MADV_DONTNEED) != 0)
        err("madvise MADV_DONTNEED failed");
    vmprint();

    printf("------------------------ After Swap-In ------------------------\n");
    for (i = 0; i < PGSIZE; i++)
    {
        char expected = (char)(i & 0xFF);
        char got = p[i];
        if (got != expected)
        {
            printf("mismatch at offset %d: expected %d, got %d\n",
                   i, (int)expected, (int)got);
            err("data corrupted after swap-in");
        }
    }
    vmprint();

    exit(0);
}