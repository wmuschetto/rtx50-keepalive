/*
 * rtx50-keepalive: a workaround for multi-second game freezes on NVIDIA RTX 50
 * series GPUs under Linux, NVIDIA/open-gpu-kernel-modules#880.
 *
 * Usage: rtx50-keepalive COMMAND [ARGUMENT...]
 *        rtx50-keepalive
 * As a Steam launch option: rtx50-keepalive %command%
 * Build: make (see the Makefile)
 *
 * In #880 the driver holds back a game's "work done" events for seconds, and
 * the game stops until they arrive. Users there report that other GPU activity,
 * such as a video on another monitor, ends a hold early. This program supplies
 * that activity without drawing anything: from its own Vulkan device it gives
 * the GPU a tiny job (a 4-byte buffer fill) every SUBMIT_INTERVAL_MS and waits
 * on each job's fence.
 *
 * With a command it runs the command as a child, with the environment it was
 * given, runs the keepalive beside it, and exits with the command's status
 * once the command exits. It never touches the command's process: no
 * injection, no Vulkan layer, no tracing. If the keepalive cannot start, the
 * command runs anyway. Without a command it runs until SIGINT, SIGTERM, SIGHUP
 * or SIGQUIT; one its launcher ignores, as nohup ignores SIGHUP, stays ignored.
 *
 * It writes one line to stderr per hold: a run of its own jobs whose fences
 * took longer than LATE_FENCE_MS, which means the driver held this program's
 * events too.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define PROGRAM "rtx50-keepalive"
#define VERSION "0.1.0"

#define NVIDIA_VENDOR_ID 0x10DEu
/* Keeps the jobs less than one frame apart at refresh rates up to 200 Hz. */
#define SUBMIT_INTERVAL_MS 5
/* A 4-byte fill completes in well under 1 ms when nothing is held.
 * Overridable (-DLATE_FENCE_MS=0) to test the hold report. */
#ifndef LATE_FENCE_MS
#define LATE_FENCE_MS 20
#endif
/* How long one wait blocks when every slot is in flight. */
#define FENCE_WAIT_MS 50
/* Jobs that may be in flight at once: 2048 slots at 5 ms keep the jobs coming
 * through a 10 s hold; the holds reported in #880 last 1-10 s. */
#define SLOT_COUNT 2048
#define FILL_BYTES 4
/* The NVIDIA driver opens one /dev/nvidia0 descriptor per fence (595.99.02,
 * measured 2026-10-13: 2048 fences, 2084 open files), past the 1024 soft limit
 * systemd and many shells set. 64 covers the device, instance and stdio. */
#define OPEN_FILES_NEEDED (SLOT_COUNT + 64)
#define MAX_PHYSICAL_DEVICES 16
#define MAX_QUEUE_FAMILIES 32

#define NANOSECONDS_PER_MILLISECOND 1000000LL
#define NANOSECONDS_PER_SECOND 1000000000LL

/* Exit statuses for a command that could not be started, as POSIX shells use them. */
#define EXIT_NOT_FOUND 127
#define EXIT_NOT_EXECUTABLE 126
#define EXIT_USAGE 2
#define EXIT_SIGNALED_BASE 128

typedef struct {
    VkCommandBuffer commands;
    VkFence fence;
    int64_t submitted_ns;
} Slot;

/* A hold: consecutive jobs that finished late. */
typedef struct {
    int open;
    char started_at[64];
    uint64_t late_jobs;
    int64_t longest_ns;
} Hold;

typedef struct {
    uint64_t submitted;
    uint64_t late;
    uint64_t holds;
    int64_t longest_ns;
} Totals;

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool pool;
    Slot slots[SLOT_COUNT];
    int fence_count;
    /* One queue runs jobs in order, so slots retire in submission order. */
    int oldest;
    int in_flight;
    int64_t started_ns;
    Hold hold;
    Totals totals;
} Keepalive;

static Keepalive keepalive;

/* Set in the parent before the signal handlers are installed; 0 without a command. */
static volatile sig_atomic_t command_pid = 0;
/* Set with command_pid: whether this process leads its session. */
static volatile sig_atomic_t leads_session = 0;
static volatile sig_atomic_t stop_requested = 0;
/* The command's exit as waitid reports it; si_pid is 0 until it has exited. */
static siginfo_t command_exit;
/* Set when waitid fails and the command's status cannot be known. */
static int command_status_lost;

