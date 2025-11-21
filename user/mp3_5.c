#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

int main(int argc, char *argv[])
{
    char *base = sbrk(0);
    printf("overheap: initial haep at %p\n", base);

    char *bad = sbrk(0) + PGSIZE;
    printf("overheap: about to write to %p (beyond heap, should kill process)\n", bad);

    *bad = 1;

    printf("overheap: ERROR, write beyond heap did not kill the process\n");
    exit(1);
}
