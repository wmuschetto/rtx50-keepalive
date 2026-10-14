/*
 * Usage: hold-start
 *
 * Checks the time a hold report gives, the time its first late job was
 * submitted: record_finished, given a first late job that took AGO_SECONDS,
 * must date the hold at the UTC time that long before now as HH:MM:SS.mmm,
 * within 1 s. Writes what it got to stderr and exits 1 if not. tests/check.sh
 * builds it from the keepalive's source and runs it.
 */
#define main keepalive_main
#include "../rtx50-keepalive.c"
#undef main

#define MILLISECONDS_PER_DAY 86400000LL
/* Far longer than the call takes, so a hold dated when it was seen, or a
 * wrong sign or unit, shows. */
#define AGO_SECONDS 5400
/* record_finished reads the clock just after this program does. */
#define TOLERANCE_MS 1000

int main(void) {
    setenv("TZ", "UTC", 1);
    tzset();
    struct timespec before;
    clock_gettime(CLOCK_REALTIME, &before);
    Hold hold = {0};
    Totals totals = {0};
    record_finished(AGO_SECONDS * NANOSECONDS_PER_SECOND, &hold, &totals);
    const char *text = hold.started_at;
    int hours, minutes, seconds, milliseconds;
    if (sscanf(text, "%2d:%2d:%2d.%3d", &hours, &minutes, &seconds, &milliseconds) != 4) {
        fprintf(stderr, "hold-start: got [%s], not HH:MM:SS.mmm\n", text);
        return 1;
    }
    int64_t printed_ms = ((hours * 60LL + minutes) * 60 + seconds) * 1000 + milliseconds;
    int64_t wanted_ns = (int64_t)before.tv_sec * NANOSECONDS_PER_SECOND + before.tv_nsec -
                        AGO_SECONDS * NANOSECONDS_PER_SECOND;
    int64_t wanted_ms = wanted_ns / NANOSECONDS_PER_MILLISECOND % MILLISECONDS_PER_DAY;
    int64_t late_ms = (printed_ms - wanted_ms + MILLISECONDS_PER_DAY) % MILLISECONDS_PER_DAY;
    if (late_ms > TOLERANCE_MS) {
        fprintf(stderr, "hold-start: got [%s], %lld ms from the time %d s ago\n", text, (long long)late_ms,
                AGO_SECONDS);
        return 1;
    }
    return 0;
}
