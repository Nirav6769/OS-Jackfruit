/* memory_hog.c — allocates memory in chunks to trigger soft/hard limits */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int mb = argc > 1 ? atoi(argv[1]) : 100;
    printf("memory_hog: allocating %d MB\n", mb);
    fflush(stdout);

    size_t chunk = 1024 * 1024; /* 1 MB */
    for (int i = 0; i < mb; i++) {
        char *p = malloc(chunk);
        if (!p) { printf("memory_hog: malloc failed at %d MB\n", i); break; }
        memset(p, i & 0xFF, chunk);  /* touch the pages so RSS actually grows */
        printf("memory_hog: allocated %d MB so far\n", i + 1);
        fflush(stdout);
        sleep(1);
    }
    printf("memory_hog: done, sleeping\n");
    fflush(stdout);
    while (1) sleep(10);
    return 0;
}
