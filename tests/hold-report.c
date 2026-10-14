/*
 * Usage: hold-report
 *
 * Feeds rtx50-keepalive's hold bookkeeping a fixed run of job times, in
 * milliseconds against the default LATE_FENCE_MS of 20, and writes the report
 * it makes to stderr. No GPU is involved. tests/check.sh builds it from the
 * keepalive's source and compares the report with the three holds the run makes.
 */
#define main keepalive_main
#include "../rtx50-keepalive.c"
#undef main

/* On time, then a hold of two late jobs, ended by a job of exactly 20 ms (on
 * time). A hold of one job, whose longest fence is shorter than the hold
 * before it, ended by an on-time job. A hold of two jobs the report ends. */
static const int64_t JOB_MS[] = {1, 30, 45, 20, 21, 1, 25, 25};

int main(void) {
    keepalive.started_ns = monotonic_ns();
    for (size_t index = 0; index < sizeof JOB_MS / sizeof JOB_MS[0]; index++) {
        keepalive.totals.submitted++;
        record_finished(JOB_MS[index] * NANOSECONDS_PER_MILLISECOND, &keepalive.hold, &keepalive.totals);
    }
    keepalive_report(&keepalive);
    return 0;
}