static void on_stop_signal(int signal_number, siginfo_t *info, void *context) {
    (void)context;
    if (command_pid > 0) {
        /* Pass on a signal a process sent to this one. A signal from the
         * terminal (si_code SI_KERNEL) went to the whole process group, the
         * command included, so passing it on would deliver it twice; the
         * SIGHUP of a hangup is the exception when this process leads its
         * session, since that goes to the leader alone. */
        if (info->si_code <= 0 || (signal_number == SIGHUP && leads_session)) {
            int saved_errno = errno;
            kill((pid_t)command_pid, signal_number);
            errno = saved_errno;
        }
        return;
    }
    stop_requested = 1;
}

/* Only interrupts the keepalive's sleep, so a command's exit is seen at once. */
static void on_child_signal(int signal_number) {
    (void)signal_number;
}

static const int STOP_SIGNALS[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT};
#define STOP_SIGNAL_COUNT (sizeof STOP_SIGNALS / sizeof STOP_SIGNALS[0])

static void install_signal_handlers(void) {
    struct sigaction on_stop = {.sa_sigaction = on_stop_signal, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&on_stop.sa_mask);
    for (size_t index = 0; index < STOP_SIGNAL_COUNT; index++) {
        struct sigaction inherited;
        /* Without a command, a signal the launcher ignores stays ignored, as
         * nohup and a shell's background jobs rely on. */
        if (command_pid == 0 && sigaction(STOP_SIGNALS[index], NULL, &inherited) == 0 &&
            inherited.sa_handler == SIG_IGN) {
            continue;
        }
        sigaction(STOP_SIGNALS[index], &on_stop, NULL);
    }
    /* SA_RESTART keeps the driver's own system calls going; clock_nanosleep
     * returns EINTR regardless, which is what wakes the keepalive. */
    struct sigaction on_child = {.sa_handler = on_child_signal, .sa_flags = SA_NOCLDSTOP | SA_RESTART};
    sigemptyset(&on_child.sa_mask);
    sigaction(SIGCHLD, &on_child, NULL);
}

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: " PROGRAM " COMMAND [ARGUMENT...]\n"
            "       " PROGRAM "\n"
            "Runs COMMAND with a tiny GPU job every %d ms beside it, a workaround for\n"
            "multi-second game freezes on NVIDIA RTX 50 GPUs\n"
            "(https://github.com/NVIDIA/open-gpu-kernel-modules/issues/880).\n"
            "Without a command it runs until Ctrl-C. As a Steam launch option:\n"
            "  " PROGRAM " %%command%%\n",
            SUBMIT_INTERVAL_MS);
}

/* Prints the failed step and returns 1 when result is not VK_SUCCESS. */
static int failed(VkResult result, const char *step) {
    if (result == VK_SUCCESS) {
        return 0;
    }
    fprintf(stderr, PROGRAM ": %s failed (VkResult %d); the keepalive is off\n", step, (int)result);
    return 1;
}

static int64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec;
}

/* Local wall-clock time ago_ns before now, as HH:MM:SS.mmm. */
static void format_wall_time(char *text, size_t size, int64_t ago_ns) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t then_ns = (int64_t)now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec - ago_ns;
    time_t then_seconds = (time_t)(then_ns / NANOSECONDS_PER_SECOND);
    struct tm local;
    localtime_r(&then_seconds, &local);
    snprintf(text, size, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
             (int)(then_ns % NANOSECONDS_PER_SECOND / NANOSECONDS_PER_MILLISECOND));
}

/* Raises the soft open-file limit to the hard one; this program never calls
 * select(), which is what the low soft limit protects. */
static int raise_open_file_limit(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        fprintf(stderr, PROGRAM ": getrlimit failed (%s); the keepalive is off\n", strerror(errno));
        return -1;
    }
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < OPEN_FILES_NEEDED) {
        fprintf(stderr, PROGRAM ": needs %d open files but the hard limit is %llu; the keepalive "
                        "is off (raise the limit, e.g. LimitNOFILE= in a systemd unit)\n",
                OPEN_FILES_NEEDED, (unsigned long long)limit.rlim_max);
        return -1;
    }
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
        fprintf(stderr, PROGRAM ": setrlimit failed (%s); the keepalive is off\n", strerror(errno));
        return -1;
    }
    return 0;
}

