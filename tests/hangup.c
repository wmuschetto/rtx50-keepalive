/*
 * Usage: hangup [--interrupt] COMMAND [ARGUMENT...]
 *
 * Runs COMMAND as the leader of a new session with a new pseudo-terminal as its
 * controlling terminal. Once a process in it writes a line to descriptor 3,
 * or every process has closed that descriptor, or GIVE_UP_AFTER_MS later, it
 * hangs the terminal up, as closing a terminal window does, or with
 * --interrupt types Ctrl-C on it. It exits with COMMAND's exit status once
 * COMMAND and every process COMMAND left behind have exited, or kills them
 * and exits 124 if any still runs GIVE_UP_AFTER_MS after the hangup or
 * Ctrl-C. tests/check.sh builds and runs it.
 */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A process in COMMAND's session writes to it once the signal is safe to send:
 * the command's trap is set, and the keepalive blocked its stop signals before
 * it forked, so they wait for its handlers. The signal never comes too early,
 * however slow the start. */
#define READY_DESCRIPTOR 3
#define GIVE_UP_AFTER_MS 5000
#define POLL_MS 50
/* The interrupt character of a new terminal (VINTR). */
#define CONTROL_C "\003"
#define EXIT_USAGE 2
#define EXIT_TIMED_OUT 124
#define EXIT_SIGNALED_BASE 128

static void sleep_ms(long milliseconds) {
    struct timespec delay = {.tv_sec = milliseconds / 1000, .tv_nsec = milliseconds % 1000 * 1000000L};
    nanosleep(&delay, NULL);
}

/* Kills every child until none is left: as a subreaper this process inherits
 * the children of each one killed, whatever process group or session they
 * moved to. The children file needs CONFIG_PROC_CHILDREN. */
static void kill_every_child(void) {
    for (;;) {
        FILE *children = fopen("/proc/thread-self/children", "r");
        if (children == NULL) {
            perror("hangup: cannot list the processes left");
            return;
        }
        long child;
        while (fscanf(children, "%ld", &child) == 1) {
            kill((pid_t)child, SIGKILL);
        }
        fclose(children);
        pid_t reaped;
        while ((reaped = waitpid(-1, NULL, WNOHANG)) > 0) {
        }
        if (reaped < 0) {
            return;
        }
        sleep_ms(POLL_MS);
    }
}

int main(int argc, char **argv) {
    int interrupt = argc > 1 && strcmp(argv[1], "--interrupt") == 0;
    if (argc < 2 + interrupt) {
        fprintf(stderr, "Usage: hangup [--interrupt] COMMAND [ARGUMENT...]\n");
        return EXIT_USAGE;
    }
    char **command = argv + 1 + interrupt;
    /* An ignored SIGCHLD inherited from the launcher lets the kernel reap
     * COMMAND, and waitpid then never returns its status. */
    signal(SIGCHLD, SIG_DFL);
    /* A process whose parent exits is handed to this one, so it can wait for
     * what COMMAND left behind: a shell that leads the session dies of the
     * hangup before the processes it started. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        perror("hangup: cannot wait for the processes COMMAND leaves behind");
        return EXIT_USAGE;
    }
    int terminal = posix_openpt(O_RDWR | O_NOCTTY);
    if (terminal < 0 || grantpt(terminal) != 0 || unlockpt(terminal) != 0) {
        perror("hangup: cannot open a pseudo-terminal");
        return EXIT_USAGE;
    }
    const char *terminal_name = ptsname(terminal);
    int ready[2];
    if (pipe(ready) != 0) {
        perror("hangup: cannot make the ready pipe");
        return EXIT_USAGE;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("hangup: fork failed");
        return EXIT_USAGE;
    }
    if (pid == 0) {
        close(terminal);
        /* COMMAND gets the pipe's write end as READY_DESCRIPTOR and no other end. */
        close(ready[0]);
        if (dup2(ready[1], READY_DESCRIPTOR) < 0) {
            perror("hangup: cannot give the command the ready descriptor");
            _exit(EXIT_USAGE);
        }
        if (ready[1] != READY_DESCRIPTOR) {
            close(ready[1]);
        }
        /* A session leader that opens a terminal without O_NOCTTY makes it its
         * controlling terminal, which stays after the descriptor closes. */
        if (setsid() < 0 || terminal_name == NULL || open(terminal_name, O_RDWR | O_CLOEXEC) < 0) {
            perror("hangup: cannot give the command a controlling terminal");
            _exit(EXIT_USAGE);
        }
        execvp(command[0], command);
        perror("hangup: cannot run the command");
        _exit(127);
    }
    close(ready[1]);
    struct pollfd ready_poll = {.fd = ready[0], .events = POLLIN};
    poll(&ready_poll, 1, GIVE_UP_AFTER_MS);
    if (interrupt) {
        /* The terminal sends SIGINT to its whole foreground process group. */
        if (write(terminal, CONTROL_C, 1) != 1) {
            perror("hangup: cannot type Ctrl-C");
        }
    } else {
        /* Closing the master side hangs the terminal up: the kernel sends
         * SIGHUP to the session leader alone. */
        close(terminal);
    }
    int command_status = 0;
    for (int waited_ms = 0; waited_ms < GIVE_UP_AFTER_MS; waited_ms += POLL_MS) {
        int status;
        pid_t exited;
        while ((exited = waitpid(-1, &status, WNOHANG)) > 0) {
            if (exited == pid) {
                command_status = status;
            }
        }
        if (exited < 0 && errno == ECHILD) {
            return WIFEXITED(command_status) ? WEXITSTATUS(command_status)
                                             : EXIT_SIGNALED_BASE + WTERMSIG(command_status);
        }
        sleep_ms(POLL_MS);
    }
    /* The session's process group: COMMAND and the processes it left in it. */
    kill(-pid, SIGKILL);
    kill_every_child();
    return EXIT_TIMED_OUT;
}
