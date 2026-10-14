/*
 * Usage: hangup COMMAND [ARGUMENT...]
 *
 * Runs COMMAND as the leader of a new session with a new pseudo-terminal as its
 * controlling terminal, hangs the terminal up HANGUP_AFTER_MS later, as closing
 * a terminal window does, and exits with COMMAND's exit status, or 124 if
 * COMMAND still runs GIVE_UP_AFTER_MS after the hangup. tests/check.sh builds
 * and runs it.
 */
#define _XOPEN_SOURCE 700

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Gives COMMAND time to set up its signal handlers, as tests/check.sh's other
 * signal test does. */
#define HANGUP_AFTER_MS 500
#define GIVE_UP_AFTER_MS 5000
#define POLL_MS 50
#define EXIT_USAGE 2
#define EXIT_TIMED_OUT 124
#define EXIT_SIGNALED_BASE 128

static void sleep_ms(long milliseconds) {
    struct timespec delay = {.tv_sec = milliseconds / 1000, .tv_nsec = milliseconds % 1000 * 1000000L};
    nanosleep(&delay, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: hangup COMMAND [ARGUMENT...]\n");
        return EXIT_USAGE;
    }
    int terminal = posix_openpt(O_RDWR | O_NOCTTY);
    if (terminal < 0 || grantpt(terminal) != 0 || unlockpt(terminal) != 0) {
        perror("hangup: cannot open a pseudo-terminal");
        return EXIT_USAGE;
    }
    const char *terminal_name = ptsname(terminal);
    pid_t pid = fork();
    if (pid < 0) {
        perror("hangup: fork failed");
        return EXIT_USAGE;
    }
    if (pid == 0) {
        close(terminal);
        /* A session leader that opens a terminal without O_NOCTTY makes it its
         * controlling terminal, which stays after the descriptor closes. */
        if (setsid() < 0 || terminal_name == NULL || open(terminal_name, O_RDWR | O_CLOEXEC) < 0) {
            perror("hangup: cannot give the command a controlling terminal");
            _exit(EXIT_USAGE);
        }
        execvp(argv[1], argv + 1);
        perror("hangup: cannot run the command");
        _exit(127);
    }
    sleep_ms(HANGUP_AFTER_MS);
    /* Closing the master side hangs the terminal up: the kernel sends SIGHUP
     * to the session leader alone. */
    close(terminal);
    for (int waited_ms = 0; waited_ms < GIVE_UP_AFTER_MS; waited_ms += POLL_MS) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_SIGNALED_BASE + WTERMSIG(status);
        }
        sleep_ms(POLL_MS);
    }
    /* The session's process group: COMMAND and the children it left in it. */
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return EXIT_TIMED_OUT;
}
