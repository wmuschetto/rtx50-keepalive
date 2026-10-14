/*
 * Usage: DEVICES=VENDOR:TYPE[,VENDOR:TYPE...] LD_PRELOAD=device-list.so COMMAND [ARGUMENT...]
 *
 * Stands in for the Vulkan loader with the devices DEVICES lists, in order:
 * VENDOR is a PCI vendor ID in hexadecimal and TYPE is discrete or
 * integrated, e.g. DEVICES=8086:integrated,10de:discrete. vkCreateDevice
 * writes "device-list: vkCreateDevice got device N" (N counts from 0) to
 * stderr and fails, so the keepalive stops before any call the stand-in
 * lacks. A call that gets a device the stand-in never listed ends the process
 * with status 66. tests/check.sh builds it as a shared object and preloads it
 * into the keepalive.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MAX_DEVICES 8
#define DEVICES_SIZE 256
/* Exiting, not aborting, leaves no core dump behind. */
#define EXIT_GOT_UNLISTED 66

static int instance_object;
/* Each device's handle is the address of its entry. */
static int device_objects[MAX_DEVICES];
static VkPhysicalDeviceProperties devices[MAX_DEVICES];
static uint32_t device_count;

/* The listed device gpu is, or the end of the process when it is none. */
static uint32_t index_of(VkPhysicalDevice gpu, const char *call) {
    for (uint32_t index = 0; index < device_count; index++) {
        if ((uintptr_t)gpu == (uintptr_t)&device_objects[index]) {
            return index;
        }
    }
    fprintf(stderr, "device-list: %s got a device it never listed\n", call);
    _Exit(EXIT_GOT_UNLISTED);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)info;
    (void)allocator;
    const char *listed = getenv("DEVICES");
    char text[DEVICES_SIZE];
    snprintf(text, sizeof text, "%s", listed == NULL ? "" : listed);
    for (char *entry = strtok(text, ","); entry != NULL && device_count < MAX_DEVICES;
         entry = strtok(NULL, ",")) {
        const char *type = strchr(entry, ':');
        int discrete = type != NULL && strcmp(type + 1, "discrete") == 0;
        devices[device_count] = (VkPhysicalDeviceProperties){
            .vendorID = (uint32_t)strtoul(entry, NULL, 16),
            .deviceType = discrete ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        };
        snprintf(devices[device_count].deviceName, sizeof devices[device_count].deviceName, "device %u",
                 device_count);
        device_count++;
    }
    *instance = (VkInstance)(uintptr_t)&instance_object;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
                                                          VkPhysicalDevice *gpus) {
    (void)instance;
    uint32_t written = device_count < *count ? device_count : *count;
    for (uint32_t index = 0; index < written; index++) {
        gpus[index] = (VkPhysicalDevice)(uintptr_t)&device_objects[index];
    }
    *count = written;
    return written < device_count ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice gpu,
                                                         VkPhysicalDeviceProperties *properties) {
    *properties = devices[index_of(gpu, __func__)];
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice gpu, uint32_t *count,
                                                                    VkQueueFamilyProperties *families) {
    index_of(gpu, __func__);
    families[0] = (VkQueueFamilyProperties){.queueFlags = VK_QUEUE_GRAPHICS_BIT, .queueCount = 1};
    *count = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)info;
    (void)allocator;
    (void)device;
    fprintf(stderr, "device-list: vkCreateDevice got %s\n", devices[index_of(gpu, __func__)].deviceName);
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)instance;
    (void)allocator;
}
