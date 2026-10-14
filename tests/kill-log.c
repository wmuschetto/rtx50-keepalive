/*
 * Usage: LD_PRELOAD=kill-log.so KILL_LOG=FILE COMMAND [ARGUMENT...]
 *
 * Creates FILE empty and writes a line "kill(PID, SIGNAL)" to it for each
 * kill() COMMAND makes, then makes the call. The programs COMMAND runs do not
 * inherit it. tests/check.sh builds it as a shared object and preloads it into
 * the keepalive.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Room for "kill(", two numbers of up to 11 characters, ", " and ")\n". */
#define LINE_SIZE 64

static int (*real_kill)(pid_t, int);
static int log_file = -1;

__attribute__((constructor)) static void open_log(void) {
    void *symbol = dlsym(RTLD_NEXT, "kill");
    memcpy(&real_kill, &symbol, sizeof real_kill);
    const char *path = getenv("KILL_LOG");
    if (path != NULL) {
        log_file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    }
    unsetenv("LD_PRELOAD");
}

/* Writes number in decimal at text + length; returns the new length. By hand,
 * since kill() runs in signal handlers, where snprintf is not safe. */
static size_t append_number(char *text, size_t length, long number) {
    unsigned long magnitude = number < 0 ? 0UL - (unsigned long)number : (unsigned long)number;
    if (number < 0) {
        text[length++] = '-';
    }
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude > 0);
    while (count > 0) {
        text[length++] = digits[--count];
    }
    return length;
}

static size_t append_text(char *text, size_t length, const char *addition) {
    size_t addition_length = strlen(addition);
    memcpy(text + length, addition, addition_length);
    return length + addition_length;
}

int kill(pid_t pid, int signal_number) {
    if (log_file >= 0) {
        int saved_errno = errno;
        char line[LINE_SIZE];
        size_t length = append_text(line, 0, "kill(");
        length = append_number(line, length, pid);
        length = append_text(line, length, ", ");
        length = append_number(line, length, signal_number);
        length = append_text(line, length, ")\n");
        ssize_t written = write(log_file, line, length);
        (void)written;
        errno = saved_errno;
    }
    return real_kill(pid, signal_number);
}
