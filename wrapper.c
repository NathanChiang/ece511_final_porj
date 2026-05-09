#include <gem5/m5ops.h>
#include <unistd.h>
#include <stdio.h>

int main(void) {
    // Tell gem5: zero all counters now.
    // Everything before this point (boot, kernel init) is excluded.
    m5_reset_stats(0, 0);

    // Replace this process with the benchmark.
    // exec() does not return on success.
    char *const argv[] = { "/root/benchmark", NULL };
    execv("/root/benchmark", argv);

    // Only reached if execv fails.
    perror("execv failed");
    return 1;
}