static VkPhysicalDevice find_nvidia_gpu(VkInstance instance, VkPhysicalDeviceProperties *properties) {
    VkPhysicalDevice devices[MAX_PHYSICAL_DEVICES];
    uint32_t count = MAX_PHYSICAL_DEVICES;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_INCOMPLETE && failed(result, "vkEnumeratePhysicalDevices")) {
        return VK_NULL_HANDLE;
    }
    for (uint32_t index = 0; index < count; index++) {
        vkGetPhysicalDeviceProperties(devices[index], properties);
        if (properties->vendorID == NVIDIA_VENDOR_ID &&
            properties->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return devices[index];
        }
    }
    fprintf(stderr, PROGRAM ": no discrete NVIDIA GPU among %u Vulkan devices (is the NVIDIA Vulkan "
                    "driver installed?); the keepalive is off\n", count);
    return VK_NULL_HANDLE;
}

/* The graphics family: the one a desktop compositor's drawing, the activity
 * reported to end a hold, runs on. */
static int find_graphics_queue_family(VkPhysicalDevice gpu, uint32_t *family) {
    VkQueueFamilyProperties families[MAX_QUEUE_FAMILIES];
    uint32_t count = MAX_QUEUE_FAMILIES;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    for (uint32_t index = 0; index < count; index++) {
        if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *family = index;
            return 0;
        }
    }
    fprintf(stderr, PROGRAM ": the NVIDIA GPU reports no graphics queue; the keepalive is off\n");
    return -1;
}

static int find_memory_type(VkPhysicalDevice gpu, uint32_t allowed_types, uint32_t *type) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    for (uint32_t index = 0; index < memory.memoryTypeCount; index++) {
        if ((allowed_types & (1u << index)) &&
            (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            *type = index;
            return 0;
        }
    }
    fprintf(stderr, PROGRAM ": no device-local memory fits a %d-byte buffer; the keepalive is off\n",
            FILL_BYTES);
    return -1;
}

/* Destroys whatever keepalive_start created; safe after a partial start,
 * since a handle is stored, and a fence counted, only once its create
 * succeeded (a failed create leaves its output undefined). Used when the
 * keepalive fails while its command runs on. */
static void keepalive_destroy(Keepalive *k) {
    if (k->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(k->device); /* best effort: the objects go either way */
        for (int index = 0; index < k->fence_count; index++) {
            vkDestroyFence(k->device, k->slots[index].fence, NULL);
        }
        vkDestroyCommandPool(k->device, k->pool, NULL);
        vkDestroyBuffer(k->device, k->buffer, NULL);
        vkFreeMemory(k->device, k->memory, NULL);
        vkDestroyDevice(k->device, NULL);
    }
    vkDestroyInstance(k->instance, NULL); /* a null instance is allowed */
}

static int create_device(Keepalive *k, VkPhysicalDeviceProperties *properties, VkPhysicalDevice *gpu,
                         uint32_t *queue_family) {
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = PROGRAM,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance;
    if (failed(vkCreateInstance(&instance_info, NULL, &instance), "vkCreateInstance")) {
        return -1;
    }
    k->instance = instance;
    *gpu = find_nvidia_gpu(k->instance, properties);
    if (*gpu == VK_NULL_HANDLE || find_graphics_queue_family(*gpu, queue_family) != 0) {
        return -1;
    }
    float priority = 1.0f; /* the only queue on this device, so the value has no effect */
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    if (failed(vkCreateDevice(*gpu, &device_info, NULL, &device), "vkCreateDevice")) {
        return -1;
    }
    k->device = device;
    vkGetDeviceQueue(k->device, *queue_family, 0, &k->queue);
    return 0;
}

static int create_buffer(Keepalive *k, VkPhysicalDevice gpu) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = FILL_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    if (failed(vkCreateBuffer(k->device, &buffer_info, NULL, &buffer), "vkCreateBuffer")) {
        return -1;
    }
    k->buffer = buffer;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(k->device, k->buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
    };
    VkDeviceMemory memory;
    if (find_memory_type(gpu, requirements.memoryTypeBits, &allocation.memoryTypeIndex) != 0 ||
        failed(vkAllocateMemory(k->device, &allocation, NULL, &memory), "vkAllocateMemory")) {
        return -1;
    }
    k->memory = memory;
    return failed(vkBindBufferMemory(k->device, k->buffer, k->memory, 0), "vkBindBufferMemory") ? -1 : 0;
}

