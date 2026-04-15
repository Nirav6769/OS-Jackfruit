/* io_pulse.c — repeatedly writes and reads a temp file */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    printf("io_pulse: starting\n");
    fflush(stdout);
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    int count = 0;
    while (1) {
        FILE *f = fopen("/tmp/io_pulse_tmp", "w");
        if (f) { fwrite(buf, 1, sizeof(buf), f); fclose(f); }
        f = fopen("/tmp/io_pulse_tmp", "r");
        if (f) { fread(buf, 1, sizeof(buf), f); fclose(f); }
        if (++count % 1000 == 0) {
            printf("io_pulse: %d iterations\n", count);
            fflush(stdout);
        }
        usleep(1000);
    }
    return 0;
}
