/*
 * Usage: LD_PRELOAD=failed-fork.so COMMAND [ARGUMENT...]
 *
 * Makes every fork fail with EAGAIN, as it does at the process limit. The
 * programs COMMAND runs do not inherit it. tests/check.sh builds it as a
 * shared object and preloads it into the keepalive.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

__attribute__((constructor)) static void hide_from_children(void) {
    unsetenv("LD_PRELOAD");
}

pid_t fork(void) {
    errno = EAGAIN;
    return -1;
}