static int record_jobs(Keepalive *k, uint32_t queue_family) {
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool pool;
    if (failed(vkCreateCommandPool(k->device, &pool_info, NULL, &pool), "vkCreateCommandPool")) {
        return -1;
    }
    k->pool = pool;
    static VkCommandBuffer buffers[SLOT_COUNT];
    VkCommandBufferAllocateInfo buffers_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = k->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = SLOT_COUNT,
    };
    if (failed(vkAllocateCommandBuffers(k->device, &buffers_info, buffers), "vkAllocateCommandBuffers")) {
        return -1;
    }
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (int index = 0; index < SLOT_COUNT; index++) {
        k->slots[index].commands = buffers[index];
        if (failed(vkCreateFence(k->device, &fence_info, NULL, &k->slots[index].fence), "vkCreateFence")) {
            return -1;
        }
        k->fence_count++;
        /* Recorded once and resubmitted: each slot is reused only after its fence signals. */
        if (failed(vkBeginCommandBuffer(buffers[index], &begin), "vkBeginCommandBuffer")) {
            return -1;
        }
        vkCmdFillBuffer(buffers[index], k->buffer, 0, FILL_BYTES, (uint32_t)index);
        if (failed(vkEndCommandBuffer(buffers[index]), "vkEndCommandBuffer")) {
            return -1;
        }
    }
    return 0;
}

static int keepalive_start(Keepalive *k) {
    /* Implicit layers (Steam's overlay, MangoHud and the like) follow the
     * environment a launcher sets for the game; this device needs none.
     * Loaders before 1.3.234 ignore the variable; VK_INSTANCE_LAYERS and
     * VK_LOADER_LAYERS_ENABLE override it, and so does VK_LOADER_LAYERS_ALLOW
     * on loaders from 1.3.262. */
    if (setenv("VK_LOADER_LAYERS_DISABLE", "~all~", 1) != 0) {
        fprintf(stderr, PROGRAM ": setenv failed (%s); the keepalive is off\n", strerror(errno));
        return -1;
    }
    if (raise_open_file_limit() != 0) {
        return -1;
    }
    VkPhysicalDeviceProperties properties;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    if (create_device(k, &properties, &gpu, &queue_family) != 0 || create_buffer(k, gpu) != 0 ||
        record_jobs(k, queue_family) != 0) {
        keepalive_destroy(k);
        return -1;
    }
    fprintf(stderr, PROGRAM ": %s, a %d-byte fill every %d ms; reporting fences over %d ms\n",
            properties.deviceName, FILL_BYTES, SUBMIT_INTERVAL_MS, LATE_FENCE_MS);
    return 0;
}

static void close_hold(Hold *hold, Totals *totals) {
    if (!hold->open) {
        return;
    }
    totals->holds++;
    fprintf(stderr, PROGRAM ": %s hold: %llu jobs late, longest fence %.1f ms\n", hold->started_at,
            (unsigned long long)hold->late_jobs, (double)hold->longest_ns / NANOSECONDS_PER_MILLISECOND);
    hold->open = 0;
}

static void record_finished(int64_t elapsed_ns, Hold *hold, Totals *totals) {
    if (elapsed_ns > totals->longest_ns) {
        totals->longest_ns = elapsed_ns;
    }
    if (elapsed_ns <= LATE_FENCE_MS * NANOSECONDS_PER_MILLISECOND) {
        close_hold(hold, totals);
        return;
    }
    totals->late++;
    if (!hold->open) {
        *hold = (Hold){.open = 1};
        /* The first late job was submitted when the hold began. */
        format_wall_time(hold->started_at, sizeof hold->started_at, elapsed_ns);
    }
    hold->late_jobs++;
    if (elapsed_ns > hold->longest_ns) {
        hold->longest_ns = elapsed_ns;
    }
}

