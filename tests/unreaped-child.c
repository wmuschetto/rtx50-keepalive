/*
 * Usage: LD_PRELOAD=unreaped-child.so COMMAND [ARGUMENT...]
 *
 * Ends COMMAND's process with status 67 as it exits if it has no exited child
 * left unreaped: a child reaped before then gave up its pid, which the kernel
 * may hand to another process. The programs COMMAND runs do not inherit it.
 * tests/check.sh builds it as a shared object and preloads it into the
 * keepalive.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIT_CHILD_REAPED 67

static void check_child_unreaped(void) {
    siginfo_t exited;
    exited.si_pid = 0;
    if (waitid(P_ALL, 0, &exited, WEXITED | WNOHANG | WNOWAIT) != 0 || exited.si_pid == 0) {
        _exit(EXIT_CHILD_REAPED);
    }
}

__attribute__((constructor)) static void watch_exit(void) {
    atexit(check_child_unreaped);
    unsetenv("LD_PRELOAD");
}
