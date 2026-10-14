/*
 * Usage: FAILED_CREATE=CREATE LD_PRELOAD=failed-create.so COMMAND [ARGUMENT...]
 *
 * Stands in for the Vulkan loader, with one discrete NVIDIA GPU, in each call
 * the keepalive makes to start, to submit its first job, and to destroy what
 * it started. The create CREATE names (vkCreateInstance, vkCreateDevice,
 * vkCreateBuffer, vkAllocateMemory, vkCreateCommandPool, or for vkCreateFence
 * the second fence's) fails after writing a handle it never created: the
 * Vulkan spec leaves a failed command's outputs undefined. A call that gets
 * that handle ends the process with status 66, where a driver might crash it.
 * With CREATE vkQueueSubmit every create succeeds and the first submit fails,
 * as a GPU lost mid-game makes it. An object destroyed after its parent, or
 * left at exit, ends the process with status 68. vkCreateInstance ends it
 * with status 69 when the keepalive has not set VK_LOADER_LAYERS_DISABLE to
 * ~all~. Any other CREATE ends it with status 2 at vkCreateInstance, so the
 * stand-in's handles never reach the real loader.
 * tests/check.sh builds it as a shared object and preloads it into the
 * keepalive.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define NVIDIA_VENDOR_ID 0x10DEu
/* Exiting, not aborting, leaves no core dump behind. */
#define EXIT_GOT_NEVER_CREATED 66
#define EXIT_NOT_DESTROYED 68
#define EXIT_LAYERS_NOT_DISABLED 69
#define EXIT_USAGE 2

static const char *const CREATES[] = {"vkCreateInstance", "vkCreateDevice",      "vkCreateBuffer",
                                      "vkAllocateMemory", "vkCreateCommandPool", "vkCreateFence",
                                      "vkQueueSubmit"};
#define CREATE_COUNT (sizeof CREATES / sizeof CREATES[0])

/* Each handle the stand-in hands out is the address of one of these, cast
 * through uintptr_t: non-dispatchable handles are integers on 32-bit builds. */
static int created;
static int never_created;
#define CREATED(type) ((type)(uintptr_t)&created)
/* Every handle is the same address, so the objects alive are a count: the
 * instance, the device, and the device's buffer, memory, pool and fences. */
static int live_objects;

/* The handle a create writes: one it never created when FAILED_CREATE names
 * the create. */
static uintptr_t handle_from(const char *create) {
    const char *failed_create = getenv("FAILED_CREATE");
    if (failed_create != NULL && strcmp(failed_create, create) == 0) {
        return (uintptr_t)&never_created;
    }
    live_objects++;
    return (uintptr_t)&created;
}

static VkResult result_of(uintptr_t handle) {
    return handle == (uintptr_t)&never_created ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}

/* Ends the process when a call gets the output of a failed create. */
static void check(uintptr_t handle, const char *call) {
    if (handle == (uintptr_t)&never_created) {
        fprintf(stderr, "failed-create: %s got the output of a failed create\n", call);
        _Exit(EXIT_GOT_NEVER_CREATED);
    }
}
#define CHECK(handle) check((uintptr_t)(handle), __func__)

/* Ends the process unless exactly wanted objects are alive. */
static void expect_live(int wanted, const char *call) {
    if (live_objects != wanted) {
        fprintf(stderr, "failed-create: objects alive at %s: %d, expected %d\n", call, live_objects, wanted);
        _Exit(EXIT_NOT_DESTROYED);
    }
}

/* Destroying VK_NULL_HANDLE is allowed and destroys nothing. */
static void destroyed(uintptr_t handle, const char *call) {
    check(handle, call);
    if (handle != 0) {
        live_objects--;
    }
}
#define DESTROYED(handle) destroyed((uintptr_t)(handle), __func__)

/* The keepalive skips its teardown only after a run that did not fail, which
 * the failed submit rules out. Runs in the command too, which creates nothing. */