/* Waits up to timeout_ns for a job: 1 if it finished, 0 if not yet, -1 on failure. */
static int wait_for_job(const Keepalive *k, const Slot *slot, int64_t timeout_ns) {
    VkResult result = vkWaitForFences(k->device, 1, &slot->fence, VK_TRUE, (uint64_t)timeout_ns);
    if (result == VK_TIMEOUT) {
        return 0;
    }
    return failed(result, "vkWaitForFences") ? -1 : 1;
}

/* Retires every finished job, oldest first: 0, or -1 on failure. */
static int retire_finished(Keepalive *k) {
    while (k->in_flight > 0) {
        int finished = wait_for_job(k, &k->slots[k->oldest], 0);
        if (finished <= 0) {
            return finished;
        }
        record_finished(monotonic_ns() - k->slots[k->oldest].submitted_ns, &k->hold, &k->totals);
        k->oldest = (k->oldest + 1) % SLOT_COUNT;
        k->in_flight--;
    }
    return 0;
}

static int submit_job(Keepalive *k) {
    Slot *slot = &k->slots[(k->oldest + k->in_flight) % SLOT_COUNT];
    if (failed(vkResetFences(k->device, 1, &slot->fence), "vkResetFences")) {
        return -1;
    }
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->commands,
    };
    slot->submitted_ns = monotonic_ns();
    if (failed(vkQueueSubmit(k->queue, 1, &submit, slot->fence), "vkQueueSubmit")) {
        return -1;
    }
    k->in_flight++;
    k->totals.submitted++;
    /* Block on the new job so the driver arms its completion event, the path
     * the bug holds; give up after one interval and keep submitting, so a held
     * event never stops the stream of jobs. */
    return wait_for_job(k, slot, SUBMIT_INTERVAL_MS * NANOSECONDS_PER_MILLISECOND) < 0 ? -1 : 0;
}

/* Runs until should_stop() returns 1: 0, or -1 if the keepalive failed. */
static int keepalive_run(Keepalive *k, int (*should_stop)(void)) {
    k->started_ns = monotonic_ns();
    int64_t next_submit_ns = k->started_ns;
    while (!should_stop()) {
        if (k->in_flight == SLOT_COUNT) {
            /* Every slot busy means a hold longer than SLOT_COUNT intervals; wait it out. */
            int finished = wait_for_job(k, &k->slots[k->oldest], FENCE_WAIT_MS * NANOSECONDS_PER_MILLISECOND);
            if (finished < 0 || (finished == 1 && retire_finished(k) != 0)) {
                return -1;
            }
            continue;
        }
        if (submit_job(k) != 0 || retire_finished(k) != 0) {
            return -1;
        }
        next_submit_ns += SUBMIT_INTERVAL_MS * NANOSECONDS_PER_MILLISECOND;
        int64_t now_ns = monotonic_ns();
        if (next_submit_ns < now_ns) {
            next_submit_ns = now_ns; /* fell behind during a hold; restart the schedule */
        }
        struct timespec wake = {
            .tv_sec = (time_t)(next_submit_ns / NANOSECONDS_PER_SECOND),
            .tv_nsec = (long)(next_submit_ns % NANOSECONDS_PER_SECOND),
        };
        int sleep_error;
        while ((sleep_error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL)) == EINTR) {
            if (should_stop()) {
                return 0;
            }
        }
        if (sleep_error != 0) {
            fprintf(stderr, PROGRAM ": clock_nanosleep failed (%s); the keepalive is off\n",
                    strerror(sleep_error));
            return -1;
        }
    }
    return 0;
}

static void keepalive_report(Keepalive *k) {
    close_hold(&k->hold, &k->totals);
    fprintf(stderr, PROGRAM ": stopped after %.0f s; %llu jobs, %llu late in %llu holds, longest fence %.1f ms\n",
            (double)(monotonic_ns() - k->started_ns) / NANOSECONDS_PER_SECOND,
            (unsigned long long)k->totals.submitted, (unsigned long long)k->totals.late,
            (unsigned long long)k->totals.holds, (double)k->totals.longest_ns / NANOSECONDS_PER_MILLISECOND);
}

/* 1 once the command has exited (its exit in command_exit), 0 while it runs.
 * Blocks until then when wait is set. The command is left unreaped (WNOWAIT):
 * its pid stays taken until this process exits, so a stop signal passed on
 * late never reaches a process that took the pid over. */
