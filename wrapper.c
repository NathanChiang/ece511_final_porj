#include <gem5/m5ops.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "--exit-only") == 0) {
        printf("===== WRAPPER_M5_EXIT_ONLY =====\n");
        m5_exit(0);
        return 0;
    }

    printf("\n===== BENCHMARK_START: /root/benchmark -p 1 -m18 =====\n");

    // Tell gem5: zero all counters now. Everything before this point
    // (boot, kernel init, and this marker) is excluded from the stats.
    m5_reset_stats(0, 0);

    pid_t child = fork();
    if (child == -1) {
        perror("fork failed");
        printf("===== BENCHMARK_DONE: launch failed =====\n");
        m5_fail(0, 1);
        return 1;
    }

    if (child == 0) {
        char *const bench_argv[] = {
            "/root/benchmark",
            "-p", "1",
            "-m18",
            NULL
        };
        execv("/root/benchmark", bench_argv);
        fprintf(stderr, "execv failed: %s\n", strerror(errno));
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) == -1) {
        perror("waitpid failed");
        printf("===== BENCHMARK_DONE: wait failed =====\n");
        m5_fail(0, 1);
        return 1;
    }

    m5_dump_stats(0, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        printf("===== BENCHMARK_DONE: exit_code=%d =====\n", code);
        if (code == 0) {
            m5_exit(0);
        } else {
            m5_fail(0, code);
        }
        return code;
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("===== BENCHMARK_DONE: signal=%d =====\n", sig);
        m5_fail(0, 128 + sig);
        return 128 + sig;
    }

    printf("===== BENCHMARK_DONE: unexpected_status=%d =====\n", status);
    m5_fail(0, 1);
    return 1;
}