__attribute__((destructor)) static void expect_all_destroyed(void) {
    expect_live(0, "exit");
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
                                                const VkAllocationCallbacks *allocator,
                                                VkInstance *instance) {
    (void)info;
    (void)allocator;
    const char *failed_create = getenv("FAILED_CREATE");
    size_t index = 0;
    while (index < CREATE_COUNT && (failed_create == NULL || strcmp(failed_create, CREATES[index]) != 0)) {
        index++;
    }
    if (index == CREATE_COUNT) {
        fprintf(stderr, "failed-create: FAILED_CREATE is [%s]; it must name a create, such as "
                        "vkCreateDevice\n",
                failed_create == NULL ? "" : failed_create);
        _Exit(EXIT_USAGE);
    }
    /* Checked here because the keepalive sets it for itself alone: it shows
     * neither in the command's environment nor in /proc/PID/environ. */
    const char *layers_disabled = getenv("VK_LOADER_LAYERS_DISABLE");
    if (layers_disabled == NULL || strcmp(layers_disabled, "~all~") != 0) {
        fprintf(stderr, "failed-create: VK_LOADER_LAYERS_DISABLE is [%s], expected [~all~]\n",
                layers_disabled == NULL ? "" : layers_disabled);
        _Exit(EXIT_LAYERS_NOT_DISABLED);
    }
    uintptr_t handle = handle_from("vkCreateInstance");
    *instance = (VkInstance)handle;
    return result_of(handle);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
                                                          VkPhysicalDevice *gpus) {
    CHECK(instance);
    gpus[0] = CREATED(VkPhysicalDevice);
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice gpu,
                                                         VkPhysicalDeviceProperties *properties) {
    CHECK(gpu);
    *properties = (VkPhysicalDeviceProperties){
        .vendorID = NVIDIA_VENDOR_ID,
        .deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        .deviceName = "failed-create stand-in",
    };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice gpu, uint32_t *count,
                                                                    VkQueueFamilyProperties *families) {
    CHECK(gpu);
    families[0] = (VkQueueFamilyProperties){.queueFlags = VK_QUEUE_GRAPHICS_BIT, .queueCount = 1};
    *count = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice gpu,
                                                               VkPhysicalDeviceMemoryProperties *memory) {
    CHECK(gpu);
    *memory = (VkPhysicalDeviceMemoryProperties){
        .memoryTypeCount = 1,
        .memoryTypes = {{.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .heapIndex = 0}},
        .memoryHeapCount = 1,
        .memoryHeaps = {{.size = 1 << 20, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}},
    };
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)info;
    (void)allocator;
    CHECK(gpu);
    uintptr_t handle = handle_from("vkCreateDevice");
    *device = (VkDevice)handle;
    return result_of(handle);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                            VkQueue *queue) {
    (void)family;
    (void)index;
    CHECK(device);
    *queue = CREATED(VkQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)info;
    (void)allocator;
    CHECK(device);
    uintptr_t handle = handle_from("vkCreateBuffer");
    *buffer = (VkBuffer)handle;
    return result_of(handle);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                         VkMemoryRequirements *requirements) {
    CHECK(device);
    CHECK(buffer);
    *requirements = (VkMemoryRequirements){.size = 4, .alignment = 4, .memoryTypeBits = 1};
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *allocator,
                                                VkDeviceMemory *memory) {
    (void)info;
    (void)allocator;
    CHECK(device);
    uintptr_t handle = handle_from("vkAllocateMemory");
    *memory = (VkDeviceMemory)handle;
    return result_of(handle);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                  VkDeviceSize offset) {
    (void)offset;
    CHECK(device);
    CHECK(buffer);
    CHECK(memory);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator,
                                                   VkCommandPool *pool) {
    (void)info;
    (void)allocator;
    CHECK(device);
    uintptr_t handle = handle_from("vkCreateCommandPool");
    *pool = (VkCommandPool)handle;
    return result_of(handle);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device,
                                                        const VkCommandBufferAllocateInfo *info,
                                                        VkCommandBuffer *buffers) {
    CHECK(device);
    CHECK(info->commandPool);
    for (uint32_t index = 0; index < info->commandBufferCount; index++) {
        buffers[index] = CREATED(VkCommandBuffer);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo *info,
                                             const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)info;
    (void)allocator;
    CHECK(device);
    /* The second fence fails, so the first is destroyed on the way out. */
    static int fences;
    uintptr_t handle = handle_from(fences++ == 0 ? "the first vkCreateFence" : "vkCreateFence");
    *fence = (VkFence)handle;
    return result_of(handle);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commands,
                                                    const VkCommandBufferBeginInfo *info) {
    (void)info;
    CHECK(commands);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                                           VkDeviceSize size, uint32_t data) {
    (void)offset;
    (void)size;
    (void)data;
    CHECK(commands);
    CHECK(buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commands) {
    CHECK(commands);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence *fences) {
    CHECK(device);
    for (uint32_t index = 0; index < count; index++) {
        CHECK(fences[index]);
    }
    return VK_SUCCESS;
}

/* The keepalive submits only once every create succeeded, which is with
 * CREATE vkQueueSubmit. */
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                             VkFence fence) {
    (void)count;
    CHECK(queue);
    CHECK(submits[0].pCommandBuffers[0]);
    CHECK(fence);
    return VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    CHECK(device);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence,
                                          const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(device);
    DESTROYED(fence);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool pool,
                                                const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(device);
    DESTROYED(pool);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                                           const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(device);
    DESTROYED(buffer);
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                        const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(device);
    DESTROYED(memory);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(device);
    /* After its buffer, memory, pool and fences: the instance and the device are left. */
    if (device != VK_NULL_HANDLE) {
        expect_live(2, __func__);
    }
    DESTROYED(device);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    CHECK(instance);
    /* After its device: the instance alone is left. */
    if (instance != VK_NULL_HANDLE) {
        expect_live(1, __func__);
    }
    DESTROYED(instance);
}