static int command_exited(int wait) {
    for (;;) {
        command_exit.si_pid = 0;
        if (waitid(P_PID, (id_t)command_pid, &command_exit, WEXITED | WNOWAIT | (wait ? 0 : WNOHANG)) == 0) {
            return command_exit.si_pid != 0;
        }
        if (errno != EINTR) {
            fprintf(stderr, PROGRAM ": waitid failed (%s); the command's exit status is lost\n",
                    strerror(errno));
            command_status_lost = 1;
            return 1;
        }
    }
}

static int command_has_exited(void) {
    return command_exited(0);
}

static int stop_was_requested(void) {
    return stop_requested;
}

/* The command's exit status, or 128 plus the signal that ended it, as shells report it. */
static int exit_status_of_command(void) {
    if (command_status_lost) {
        return EXIT_FAILURE;
    }
    if (command_exit.si_code == CLD_EXITED) {
        return command_exit.si_status;
    }
    if (command_exit.si_code == CLD_KILLED || command_exit.si_code == CLD_DUMPED) {
        return EXIT_SIGNALED_BASE + command_exit.si_status;
    }
    return EXIT_FAILURE;
}

static void exec_command(char **command) {
    execvp(command[0], command);
    int exec_error = errno;
    fprintf(stderr, PROGRAM ": cannot run %s: %s\n", command[0], strerror(exec_error));
    _exit(exec_error == ENOENT ? EXIT_NOT_FOUND : EXIT_NOT_EXECUTABLE);
}

static int run_with_command(char **command) {
    /* Hold the signals until the handlers know the command's pid. */
    sigset_t held, previous;
    sigemptyset(&held);
    for (size_t index = 0; index < STOP_SIGNAL_COUNT; index++) {
        sigaddset(&held, STOP_SIGNALS[index]);
    }
    sigaddset(&held, SIGCHLD);
    sigprocmask(SIG_BLOCK, &held, &previous);
    /* An inherited SIG_IGN lets the kernel reap a command that exits before
     * install_signal_handlers, and its exit status is lost. The command gets
     * the inherited action back. */
    struct sigaction inherited_on_child;
    struct sigaction default_on_child = {.sa_handler = SIG_DFL};
    sigaction(SIGCHLD, &default_on_child, &inherited_on_child);
    pid_t pid = fork();
    if (pid <= 0) {
        if (pid < 0) {
            fprintf(stderr, PROGRAM ": fork failed (%s); running the command without the keepalive\n",
                    strerror(errno));
        }
        sigaction(SIGCHLD, &inherited_on_child, NULL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        exec_command(command);
    }
    command_pid = (sig_atomic_t)pid;
    leads_session = getsid(0) == getpid();
    install_signal_handlers();
    sigprocmask(SIG_SETMASK, &previous, NULL);

    if (keepalive_start(&keepalive) == 0) {
        int result = keepalive_run(&keepalive, command_has_exited);
        keepalive_report(&keepalive);
        if (result == 0) {
            /* No teardown: exiting frees the device in about 0.3 s, where
             * destroying 2048 fences one by one takes 1.6 s (measured
             * 2026-10-14), and the launcher counts the game as running until
             * this process exits. */
            return exit_status_of_command();
        }
        keepalive_destroy(&keepalive);
    }
    command_exited(1);
    return exit_status_of_command();
}

static int run_alone(void) {
    install_signal_handlers();
    if (keepalive_start(&keepalive) != 0) {
        return EXIT_FAILURE;
    }
    int result = keepalive_run(&keepalive, stop_was_requested);
    keepalive_report(&keepalive);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE; /* exiting frees the device, as above */
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf(PROGRAM " " VERSION "\n");
        return EXIT_SUCCESS;
    }
    /* A kernel without the exec change of Linux 5.18 (some older stable kernels
     * have it) starts a program run with an empty argv with argc 0 and its
     * environment right after argv[0]; the others give argc 1. */
    char **command = argc > 0 ? argv + 1 : argv;
    if (argc > 1 && strcmp(argv[1], "--") == 0) {
        command++;
        if (*command == NULL) {
            print_usage(stderr);
            return EXIT_USAGE;
        }
    } else if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, PROGRAM ": unknown option %s; put -- before a command that starts with -\n",
                argv[1]);
        return EXIT_USAGE;
    }
    return *command == NULL ? run_alone() : run_with_command(command);
}
