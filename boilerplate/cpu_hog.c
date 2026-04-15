/* cpu_hog.c — spins a loop to consume CPU */
#include <stdio.h>
#include <time.h>

int main(void) {
    printf("cpu_hog: starting\n");
    fflush(stdout);
    volatile long x = 0;
    time_t start = time(NULL);
    while (1) {
        for (long i = 0; i < 1000000L; i++) x += i;
        if (time(NULL) - start >= 1) {
            printf("cpu_hog: still running (x=%ld)\n", x);
            fflush(stdout);
            start = time(NULL);
        }
    }
    return 0;
}
