/*
 * Usage: LD_PRELOAD=slow-fork.so COMMAND [ARGUMENT...]
 *
 * Makes fork return to the parent DELAY_MS late, so a child that exits at once
 * does so before the parent's next step. tests/check.sh builds it as a shared
 * object and preloads it into the keepalive.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DELAY_MS 300

pid_t fork(void) {
    pid_t (*real_fork)(void);
    void *symbol = dlsym(RTLD_NEXT, "fork");
    memcpy(&real_fork, &symbol, sizeof real_fork);
    pid_t pid = real_fork();
    if (pid > 0) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = DELAY_MS * 1000000L};
        nanosleep(&delay, NULL);
    }
    return pid;
}
