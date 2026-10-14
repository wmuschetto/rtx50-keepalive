/*
 * Usage: empty-argv
 *
 * Calls rtx50-keepalive's main as a kernel before 5.18 starts a program run
 * with an empty argv: argc 0, argv[0] NULL and the environment right after
 * it. tests/check.sh builds it from the keepalive's source and runs it.
 */
#define main keepalive_main
#include "../rtx50-keepalive.c"
#undef main

int main(void) {
    char *process_start[] = {NULL, "sh", "-c", "echo the environment ran", NULL};
    return keepalive_main(0, process_start);
}
