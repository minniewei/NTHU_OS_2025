#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXVA (1L << (9 + 9 + 9 + 12 - 1)) // 256GB
#define PGSIZE 4096

int main(void)
{
    char *base = sbrk(0);
    char *p;

    /* Grow heap in smaller chunks to approach MAXVA */
    uint64 target = MAXVA - PGSIZE * 10;
    uint64 current = (uint64)base;
    int chunk = 10 * 1024 * 1024; // grow in 10MB chunks
    while (current + chunk < target)
    {
        p = sbrk(chunk);
        if (p == (char *)-1)
            break;
        current += chunk;
    }
    uint64 current_heap = (uint64)sbrk(0);
    printf("----------------- Before sbrk exceeding MAXVA -----------------\n");
    printf("maxvaTest: current heap boundary is at %p\n", (void *)current_heap);

    /* Attempt to exceed MAXVA */
    uint64 space_to_maxva = MAXVA - current_heap;
    int test_size;
    if (space_to_maxva > 0x7FFFFFFF)
        test_size = 1024 * 1024 * 1024;
    else
        test_size = (int)space_to_maxva + PGSIZE;

    p = sbrk(test_size);
    if (p == (char *)-1)
    {
        printf("----------------- After sbrk exceeding MAXVA -----------------\n");
        printf("maxvaTest: OK (sbrk correctly returned -1)\n");
        exit(0);
    }
    else
    {
        printf("maxvaTest: FAILED (sbrk should return -1 when exceeding limit)\n");
        exit(1);
    }
}
