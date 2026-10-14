/*
 * Usage: keepalive-loop
 *
 * Runs rtx50-keepalive's job loop, with stand-ins for the Vulkan calls it
 * makes and for its clock, through a hold longer than SLOT_COUNT intervals
 * and on until its ring of slots has wrapped three times, and writes the
 * report it makes to stderr. The jobs from HOLD_FIRST_JOB on stay unfinished
 * until every slot holds one and the keepalive has waited on the oldest
 * RELEASE_ON_WAIT times; that wait ends RELEASE_AFTER_MS later with all of
 * them finished. It exits 66 when the loop resets, submits or waits on a
 * fence out of turn or sends a burst of jobs, and 67 when it stops
 * submitting. No GPU is involved, and no time passes: the stand-in clock
 * moves only when the keepalive sleeps or waits. tests/check.sh builds it
 * from the keepalive's source.
 */
#define main keepalive_main
#include "../rtx50-keepalive.c"
#undef main

#define HOLD_FIRST_JOB 3
#define RELEASE_ON_WAIT 3
#define RELEASE_AFTER_MS 10
/* The job that ends a hold and the next go at once, as the schedule restarts. */
#define MOST_JOBS_AT_ONCE 2
#define JOBS_TO_RUN (3 * SLOT_COUNT)
/* The loop checks once per job, and once per wait while every slot is busy. */
#define MOST_STOP_CHECKS (2 * JOBS_TO_RUN)
#define EXIT_FENCE_OUT_OF_TURN 66
#define EXIT_NO_PROGRESS 67

typedef enum { UNSUBMITTED, HELD, FINISHED } FenceState;
static FenceState fence_states[SLOT_COUNT];
static int held_count;
static size_t oldest_held_slot;
static int waits_with_every_slot_held;
static int released;
static int64_t last_submit_ns = -1;
static int jobs_at_once;
/* Not 0, so a hold's wall-clock start is a valid time. */
static int64_t clock_ns = 1000 * NANOSECONDS_PER_SECOND;

static void out_of_turn(const char *what) {
    fprintf(stderr, "keepalive-loop: %s\n", what);
    _Exit(EXIT_FENCE_OUT_OF_TURN);
}

/* Each slot's fence is its index plus 1, cast through uintptr_t:
 * non-dispatchable handles are integers on 32-bit builds. */
static size_t slot_of(VkFence fence) {
    uintptr_t value = (uintptr_t)fence;
    if (value == 0 || value > SLOT_COUNT) {
        out_of_turn("a call got a fence that is no slot's");
    }
    return value - 1;
}

int clock_gettime(clockid_t clock_id, struct timespec *now) {
    (void)clock_id;
    now->tv_sec = (time_t)(clock_ns / NANOSECONDS_PER_SECOND);
    now->tv_nsec = (long)(clock_ns % NANOSECONDS_PER_SECOND);
    return 0;
}

int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *wake, struct timespec *remaining) {
    (void)clock_id;
    (void)flags;
    (void)remaining;
    int64_t wake_ns = (int64_t)wake->tv_sec * NANOSECONDS_PER_SECOND + wake->tv_nsec;
    if (wake_ns > clock_ns) {
        clock_ns = wake_ns;
    }
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence *fences) {
    (void)device;
    for (uint32_t index = 0; index < count; index++) {
        size_t slot = slot_of(fences[index]);
        if (fence_states[slot] == HELD) {
            out_of_turn("vkResetFences got the fence of a job in flight");
        }
        fence_states[slot] = UNSUBMITTED;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                             VkFence fence) {
    (void)queue;
    (void)count;
    (void)submits;
    size_t slot = slot_of(fence);
    if (fence_states[slot] != UNSUBMITTED) {
        out_of_turn("vkQueueSubmit got a fence that was not reset");
    }
    jobs_at_once = clock_ns == last_submit_ns ? jobs_at_once + 1 : 1;
    last_submit_ns = clock_ns;
    if (jobs_at_once > MOST_JOBS_AT_ONCE) {
        out_of_turn("vkQueueSubmit got a burst of jobs: the schedule did not restart after the hold");
    }
    if (!released && keepalive.totals.submitted >= HOLD_FIRST_JOB) {
        if (held_count == 0) {
            oldest_held_slot = slot;
        }
        fence_states[slot] = HELD;
        held_count++;
    } else {
        fence_states[slot] = FINISHED;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences,
                                               VkBool32 wait_all, uint64_t timeout_ns) {
    (void)device;
    (void)count;
    (void)wait_all;
    size_t slot = slot_of(fences[0]);
    if (fence_states[slot] == UNSUBMITTED) {
        out_of_turn("vkWaitForFences got the fence of a job never submitted");
    }
    if (fence_states[slot] == FINISHED) {
        return VK_SUCCESS;
    }
    /* Only a wait on the oldest job, once every slot is busy, ends the hold. */
    if (held_count == SLOT_COUNT && slot == oldest_held_slot && timeout_ns > 0 &&
        ++waits_with_every_slot_held == RELEASE_ON_WAIT) {
        clock_ns += RELEASE_AFTER_MS * NANOSECONDS_PER_MILLISECOND;
        for (size_t index = 0; index < SLOT_COUNT; index++) {
            fence_states[index] = FINISHED;
        }
        released = 1;
        held_count = 0;
        return VK_SUCCESS;
    }
    clock_ns += (int64_t)timeout_ns;
    return VK_TIMEOUT;
}

static int ran_enough_jobs(void) {
    static int checks;
    if (++checks > MOST_STOP_CHECKS) {
        fprintf(stderr, "keepalive-loop: %llu jobs after %d checks\n",
                (unsigned long long)keepalive.totals.submitted, checks);
        _Exit(EXIT_NO_PROGRESS);
    }
    return keepalive.totals.submitted >= JOBS_TO_RUN;
}

int main(void) {
    for (size_t index = 0; index < SLOT_COUNT; index++) {
        keepalive.slots[index].fence = (VkFence)(uintptr_t)(index + 1);
    }
    if (keepalive_run(&keepalive, ran_enough_jobs) != 0) {
        return EXIT_FAILURE;
    }
    keepalive_report(&keepalive);
    return 0;
}
