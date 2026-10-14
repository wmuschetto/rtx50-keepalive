/*
 * Usage: LD_PRELOAD=failed-create.so COMMAND [ARGUMENT...]
 *
 * Stands in for the Vulkan loader's vkCreateInstance, which fails here after
 * writing a handle it never created: the Vulkan spec leaves a failed
 * command's outputs undefined. vkDestroyInstance ends the process with
 * status 66 when it gets that handle, where a driver might crash it.
 * tests/check.sh builds it as a shared object and preloads it into the
 * keepalive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

/* Exiting, not aborting, leaves no core dump behind. */
#define EXIT_DESTROYED_NEVER_CREATED 66

static int never_created;
#define NEVER_CREATED ((VkInstance)(void *)&never_created)

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)info;
    (void)allocator;
    *instance = NEVER_CREATED;
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    if (instance == NEVER_CREATED) {
        fputs("failed-create: vkDestroyInstance got the output of a failed vkCreateInstance\n", stderr);
        _Exit(EXIT_DESTROYED_NEVER_CREATED);
    }
}
