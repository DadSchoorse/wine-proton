/* Wine Vulkan ICD implementation
 *
 * Copyright 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"

#include "vulkan_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* For now default to 4 as it felt like a reasonable version feature wise to support.
 * Don't support the optional vk_icdGetPhysicalDeviceProcAddr introduced in this version
 * as it is unlikely we will implement physical device extensions, which the loader is not
 * aware of. Version 5 adds more extensive version checks. Something to tackle later.
 */
#define WINE_VULKAN_ICD_VERSION 4

#define wine_vk_find_struct(s, t) wine_vk_find_struct_((void *)s, VK_STRUCTURE_TYPE_##t)
static void *wine_vk_find_struct_(void *s, VkStructureType t)
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t)
            return header;
    }

    return NULL;
}

static void *wine_vk_get_global_proc_addr(const char *name);

static const struct vulkan_funcs *vk_funcs;
static VkResult (*p_vkEnumerateInstanceVersion)(uint32_t *version);

void WINAPI wine_vkGetPhysicalDeviceProperties(VkPhysicalDevice physical_device,
        VkPhysicalDeviceProperties *properties);

static void wine_vk_physical_device_free(struct VkPhysicalDevice_T *phys_dev)
{
    if (!phys_dev)
        return;

    heap_free(phys_dev->extensions);
    heap_free(phys_dev);
}

static struct VkPhysicalDevice_T *wine_vk_physical_device_alloc(struct VkInstance_T *instance,
        VkPhysicalDevice phys_dev)
{
    struct VkPhysicalDevice_T *object;
    uint32_t num_host_properties, num_properties = 0;
    VkExtensionProperties *host_properties = NULL;
    VkResult res;
    unsigned int i, j;

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return NULL;

    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
    object->instance = instance;
    object->phys_dev = phys_dev;

    res = instance->funcs.p_vkEnumerateDeviceExtensionProperties(phys_dev,
            NULL, &num_host_properties, NULL);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate device extensions, res=%d\n", res);
        goto err;
    }

    host_properties = heap_calloc(num_host_properties, sizeof(*host_properties));
    if (!host_properties)
    {
        ERR("Failed to allocate memory for device properties!\n");
        goto err;
    }

    res = instance->funcs.p_vkEnumerateDeviceExtensionProperties(phys_dev,
            NULL, &num_host_properties, host_properties);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate device extensions, res=%d\n", res);
        goto err;
    }

    /* Count list of extensions for which we have an implementation.
     * TODO: perform translation for platform specific extensions.
     */
    for (i = 0; i < num_host_properties; i++)
    {
        if (wine_vk_device_extension_supported(host_properties[i].extensionName))
        {
            TRACE("Enabling extension '%s' for physical device %p\n", host_properties[i].extensionName, object);
            num_properties++;
        }
        else
        {
            TRACE("Skipping extension '%s', no implementation found in winevulkan.\n", host_properties[i].extensionName);
        }
    }

    TRACE("Host supported extensions %u, Wine supported extensions %u\n", num_host_properties, num_properties);

    if (!(object->extensions = heap_calloc(num_properties, sizeof(*object->extensions))))
    {
        ERR("Failed to allocate memory for device extensions!\n");
        goto err;
    }

    for (i = 0, j = 0; i < num_host_properties; i++)
    {
        if (wine_vk_device_extension_supported(host_properties[i].extensionName))
        {
            object->extensions[j] = host_properties[i];
            j++;
        }
    }
    object->extension_count = num_properties;

    heap_free(host_properties);
    return object;

err:
    wine_vk_physical_device_free(object);
    heap_free(host_properties);
    return NULL;
}

static void wine_vk_free_command_buffers(struct VkDevice_T *device,
        struct wine_cmd_pool *pool, uint32_t count, const VkCommandBuffer *buffers)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        if (!buffers[i])
            continue;

        device->funcs.p_vkFreeCommandBuffers(device->device, pool->command_pool, 1, &buffers[i]->command_buffer);
        list_remove(&buffers[i]->pool_link);
        heap_free(buffers[i]);
    }
}

static struct VkQueue_T *wine_vk_device_alloc_queues(struct VkDevice_T *device,
        uint32_t family_index, uint32_t queue_count, VkDeviceQueueCreateFlags flags)
{
    VkDeviceQueueInfo2 queue_info;
    struct VkQueue_T *queues;
    unsigned int i;

    if (!(queues = heap_calloc(queue_count, sizeof(*queues))))
    {
        ERR("Failed to allocate memory for queues\n");
        return NULL;
    }

    for (i = 0; i < queue_count; i++)
    {
        struct VkQueue_T *queue = &queues[i];

        queue->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
        queue->device = device;
        queue->flags = flags;

        /* The Vulkan spec says:
         *
         * "vkGetDeviceQueue must only be used to get queues that were created
         * with the flags parameter of VkDeviceQueueCreateInfo set to zero."
         */
        if (flags && device->funcs.p_vkGetDeviceQueue2)
        {
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
            queue_info.pNext = NULL;
            queue_info.flags = flags;
            queue_info.queueFamilyIndex = family_index;
            queue_info.queueIndex = i;
            device->funcs.p_vkGetDeviceQueue2(device->device, &queue_info, &queue->queue);
        }
        else
        {
            device->funcs.p_vkGetDeviceQueue(device->device, family_index, i, &queue->queue);
        }
    }

    return queues;
}

static void wine_vk_device_free_create_info(VkDeviceCreateInfo *create_info)
{
    VkDeviceGroupDeviceCreateInfo *group_info;

    if ((group_info = wine_vk_find_struct(create_info, DEVICE_GROUP_DEVICE_CREATE_INFO)))
    {
        heap_free((void *)group_info->pPhysicalDevices);
    }

    free_VkDeviceCreateInfo_struct_chain(create_info);
}

static VkResult wine_vk_device_convert_create_info(const VkDeviceCreateInfo *src,
        VkDeviceCreateInfo *dst)
{
    VkDeviceGroupDeviceCreateInfo *group_info;
    unsigned int i;
    VkResult res;

    *dst = *src;

    if ((res = convert_VkDeviceCreateInfo_struct_chain(src->pNext, dst)) < 0)
    {
        WARN("Failed to convert VkDeviceCreateInfo pNext chain, res=%d.\n", res);
        return res;
    }

    /* FIXME: convert_VkDeviceCreateInfo_struct_chain() should unwrap handles for us. */
    if ((group_info = wine_vk_find_struct(dst, DEVICE_GROUP_DEVICE_CREATE_INFO)))
    {
        VkPhysicalDevice *physical_devices;

        if (!(physical_devices = heap_calloc(group_info->physicalDeviceCount, sizeof(*physical_devices))))
        {
            free_VkDeviceCreateInfo_struct_chain(dst);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (i = 0; i < group_info->physicalDeviceCount; ++i)
        {
            physical_devices[i] = group_info->pPhysicalDevices[i]->phys_dev;
        }
        group_info->pPhysicalDevices = physical_devices;
    }

    /* Should be filtered out by loader as ICDs don't support layers. */
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;

    TRACE("Enabled extensions: %u.\n", dst->enabledExtensionCount);
    for (i = 0; i < dst->enabledExtensionCount; i++)
    {
        TRACE("Extension %u: %s.\n", i, debugstr_a(dst->ppEnabledExtensionNames[i]));
    }

    return VK_SUCCESS;
}

/* Helper function used for freeing a device structure. This function supports full
 * and partial object cleanups and can thus be used for vkCreateDevice failures.
 */
static void wine_vk_device_free(struct VkDevice_T *device)
{
    if (!device)
        return;

    if (device->queues)
    {
        unsigned int i;
        for (i = 0; i < device->max_queue_families; i++)
        {
            heap_free(device->queues[i]);
        }
        heap_free(device->queues);
        device->queues = NULL;
    }

    if (device->device && device->funcs.p_vkDestroyDevice)
    {
        device->funcs.p_vkDestroyDevice(device->device, NULL /* pAllocator */);
    }

    heap_free(device->queue_props);
    heap_free(device->swapchains);
    DeleteCriticalSection(&device->swapchain_lock);

    heap_free(device);
}

static BOOL wine_vk_init(void)
{
    HDC hdc;

    hdc = GetDC(0);
    vk_funcs = __wine_get_vulkan_driver(hdc, WINE_VULKAN_DRIVER_VERSION);
    ReleaseDC(0, hdc);
    if (!vk_funcs)
    {
        ERR("Failed to load Wine graphics driver supporting Vulkan.\n");
        return FALSE;
    }

    p_vkEnumerateInstanceVersion = vk_funcs->p_vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");

    return TRUE;
}

/* Helper function for converting between win32 and host compatible VkInstanceCreateInfo.
 * This function takes care of extensions handled at winevulkan layer, a Wine graphics
 * driver is responsible for handling e.g. surface extensions.
 */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src,
        VkInstanceCreateInfo *dst)
{
    unsigned int i;
    VkResult res;

    *dst = *src;

    if ((res = convert_VkInstanceCreateInfo_struct_chain(src->pNext, dst)) < 0)
    {
        WARN("Failed to convert VkInstanceCreateInfo pNext chain, res=%d.\n", res);
        return res;
    }

    /* ICDs don't support any layers, so nothing to copy. Modern versions of the loader
     * filter this data out as well.
     */
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;

    TRACE("Enabled extensions: %u\n", dst->enabledExtensionCount);
    for (i = 0; i < dst->enabledExtensionCount; i++)
    {
        TRACE("Extension %u: %s\n", i, debugstr_a(dst->ppEnabledExtensionNames[i]));
    }

    return VK_SUCCESS;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult wine_vk_instance_load_physical_devices(struct VkInstance_T *instance)
{
    VkPhysicalDevice *tmp_phys_devs;
    uint32_t phys_dev_count;
    unsigned int i;
    VkResult res;

    res = instance->funcs.p_vkEnumeratePhysicalDevices(instance->instance, &phys_dev_count, NULL);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to enumerate physical devices, res=%d\n", res);
        return res;
    }
    if (!phys_dev_count)
        return res;

    if (!(tmp_phys_devs = heap_calloc(phys_dev_count, sizeof(*tmp_phys_devs))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = instance->funcs.p_vkEnumeratePhysicalDevices(instance->instance, &phys_dev_count, tmp_phys_devs);
    if (res != VK_SUCCESS)
    {
        heap_free(tmp_phys_devs);
        return res;
    }

    instance->phys_devs = heap_calloc(phys_dev_count, sizeof(*instance->phys_devs));
    if (!instance->phys_devs)
    {
        heap_free(tmp_phys_devs);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    /* Wrap each native physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < phys_dev_count; i++)
    {
        struct VkPhysicalDevice_T *phys_dev = wine_vk_physical_device_alloc(instance, tmp_phys_devs[i]);
        if (!phys_dev)
        {
            ERR("Unable to allocate memory for physical device!\n");
            heap_free(tmp_phys_devs);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        instance->phys_devs[i] = phys_dev;
        instance->phys_dev_count = i + 1;
    }
    instance->phys_dev_count = phys_dev_count;

    heap_free(tmp_phys_devs);
    return VK_SUCCESS;
}

static struct VkPhysicalDevice_T *wine_vk_instance_wrap_physical_device(struct VkInstance_T *instance,
        VkPhysicalDevice physical_device)
{
    unsigned int i;

    for (i = 0; i < instance->phys_dev_count; ++i)
    {
        struct VkPhysicalDevice_T *current = instance->phys_devs[i];
        if (current->phys_dev == physical_device)
            return current;
    }

    ERR("Unrecognized physical device %p.\n", physical_device);
    return NULL;
}

/* Helper function used for freeing an instance structure. This function supports full
 * and partial object cleanups and can thus be used for vkCreateInstance failures.
 */
static void wine_vk_instance_free(struct VkInstance_T *instance)
{
    if (!instance)
        return;

    if (instance->phys_devs)
    {
        unsigned int i;

        for (i = 0; i < instance->phys_dev_count; i++)
        {
            wine_vk_physical_device_free(instance->phys_devs[i]);
        }
        heap_free(instance->phys_devs);
    }

    if (instance->instance)
        vk_funcs->p_vkDestroyInstance(instance->instance, NULL /* allocator */);

    heap_free(instance);
}

VkResult WINAPI wine_vkAllocateCommandBuffers(VkDevice device,
        const VkCommandBufferAllocateInfo *allocate_info, VkCommandBuffer *buffers)
{
    struct wine_cmd_pool *pool;
    VkResult res = VK_SUCCESS;
    unsigned int i;

    TRACE("%p, %p, %p\n", device, allocate_info, buffers);

    pool = wine_cmd_pool_from_handle(allocate_info->commandPool);

    memset(buffers, 0, allocate_info->commandBufferCount * sizeof(*buffers));

    for (i = 0; i < allocate_info->commandBufferCount; i++)
    {
#if defined(USE_STRUCT_CONVERSION)
        VkCommandBufferAllocateInfo_host allocate_info_host;
#else
        VkCommandBufferAllocateInfo allocate_info_host;
#endif
        /* TODO: future extensions (none yet) may require pNext conversion. */
        allocate_info_host.pNext = allocate_info->pNext;
        allocate_info_host.sType = allocate_info->sType;
        allocate_info_host.commandPool = pool->command_pool;
        allocate_info_host.level = allocate_info->level;
        allocate_info_host.commandBufferCount = 1;

        TRACE("Allocating command buffer %u from pool 0x%s.\n",
                i, wine_dbgstr_longlong(allocate_info_host.commandPool));

        if (!(buffers[i] = heap_alloc_zero(sizeof(**buffers))))
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }

        buffers[i]->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;
        buffers[i]->device = device;
        list_add_tail(&pool->command_buffers, &buffers[i]->pool_link);
        res = device->funcs.p_vkAllocateCommandBuffers(device->device,
                &allocate_info_host, &buffers[i]->command_buffer);
        if (res != VK_SUCCESS)
        {
            ERR("Failed to allocate command buffer, res=%d.\n", res);
            buffers[i]->command_buffer = VK_NULL_HANDLE;
            break;
        }
    }

    if (res != VK_SUCCESS)
    {
        wine_vk_free_command_buffers(device, pool, i + 1, buffers);
        memset(buffers, 0, allocate_info->commandBufferCount * sizeof(*buffers));
    }

    return res;
}

void WINAPI wine_vkCmdExecuteCommands(VkCommandBuffer buffer, uint32_t count,
        const VkCommandBuffer *buffers)
{
    VkCommandBuffer *tmp_buffers;
    unsigned int i;

    TRACE("%p %u %p\n", buffer, count, buffers);

    if (!buffers || !count)
        return;

    /* Unfortunately we need a temporary buffer as our command buffers are wrapped.
     * This call is called often and if a performance concern, we may want to use
     * alloca as we shouldn't need much memory and it needs to be cleaned up after
     * the call anyway.
     */
    if (!(tmp_buffers = heap_alloc(count * sizeof(*tmp_buffers))))
    {
        ERR("Failed to allocate memory for temporary command buffers\n");
        return;
    }

    for (i = 0; i < count; i++)
        tmp_buffers[i] = buffers[i]->command_buffer;

    buffer->device->funcs.p_vkCmdExecuteCommands(buffer->command_buffer, count, tmp_buffers);

    heap_free(tmp_buffers);
}

VkResult WINAPI wine_vkCreateDevice(VkPhysicalDevice phys_dev,
        const VkDeviceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkDeviceCreateInfo create_info_host;
    uint32_t max_queue_families;
    struct VkDevice_T *object;
    unsigned int i;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", phys_dev, create_info, allocator, device);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties properties;

        wine_vkGetPhysicalDeviceProperties(phys_dev, &properties);

        TRACE("Device name: %s.\n", debugstr_a(properties.deviceName));
        TRACE("Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID);
        TRACE("Driver version: %#x.\n", properties.driverVersion);
    }

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;

    res = wine_vk_device_convert_create_info(create_info, &create_info_host);
    if (res != VK_SUCCESS)
        goto fail;

    res = phys_dev->instance->funcs.p_vkCreateDevice(phys_dev->phys_dev,
            &create_info_host, NULL /* allocator */, &object->device);
    wine_vk_device_free_create_info(&create_info_host);
    if (res != VK_SUCCESS)
    {
        WARN("Failed to create device, res=%d.\n", res);
        goto fail;
    }

    object->phys_dev = phys_dev;

    /* Just load all function pointers we are aware off. The loader takes care of filtering.
     * We use vkGetDeviceProcAddr as opposed to vkGetInstanceProcAddr for efficiency reasons
     * as functions pass through fewer dispatch tables within the loader.
     */
#define USE_VK_FUNC(name) \
    object->funcs.p_##name = (void *)vk_funcs->p_vkGetDeviceProcAddr(object->device, #name); \
    if (object->funcs.p_##name == NULL) \
        TRACE("Not found '%s'.\n", #name);
    ALL_VK_DEVICE_FUNCS()
#undef USE_VK_FUNC

    /* We need to cache all queues within the device as each requires wrapping since queues are
     * dispatchable objects.
     */
    phys_dev->instance->funcs.p_vkGetPhysicalDeviceQueueFamilyProperties(phys_dev->phys_dev,
            &max_queue_families, NULL);
    object->max_queue_families = max_queue_families;
    TRACE("Max queue families: %u.\n", object->max_queue_families);

    if (!(object->queues = heap_calloc(max_queue_families, sizeof(*object->queues))))
    {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    for (i = 0; i < create_info_host.queueCreateInfoCount; i++)
    {
        uint32_t flags = create_info_host.pQueueCreateInfos[i].flags;
        uint32_t family_index = create_info_host.pQueueCreateInfos[i].queueFamilyIndex;
        uint32_t queue_count = create_info_host.pQueueCreateInfos[i].queueCount;

        TRACE("Queue family index %u, queue count %u.\n", family_index, queue_count);

        if (!(object->queues[family_index] = wine_vk_device_alloc_queues(object, family_index, queue_count, flags)))
        {
            ERR("Failed to allocate memory for queues.\n");
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }
    }

    object->quirks = phys_dev->instance->quirks;

    InitializeCriticalSection(&object->swapchain_lock);

    *device = object;
    TRACE("Created device %p (native device %p).\n", object, object->device);
    return VK_SUCCESS;

fail:
    wine_vk_device_free(object);
    return res;
}

VkResult WINAPI wine_vkCreateInstance(const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    VkInstanceCreateInfo create_info_host;
    const VkApplicationInfo *app_info;
    struct VkInstance_T *object;
    VkResult res;

    TRACE("create_info %p, allocator %p, instance %p\n", create_info, allocator, instance);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = heap_alloc_zero(sizeof(*object))))
    {
        ERR("Failed to allocate memory for instance\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;

    wine_vk_instance_convert_create_info(create_info, &create_info_host);

    res = vk_funcs->p_vkCreateInstance(&create_info_host, NULL /* allocator */, &object->instance);
    free_VkInstanceCreateInfo_struct_chain(&create_info_host);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create instance, res=%d\n", res);
        wine_vk_instance_free(object);
        return res;
    }

    /* Load all instance functions we are aware of. Note the loader takes care
     * of any filtering for extensions which were not requested, but which the
     * ICD may support.
     */
#define USE_VK_FUNC(name) \
    object->funcs.p_##name = (void *)vk_funcs->p_vkGetInstanceProcAddr(object->instance, #name);
    ALL_VK_INSTANCE_FUNCS()
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as
     * each vkPhysicalDevice is a dispatchable object, which means we need to wrap
     * the native physical devices and present those to the application.
     * Cleanup happens as part of wine_vkDestroyInstance.
     */
    res = wine_vk_instance_load_physical_devices(object);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to load physical devices, res=%d\n", res);
        wine_vk_instance_free(object);
        return res;
    }

    if ((app_info = create_info->pApplicationInfo))
    {
        TRACE("Application name %s, application version %#x.\n",
                debugstr_a(app_info->pApplicationName), app_info->applicationVersion);
        TRACE("Engine name %s, engine version %#x.\n", debugstr_a(app_info->pEngineName),
                app_info->engineVersion);
        TRACE("API version %#x.\n", app_info->apiVersion);

        if (app_info->pEngineName && !strcmp(app_info->pEngineName, "idTech"))
            object->quirks |= WINEVULKAN_QUIRK_GET_DEVICE_PROC_ADDR;
    }

    *instance = object;
    TRACE("Created instance %p (native instance %p).\n", object, object->instance);
    return VK_SUCCESS;
}

void WINAPI wine_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    TRACE("%p %p\n", device, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_device_free(device);
}

void WINAPI wine_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    TRACE("%p, %p\n", instance, allocator);

    if (allocator)
        FIXME("Support allocation allocators\n");

    wine_vk_instance_free(instance);
}

VkResult WINAPI wine_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice phys_dev,
        const char *layer_name, uint32_t *count, VkExtensionProperties *properties)
{
    TRACE("%p, %p, %p, %p\n", phys_dev, layer_name, count, properties);

    /* This shouldn't get called with layer_name set, the ICD loader prevents it. */
    if (layer_name)
    {
        ERR("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (!properties)
    {
        *count = phys_dev->extension_count;
        return VK_SUCCESS;
    }

    *count = min(*count, phys_dev->extension_count);
    memcpy(properties, phys_dev->extensions, *count * sizeof(*properties));

    TRACE("Returning %u extensions.\n", *count);
    return *count < phys_dev->extension_count ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult WINAPI wine_vkEnumerateInstanceExtensionProperties(const char *layer_name,
        uint32_t *count, VkExtensionProperties *properties)
{
    uint32_t num_properties = 0, num_host_properties;
    VkExtensionProperties *host_properties;
    unsigned int i, j;
    VkResult res;

    TRACE("%p, %p, %p\n", layer_name, count, properties);

    if (layer_name)
    {
        WARN("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    res = vk_funcs->p_vkEnumerateInstanceExtensionProperties(NULL, &num_host_properties, NULL);
    if (res != VK_SUCCESS)
        return res;

    if (!(host_properties = heap_calloc(num_host_properties, sizeof(*host_properties))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = vk_funcs->p_vkEnumerateInstanceExtensionProperties(NULL, &num_host_properties, host_properties);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to retrieve host properties, res=%d.\n", res);
        heap_free(host_properties);
        return res;
    }

    /* The Wine graphics driver provides us with all extensions supported by the host side
     * including extension fixup (e.g. VK_KHR_xlib_surface -> VK_KHR_win32_surface). It is
     * up to us here to filter the list down to extensions for which we have thunks.
     */
    for (i = 0; i < num_host_properties; i++)
    {
        if (wine_vk_instance_extension_supported(host_properties[i].extensionName))
            num_properties++;
        else
            TRACE("Instance extension '%s' is not supported.\n", host_properties[i].extensionName);
    }

    if (!properties)
    {
        TRACE("Returning %u extensions.\n", num_properties);
        *count = num_properties;
        heap_free(host_properties);
        return VK_SUCCESS;
    }

    for (i = 0, j = 0; i < num_host_properties && j < *count; i++)
    {
        if (wine_vk_instance_extension_supported(host_properties[i].extensionName))
        {
            TRACE("Enabling extension '%s'.\n", host_properties[i].extensionName);
            properties[j++] = host_properties[i];
        }
    }
    *count = min(*count, num_properties);

    heap_free(host_properties);
    return *count < num_properties ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult WINAPI wine_vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *properties)
{
    TRACE("%p, %p\n", count, properties);

    if (!properties)
    {
        *count = 0;
        return VK_SUCCESS;
    }

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VkResult WINAPI wine_vkEnumerateInstanceVersion(uint32_t *version)
{
    VkResult res;

    TRACE("%p\n", version);

    if (p_vkEnumerateInstanceVersion)
    {
        res = p_vkEnumerateInstanceVersion(version);
    }
    else
    {
        *version = VK_API_VERSION_1_0;
        res = VK_SUCCESS;
    }

    TRACE("API version %u.%u.%u.\n",
            VK_VERSION_MAJOR(*version), VK_VERSION_MINOR(*version), VK_VERSION_PATCH(*version));
    *version = min(WINE_VK_VERSION, *version);
    return res;
}

VkResult WINAPI wine_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
        VkPhysicalDevice *devices)
{
    unsigned int i;

    TRACE("%p %p %p\n", instance, count, devices);

    if (!devices)
    {
        *count = instance->phys_dev_count;
        return VK_SUCCESS;
    }

    *count = min(*count, instance->phys_dev_count);
    for (i = 0; i < *count; i++)
    {
        devices[i] = instance->phys_devs[i];
    }

    TRACE("Returning %u devices.\n", *count);
    return *count < instance->phys_dev_count ? VK_INCOMPLETE : VK_SUCCESS;
}

void WINAPI wine_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool_handle,
        uint32_t count, const VkCommandBuffer *buffers)
{
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(pool_handle);

    TRACE("%p, 0x%s, %u, %p\n", device, wine_dbgstr_longlong(pool_handle), count, buffers);

    wine_vk_free_command_buffers(device, pool, count, buffers);
}

PFN_vkVoidFunction WINAPI wine_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    void *func;
    TRACE("%p, %s\n", device, debugstr_a(name));

    /* The spec leaves return value undefined for a NULL device, let's just return NULL. */
    if (!device || !name)
        return NULL;

    /* Per the spec, we are only supposed to return device functions as in functions
     * for which the first parameter is vkDevice or a child of vkDevice like a
     * vkCommandBuffer or vkQueue.
     * Loader takes care of filtering of extensions which are enabled or not.
     */
    func = wine_vk_get_device_proc_addr(name);
    if (func)
        return func;

    /* vkGetDeviceProcAddr was intended for loading device and subdevice functions.
     * idTech 6 titles such as Doom and Wolfenstein II, however use it also for
     * loading of instance functions. This is undefined behavior as the specification
     * disallows using any of the returned function pointers outside of device /
     * subdevice objects. The games don't actually use the function pointers and if they
     * did, they would crash as VkInstance / VkPhysicalDevice parameters need unwrapping.
     * Khronos clarified behavior in the Vulkan spec and expects drivers to get updated,
     * however it would require both driver and game fixes.
     * https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/issues/2323
     * https://github.com/KhronosGroup/Vulkan-Docs/issues/655
     */
    if (device->quirks & WINEVULKAN_QUIRK_GET_DEVICE_PROC_ADDR
            && (func = wine_vk_get_instance_proc_addr(name)))
    {
        WARN("Returning instance function %s.\n", debugstr_a(name));
        return func;
    }

    WARN("Unsupported device function: %s.\n", debugstr_a(name));
    return NULL;
}

void WINAPI wine_vkGetDeviceQueue(VkDevice device, uint32_t family_index,
        uint32_t queue_index, VkQueue *queue)
{
    TRACE("%p, %u, %u, %p\n", device, family_index, queue_index, queue);

    *queue = &device->queues[family_index][queue_index];
}

void WINAPI wine_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    struct VkQueue_T *matching_queue;
    const VkBaseInStructure *chain;

    TRACE("%p, %p, %p\n", device, info, queue);

    if ((chain = info->pNext))
        FIXME("Ignoring a linked structure of type %u.\n", chain->sType);

    matching_queue = &device->queues[info->queueFamilyIndex][info->queueIndex];
    if (matching_queue->flags != info->flags)
    {
        WARN("No matching flags were specified %#x, %#x.\n", matching_queue->flags, info->flags);
        matching_queue = VK_NULL_HANDLE;
    }
    *queue = matching_queue;
}

PFN_vkVoidFunction WINAPI wine_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    void *func;

    TRACE("%p, %s\n", instance, debugstr_a(name));

    if (!name)
        return NULL;

    /* vkGetInstanceProcAddr can load most Vulkan functions when an instance is passed in, however
     * for a NULL instance it can only load global functions.
     */
    func = wine_vk_get_global_proc_addr(name);
    if (func)
    {
        return func;
    }
    if (!instance)
    {
        WARN("Global function %s not found.\n", debugstr_a(name));
        return NULL;
    }

    func = wine_vk_get_instance_proc_addr(name);
    if (func) return func;

    /* vkGetInstanceProcAddr also loads any children of instance, so device functions as well. */
    func = wine_vk_get_device_proc_addr(name);
    if (func) return func;

    WARN("Unsupported device or instance function: %s.\n", debugstr_a(name));
    return NULL;
}

void * WINAPI wine_vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    TRACE("%p, %s\n", instance, debugstr_a(name));

    /* Initial version of the Vulkan ICD spec required vkGetInstanceProcAddr to be
     * exported. vk_icdGetInstanceProcAddr was added later to separate ICD calls from
     * Vulkan API. One of them in our case should forward to the other, so just forward
     * to the older vkGetInstanceProcAddr.
     */
    return wine_vkGetInstanceProcAddr(instance, name);
}

VkResult WINAPI wine_vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *supported_version)
{
    uint32_t req_version;

    TRACE("%p\n", supported_version);

    /* The spec is not clear how to handle this. Mesa drivers don't check, but it
     * is probably best to not explode. VK_INCOMPLETE seems to be the closest value.
     */
    if (!supported_version)
        return VK_INCOMPLETE;

    req_version = *supported_version;
    *supported_version = min(req_version, WINE_VULKAN_ICD_VERSION);
    TRACE("Loader requested ICD version %u, returning %u\n", req_version, *supported_version);

    return VK_SUCCESS;
}

VkResult WINAPI wine_vkQueueSubmit(VkQueue queue, uint32_t count,
        const VkSubmitInfo *submits, VkFence fence)
{
    VkSubmitInfo *submits_host;
    VkResult res;
    VkCommandBuffer *command_buffers;
    unsigned int i, j, num_command_buffers;

    TRACE("%p %u %p 0x%s\n", queue, count, submits, wine_dbgstr_longlong(fence));

    if (count == 0)
    {
        return queue->device->funcs.p_vkQueueSubmit(queue->queue, 0, NULL, fence);
    }

    submits_host = heap_calloc(count, sizeof(*submits_host));
    if (!submits_host)
    {
        ERR("Unable to allocate memory for submit buffers!\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (i = 0; i < count; i++)
    {
        memcpy(&submits_host[i], &submits[i], sizeof(*submits_host));

        num_command_buffers = submits[i].commandBufferCount;
        command_buffers = heap_calloc(num_command_buffers, sizeof(*submits_host));
        if (!command_buffers)
        {
            ERR("Unable to allocate memory for command buffers!\n");
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }

        for (j = 0; j < num_command_buffers; j++)
        {
            command_buffers[j] = submits[i].pCommandBuffers[j]->command_buffer;
        }
        submits_host[i].pCommandBuffers = command_buffers;
    }

    res = queue->device->funcs.p_vkQueueSubmit(queue->queue, count, submits_host, fence);

done:
    for (i = 0; i < count; i++)
    {
        heap_free((void *)submits_host[i].pCommandBuffers);
    }
    heap_free(submits_host);

    TRACE("Returning %d\n", res);
    return res;
}

VkResult WINAPI wine_vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkCommandPool *command_pool)
{
    struct wine_cmd_pool *object;
    VkResult res;

    TRACE("%p, %p, %p, %p\n", device, info, allocator, command_pool);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    list_init(&object->command_buffers);

    res = device->funcs.p_vkCreateCommandPool(device->device, info, NULL, &object->command_pool);

    if (res == VK_SUCCESS)
        *command_pool = wine_cmd_pool_to_handle(object);
    else
        heap_free(object);

    return res;
}

void WINAPI wine_vkDestroyCommandPool(VkDevice device, VkCommandPool handle,
        const VkAllocationCallbacks *allocator)
{
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(handle);
    struct VkCommandBuffer_T *buffer, *cursor;

    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(handle), allocator);

    if (!handle)
        return;

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* The Vulkan spec says:
     *
     * "When a pool is destroyed, all command buffers allocated from the pool are freed."
     */
    LIST_FOR_EACH_ENTRY_SAFE(buffer, cursor, &pool->command_buffers, struct VkCommandBuffer_T, pool_link)
    {
        heap_free(buffer);
    }

    device->funcs.p_vkDestroyCommandPool(device->device, pool->command_pool, NULL);
    heap_free(pool);
}

static VkResult wine_vk_enumerate_physical_device_groups(struct VkInstance_T *instance,
        VkResult (*p_vkEnumeratePhysicalDeviceGroups)(VkInstance, uint32_t *, VkPhysicalDeviceGroupProperties *),
        uint32_t *count, VkPhysicalDeviceGroupProperties *properties)
{
    unsigned int i, j;
    VkResult res;

    res = p_vkEnumeratePhysicalDeviceGroups(instance->instance, count, properties);
    if (res < 0 || !properties)
        return res;

    for (i = 0; i < *count; ++i)
    {
        VkPhysicalDeviceGroupProperties *current = &properties[i];
        for (j = 0; j < current->physicalDeviceCount; ++j)
        {
            VkPhysicalDevice dev = current->physicalDevices[j];
            if (!(current->physicalDevices[j] = wine_vk_instance_wrap_physical_device(instance, dev)))
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    return res;
}

VkResult WINAPI wine_vkEnumeratePhysicalDeviceGroups(VkInstance instance,
        uint32_t *count, VkPhysicalDeviceGroupProperties *properties)
{
    TRACE("%p, %p, %p\n", instance, count, properties);
    return wine_vk_enumerate_physical_device_groups(instance,
            instance->funcs.p_vkEnumeratePhysicalDeviceGroups, count, properties);
}

VkResult WINAPI wine_vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance,
        uint32_t *count, VkPhysicalDeviceGroupProperties *properties)
{
    TRACE("%p, %p, %p\n", instance, count, properties);
    return wine_vk_enumerate_physical_device_groups(instance,
            instance->funcs.p_vkEnumeratePhysicalDeviceGroupsKHR, count, properties);
}

void WINAPI wine_vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalFenceInfo *fence_info, VkExternalFenceProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, fence_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalFenceFeatures = 0;
}

void WINAPI wine_vkGetPhysicalDeviceExternalFencePropertiesKHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalFenceInfo *fence_info, VkExternalFenceProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, fence_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalFenceFeatures = 0;
}

void WINAPI wine_vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalBufferInfo *buffer_info, VkExternalBufferProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, buffer_info, properties);
    memset(&properties->externalMemoryProperties, 0, sizeof(properties->externalMemoryProperties));
}

void WINAPI wine_vkGetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalBufferInfo *buffer_info, VkExternalBufferProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, buffer_info, properties);
    memset(&properties->externalMemoryProperties, 0, sizeof(properties->externalMemoryProperties));
}

VkResult WINAPI wine_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceImageFormatInfo2 *format_info, VkImageFormatProperties2 *properties)
{
    VkExternalImageFormatProperties *external_image_properties;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, format_info, properties);

    res = thunk_vkGetPhysicalDeviceImageFormatProperties2(phys_dev, format_info, properties);

    if ((external_image_properties = wine_vk_find_struct(properties, EXTERNAL_IMAGE_FORMAT_PROPERTIES)))
    {
        VkExternalMemoryProperties *p = &external_image_properties->externalMemoryProperties;
        p->externalMemoryFeatures = 0;
        p->exportFromImportedHandleTypes = 0;
        p->compatibleHandleTypes = 0;
    }

    return res;
}

VkResult WINAPI wine_vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceImageFormatInfo2 *format_info, VkImageFormatProperties2 *properties)
{
    VkExternalImageFormatProperties *external_image_properties;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, format_info, properties);

    res = thunk_vkGetPhysicalDeviceImageFormatProperties2KHR(phys_dev, format_info, properties);

    if ((external_image_properties = wine_vk_find_struct(properties, EXTERNAL_IMAGE_FORMAT_PROPERTIES)))
    {
        VkExternalMemoryProperties *p = &external_image_properties->externalMemoryProperties;
        p->externalMemoryFeatures = 0;
        p->exportFromImportedHandleTypes = 0;
        p->compatibleHandleTypes = 0;
    }

    return res;
}

void WINAPI wine_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info, VkExternalSemaphoreProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, semaphore_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalSemaphoreFeatures = 0;
}

void WINAPI wine_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info, VkExternalSemaphoreProperties *properties)
{
    TRACE("%p, %p, %p\n", phys_dev, semaphore_info, properties);
    properties->exportFromImportedHandleTypes = 0;
    properties->compatibleHandleTypes = 0;
    properties->externalSemaphoreFeatures = 0;
}

VkResult WINAPI wine_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
    VkResult res;
    VkExtent2D user_res;

    TRACE("%p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);

    res = thunk_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface, capabilities);

    /* HACK: It happened more than once that a Windows game didn't expect that maxImageCount can be
     * set to 0. A value of 0 means that there is no limit on the number of images. */
    if (res == VK_SUCCESS && capabilities->minImageCount && !capabilities->maxImageCount)
        capabilities->maxImageCount = 32;

    if(vk_funcs->query_fs_hack &&
            vk_funcs->query_fs_hack(NULL, &user_res, NULL, NULL)){
        capabilities->currentExtent = user_res;
        capabilities->minImageExtent = user_res;
        capabilities->maxImageExtent = user_res;
    }

    return res;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, void *reserved)
{
    TRACE("%p, %u, %p\n", hinst, reason, reserved);

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinst);
            return wine_vk_init();
    }
    return TRUE;
}

static const struct vulkan_func vk_global_dispatch_table[] =
{
    {"vkCreateInstance", &wine_vkCreateInstance},
    {"vkEnumerateInstanceExtensionProperties", &wine_vkEnumerateInstanceExtensionProperties},
    {"vkEnumerateInstanceLayerProperties", &wine_vkEnumerateInstanceLayerProperties},
    {"vkEnumerateInstanceVersion", &wine_vkEnumerateInstanceVersion},
    {"vkGetInstanceProcAddr", &wine_vkGetInstanceProcAddr},
};

static void *wine_vk_get_global_proc_addr(const char *name)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vk_global_dispatch_table); i++)
    {
        if (strcmp(name, vk_global_dispatch_table[i].name) == 0)
        {
            TRACE("Found name=%s in global table\n", debugstr_a(name));
            return vk_global_dispatch_table[i].func;
        }
    }
    return NULL;
}

/*
 * Wrapper around driver vkGetInstanceProcAddr implementation.
 * Allows winelib applications to access Vulkan functions with Wine
 * additions and native ABI.
 */
void *native_vkGetInstanceProcAddrWINE(VkInstance instance, const char *name)
{
    return vk_funcs->p_vkGetInstanceProcAddr(instance, name);
}

VkResult WINAPI wine_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)swapchain;
    TRACE("%p, 0x%s, 0x%s, 0x%s, 0x%s, %p\n", device, wine_dbgstr_longlong(swapchain), wine_dbgstr_longlong(timeout), wine_dbgstr_longlong(semaphore), wine_dbgstr_longlong(fence), pImageIndex);
    return device->funcs.p_vkAcquireNextImageKHR(device->device, object->swapchain, timeout, semaphore, fence, pImageIndex);
}

#if defined(USE_STRUCT_CONVERSION)
static inline void convert_VkSwapchainCreateInfoKHR_win_to_host(const VkSwapchainCreateInfoKHR *in, VkSwapchainCreateInfoKHR_host *out)
#else
static inline void convert_VkSwapchainCreateInfoKHR_win_to_host(const VkSwapchainCreateInfoKHR *in, VkSwapchainCreateInfoKHR *out)
#endif
{
    if (!in) return;

    out->sType = in->sType;
    out->pNext = in->pNext;
    out->flags = in->flags;
    out->surface = in->surface;
    out->minImageCount = in->minImageCount;
    out->imageFormat = in->imageFormat;
    out->imageColorSpace = in->imageColorSpace;
    out->imageExtent = in->imageExtent;
    out->imageArrayLayers = in->imageArrayLayers;
    out->imageUsage = in->imageUsage;
    out->imageSharingMode = in->imageSharingMode;
    out->queueFamilyIndexCount = in->queueFamilyIndexCount;
    out->pQueueFamilyIndices = in->pQueueFamilyIndices;
    out->preTransform = in->preTransform;
    out->compositeAlpha = in->compositeAlpha;
    out->presentMode = in->presentMode;
    out->clipped = in->clipped;
    out->oldSwapchain = in->oldSwapchain;
}

/*
#version 450

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform pushConstants {
    //both in real image coords
    vec2 offset;
    vec2 extents;
} constants;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    vec2 texcoord = (vec2(gl_GlobalInvocationID.xy) - constants.offset) / constants.extents;
    vec4 c = texture(texSampler, texcoord);
    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), c.bgra);
}
*/
const uint32_t blit_comp_spv[] = {
    0x07230203, 0x00010000, 0x00080008, 0x00001ED8, 0x00000000, 0x00020011, 0x00000001, 0x00020011, 0x00000032,
    0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000004, 0x6E69616D, 0x00000000, 0x00000BB7, 0x00060010, 0x00000004, 0x00000011,
    0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002, 0x000001C2, 0x000A0004, 0x475F4C47, 0x4C474F4F,
    0x70635F45, 0x74735F70, 0x5F656C79, 0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004, 0x475F4C47,
    0x4C474F4F, 0x6E695F45, 0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6E69616D,
    0x00000000, 0x00060005, 0x00000072, 0x75706E69, 0x6D615374, 0x72656C70, 0x00000000, 0x00050005, 0x00000BA1,
    0x7074756F, 0x6D497475, 0x00656761, 0x00080005, 0x00000BB7, 0x475F6C67, 0x61626F6C, 0x766E496C, 0x7461636F,
    0x496E6F69, 0x00000044, 0x00040047, 0x00000072, 0x00000022, 0x00000000, 0x00040047, 0x00000072, 0x00000021,
    0x00000000, 0x00040047, 0x00000BA1, 0x00000022, 0x00000000, 0x00040047, 0x00000BA1, 0x00000021, 0x00000001,
    0x00040047, 0x00000BB7, 0x0000000B, 0x0000001C, 0x00040047, 0x00000BDF, 0x0000000B, 0x00000019, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040015, 0x00000022,
    0x00000020, 0x00000000, 0x00040017, 0x00000023, 0x00000022, 0x00000002, 0x0004002B, 0x00000006, 0x0000005F,
    0x3F800000, 0x0004002B, 0x00000006, 0x00000067, 0x00000000, 0x00090019, 0x0000006F, 0x00000006, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001B, 0x00000070, 0x0000006F, 0x00040020,
    0x00000071, 0x00000000, 0x00000070, 0x0004003B, 0x00000071, 0x00000072, 0x00000000, 0x00040015, 0x00000075,
    0x00000020, 0x00000001, 0x00040017, 0x00000076, 0x00000075, 0x00000002, 0x0004002B, 0x00000075, 0x00000078,
    0x00000000, 0x00040017, 0x0000007A, 0x00000006, 0x00000004, 0x0004002B, 0x00000022, 0x0000008E, 0x00000001,
    0x0004002B, 0x00000006, 0x00000090, 0x3F000000, 0x00040017, 0x000000BA, 0x00000006, 0x00000002, 0x0004002B,
    0x00000075, 0x000000DA, 0xFFFFFFFF, 0x0005002C, 0x00000076, 0x000000DB, 0x000000DA, 0x000000DA, 0x0005002C,
    0x00000076, 0x000000E2, 0x00000078, 0x000000DA, 0x0004002B, 0x00000075, 0x000000E9, 0x00000001, 0x0005002C,
    0x00000076, 0x000000EA, 0x000000E9, 0x000000DA, 0x0005002C, 0x00000076, 0x000000F1, 0x000000DA, 0x00000078,
    0x0005002C, 0x00000076, 0x000000FD, 0x000000E9, 0x00000078, 0x0005002C, 0x00000076, 0x00000104, 0x000000DA,
    0x000000E9, 0x0005002C, 0x00000076, 0x0000010B, 0x00000078, 0x000000E9, 0x0005002C, 0x00000076, 0x00000112,
    0x000000E9, 0x000000E9, 0x0004002B, 0x00000006, 0x000002A5, 0x40000000, 0x0004002B, 0x00000075, 0x00000382,
    0x00000002, 0x0005002C, 0x00000076, 0x00000383, 0x00000382, 0x000000DA, 0x0005002C, 0x00000076, 0x00000390,
    0x00000382, 0x00000078, 0x0005002C, 0x00000076, 0x000003A3, 0x000000DA, 0x00000382, 0x0005002C, 0x00000076,
    0x000003AA, 0x00000078, 0x00000382, 0x0005002C, 0x00000076, 0x000003B7, 0x00000382, 0x000000E9, 0x0005002C,
    0x00000076, 0x000003BE, 0x000000E9, 0x00000382, 0x0005002C, 0x00000076, 0x000003C5, 0x00000382, 0x00000382,
    0x0004002B, 0x00000006, 0x000009AF, 0x3D000000, 0x00090019, 0x00000B9F, 0x00000006, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000002, 0x00000004, 0x00040020, 0x00000BA0, 0x00000000, 0x00000B9F, 0x0004003B,
    0x00000BA0, 0x00000BA1, 0x00000000, 0x00040017, 0x00000BB5, 0x00000022, 0x00000003, 0x00040020, 0x00000BB6,
    0x00000001, 0x00000BB5, 0x0004003B, 0x00000BB6, 0x00000BB7, 0x00000001, 0x0004002B, 0x00000022, 0x00000BDE,
    0x00000008, 0x0006002C, 0x00000BB5, 0x00000BDF, 0x00000BDE, 0x00000BDE, 0x0000008E, 0x0004002B, 0x00000006,
    0x00001ECA, 0xBE4CCCCD, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200F8, 0x00000005,
    0x0004003D, 0x00000070, 0x00000B95, 0x00000072, 0x00040064, 0x0000006F, 0x00000B96, 0x00000B95, 0x00050067,
    0x00000076, 0x00000B97, 0x00000B96, 0x00000078, 0x00050051, 0x00000075, 0x00000B98, 0x00000B97, 0x00000000,
    0x0004006F, 0x00000006, 0x00000B99, 0x00000B98, 0x00040064, 0x0000006F, 0x00000B9B, 0x00000B95, 0x00050067,
    0x00000076, 0x00000B9C, 0x00000B9B, 0x00000078, 0x00050051, 0x00000075, 0x00000B9D, 0x00000B9C, 0x00000001,
    0x0004006F, 0x00000006, 0x00000B9E, 0x00000B9D, 0x0004003D, 0x00000B9F, 0x00000BA2, 0x00000BA1, 0x00040068,
    0x00000076, 0x00000BA3, 0x00000BA2, 0x00050051, 0x00000075, 0x00000BA4, 0x00000BA3, 0x00000000, 0x0004006F,
    0x00000006, 0x00000BA5, 0x00000BA4, 0x0004003D, 0x00000B9F, 0x00000BA6, 0x00000BA1, 0x00040068, 0x00000076,
    0x00000BA7, 0x00000BA6, 0x00050051, 0x00000075, 0x00000BA8, 0x00000BA7, 0x00000001, 0x0004006F, 0x00000006,
    0x00000BA9, 0x00000BA8, 0x00050088, 0x00000006, 0x00000C31, 0x0000005F, 0x00000BA5, 0x00050085, 0x00000006,
    0x00000BFA, 0x00000B99, 0x00000C31, 0x0004007C, 0x00000022, 0x00000BFB, 0x00000BFA, 0x00050088, 0x00000006,
    0x00000C38, 0x0000005F, 0x00000BA9, 0x00050085, 0x00000006, 0x00000C00, 0x00000B9E, 0x00000C38, 0x0004007C,
    0x00000022, 0x00000C01, 0x00000C00, 0x00050085, 0x00000006, 0x00000C05, 0x00000090, 0x00000B99, 0x00050085,
    0x00000006, 0x00000C08, 0x00000C05, 0x00000C31, 0x00050083, 0x00000006, 0x00000C0A, 0x00000C08, 0x00000090,
    0x0004007C, 0x00000022, 0x00000C0B, 0x00000C0A, 0x00050085, 0x00000006, 0x00000C0F, 0x00000090, 0x00000B9E,
    0x00050085, 0x00000006, 0x00000C12, 0x00000C0F, 0x00000C38, 0x00050083, 0x00000006, 0x00000C14, 0x00000C12,
    0x00000090, 0x0004007C, 0x00000022, 0x00000C15, 0x00000C14, 0x0004007C, 0x00000022, 0x00000C1F, 0x00001ECA,
    0x0004003D, 0x00000BB5, 0x00000BB8, 0x00000BB7, 0x0007004F, 0x00000023, 0x00000BB9, 0x00000BB8, 0x00000BB8,
    0x00000000, 0x00000001, 0x00050051, 0x00000022, 0x00000BBA, 0x00000BB8, 0x00000000, 0x00050051, 0x00000022,
    0x00000BBB, 0x00000BB8, 0x00000001, 0x00050050, 0x00000023, 0x00000BBC, 0x00000BBA, 0x00000BBB, 0x00040070,
    0x000000BA, 0x00001107, 0x00000BBC, 0x00050050, 0x00000023, 0x0000110C, 0x00000BFB, 0x00000C01, 0x0004007C,
    0x000000BA, 0x0000110D, 0x0000110C, 0x00050085, 0x000000BA, 0x0000110E, 0x00001107, 0x0000110D, 0x00050050,
    0x00000023, 0x00001113, 0x00000C0B, 0x00000C15, 0x0004007C, 0x000000BA, 0x00001114, 0x00001113, 0x00050081,
    0x000000BA, 0x00001115, 0x0000110E, 0x00001114, 0x0006000C, 0x000000BA, 0x00001117, 0x00000001, 0x00000008,
    0x00001115, 0x00050083, 0x000000BA, 0x0000111A, 0x00001115, 0x00001117, 0x0004006E, 0x00000076, 0x0000111C,
    0x00001117, 0x00050080, 0x00000076, 0x0000111E, 0x0000111C, 0x000000DB, 0x0004007C, 0x00000023, 0x0000111F,
    0x0000111E, 0x0004007C, 0x00000076, 0x00001871, 0x0000111F, 0x00040064, 0x0000006F, 0x00001872, 0x00000B95,
    0x0007005F, 0x0000007A, 0x00001873, 0x00001872, 0x00001871, 0x00000002, 0x00000078, 0x00050080, 0x00000076,
    0x00001122, 0x0000111C, 0x000000E2, 0x0004007C, 0x00000023, 0x00001123, 0x00001122, 0x0004007C, 0x00000076,
    0x00001878, 0x00001123, 0x00040064, 0x0000006F, 0x00001879, 0x00000B95, 0x0007005F, 0x0000007A, 0x0000187A,
    0x00001879, 0x00001878, 0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001126, 0x0000111C, 0x000000F1,
    0x0004007C, 0x00000023, 0x00001127, 0x00001126, 0x0004007C, 0x00000076, 0x0000187F, 0x00001127, 0x00040064,
    0x0000006F, 0x00001880, 0x00000B95, 0x0007005F, 0x0000007A, 0x00001881, 0x00001880, 0x0000187F, 0x00000002,
    0x00000078, 0x0004007C, 0x00000023, 0x0000112A, 0x0000111C, 0x0004007C, 0x00000076, 0x00001886, 0x0000112A,
    0x00040064, 0x0000006F, 0x00001887, 0x00000B95, 0x0007005F, 0x0000007A, 0x00001888, 0x00001887, 0x00001886,
    0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x0000112D, 0x0000111C, 0x000000EA, 0x0004007C, 0x00000023,
    0x0000112E, 0x0000112D, 0x0004007C, 0x00000076, 0x0000188D, 0x0000112E, 0x00040064, 0x0000006F, 0x0000188E,
    0x00000B95, 0x0007005F, 0x0000007A, 0x0000188F, 0x0000188E, 0x0000188D, 0x00000002, 0x00000078, 0x00050080,
    0x00000076, 0x00001131, 0x0000111C, 0x00000383, 0x0004007C, 0x00000023, 0x00001132, 0x00001131, 0x0004007C,
    0x00000076, 0x00001894, 0x00001132, 0x00040064, 0x0000006F, 0x00001895, 0x00000B95, 0x0007005F, 0x0000007A,
    0x00001896, 0x00001895, 0x00001894, 0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001135, 0x0000111C,
    0x000000FD, 0x0004007C, 0x00000023, 0x00001136, 0x00001135, 0x0004007C, 0x00000076, 0x0000189B, 0x00001136,
    0x00040064, 0x0000006F, 0x0000189C, 0x00000B95, 0x0007005F, 0x0000007A, 0x0000189D, 0x0000189C, 0x0000189B,
    0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001139, 0x0000111C, 0x00000390, 0x0004007C, 0x00000023,
    0x0000113A, 0x00001139, 0x0004007C, 0x00000076, 0x000018A2, 0x0000113A, 0x00040064, 0x0000006F, 0x000018A3,
    0x00000B95, 0x0007005F, 0x0000007A, 0x000018A4, 0x000018A3, 0x000018A2, 0x00000002, 0x00000078, 0x00050080,
    0x00000076, 0x0000113D, 0x0000111C, 0x00000104, 0x0004007C, 0x00000023, 0x0000113E, 0x0000113D, 0x0004007C,
    0x00000076, 0x000018A9, 0x0000113E, 0x00040064, 0x0000006F, 0x000018AA, 0x00000B95, 0x0007005F, 0x0000007A,
    0x000018AB, 0x000018AA, 0x000018A9, 0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001141, 0x0000111C,
    0x0000010B, 0x0004007C, 0x00000023, 0x00001142, 0x00001141, 0x0004007C, 0x00000076, 0x000018B0, 0x00001142,
    0x00040064, 0x0000006F, 0x000018B1, 0x00000B95, 0x0007005F, 0x0000007A, 0x000018B2, 0x000018B1, 0x000018B0,
    0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001145, 0x0000111C, 0x000003A3, 0x0004007C, 0x00000023,
    0x00001146, 0x00001145, 0x0004007C, 0x00000076, 0x000018B7, 0x00001146, 0x00040064, 0x0000006F, 0x000018B8,
    0x00000B95, 0x0007005F, 0x0000007A, 0x000018B9, 0x000018B8, 0x000018B7, 0x00000002, 0x00000078, 0x00050080,
    0x00000076, 0x00001149, 0x0000111C, 0x000003AA, 0x0004007C, 0x00000023, 0x0000114A, 0x00001149, 0x0004007C,
    0x00000076, 0x000018BE, 0x0000114A, 0x00040064, 0x0000006F, 0x000018BF, 0x00000B95, 0x0007005F, 0x0000007A,
    0x000018C0, 0x000018BF, 0x000018BE, 0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x0000114D, 0x0000111C,
    0x00000112, 0x0004007C, 0x00000023, 0x0000114E, 0x0000114D, 0x0004007C, 0x00000076, 0x000018C5, 0x0000114E,
    0x00040064, 0x0000006F, 0x000018C6, 0x00000B95, 0x0007005F, 0x0000007A, 0x000018C7, 0x000018C6, 0x000018C5,
    0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001151, 0x0000111C, 0x000003B7, 0x0004007C, 0x00000023,
    0x00001152, 0x00001151, 0x0004007C, 0x00000076, 0x000018CC, 0x00001152, 0x00040064, 0x0000006F, 0x000018CD,
    0x00000B95, 0x0007005F, 0x0000007A, 0x000018CE, 0x000018CD, 0x000018CC, 0x00000002, 0x00000078, 0x00050080,
    0x00000076, 0x00001155, 0x0000111C, 0x000003BE, 0x0004007C, 0x00000023, 0x00001156, 0x00001155, 0x0004007C,
    0x00000076, 0x000018D3, 0x00001156, 0x00040064, 0x0000006F, 0x000018D4, 0x00000B95, 0x0007005F, 0x0000007A,
    0x000018D5, 0x000018D4, 0x000018D3, 0x00000002, 0x00000078, 0x00050080, 0x00000076, 0x00001159, 0x0000111C,
    0x000003C5, 0x0004007C, 0x00000023, 0x0000115A, 0x00001159, 0x0004007C, 0x00000076, 0x000018DA, 0x0000115A,
    0x00040064, 0x0000006F, 0x000018DB, 0x00000B95, 0x0007005F, 0x0000007A, 0x000018DC, 0x000018DB, 0x000018DA,
    0x00000002, 0x00000078, 0x00050051, 0x00000006, 0x0000115D, 0x00001873, 0x00000000, 0x00050051, 0x00000006,
    0x0000115F, 0x00001873, 0x00000001, 0x00050051, 0x00000006, 0x00001161, 0x00001873, 0x00000002, 0x00050051,
    0x00000006, 0x0000116A, 0x0000187A, 0x00000000, 0x00050051, 0x00000006, 0x0000116C, 0x0000187A, 0x00000001,
    0x00050051, 0x00000006, 0x0000116E, 0x0000187A, 0x00000002, 0x00050051, 0x00000006, 0x00001177, 0x0000188F,
    0x00000000, 0x00050051, 0x00000006, 0x00001179, 0x0000188F, 0x00000001, 0x00050051, 0x00000006, 0x0000117B,
    0x0000188F, 0x00000002, 0x00050051, 0x00000006, 0x00001184, 0x00001896, 0x00000000, 0x00050051, 0x00000006,
    0x00001186, 0x00001896, 0x00000001, 0x00050051, 0x00000006, 0x00001188, 0x00001896, 0x00000002, 0x00050051,
    0x00000006, 0x00001191, 0x00001881, 0x00000000, 0x00050051, 0x00000006, 0x00001193, 0x00001881, 0x00000001,
    0x00050051, 0x00000006, 0x00001195, 0x00001881, 0x00000002, 0x00050051, 0x00000006, 0x0000119E, 0x00001888,
    0x00000000, 0x00050051, 0x00000006, 0x000011A0, 0x00001888, 0x00000001, 0x00050051, 0x00000006, 0x000011A2,
    0x00001888, 0x00000002, 0x00050051, 0x00000006, 0x000011AB, 0x0000189D, 0x00000000, 0x00050051, 0x00000006,
    0x000011AD, 0x0000189D, 0x00000001, 0x00050051, 0x00000006, 0x000011AF, 0x0000189D, 0x00000002, 0x00050051,
    0x00000006, 0x000011B8, 0x000018A4, 0x00000000, 0x00050051, 0x00000006, 0x000011BA, 0x000018A4, 0x00000001,
    0x00050051, 0x00000006, 0x000011BC, 0x000018A4, 0x00000002, 0x00050051, 0x00000006, 0x000011C5, 0x000018AB,
    0x00000000, 0x00050051, 0x00000006, 0x000011C7, 0x000018AB, 0x00000001, 0x00050051, 0x00000006, 0x000011C9,
    0x000018AB, 0x00000002, 0x00050051, 0x00000006, 0x000011D2, 0x000018B2, 0x00000000, 0x00050051, 0x00000006,
    0x000011D4, 0x000018B2, 0x00000001, 0x00050051, 0x00000006, 0x000011D6, 0x000018B2, 0x00000002, 0x00050051,
    0x00000006, 0x000011DF, 0x000018C7, 0x00000000, 0x00050051, 0x00000006, 0x000011E1, 0x000018C7, 0x00000001,
    0x00050051, 0x00000006, 0x000011E3, 0x000018C7, 0x00000002, 0x00050051, 0x00000006, 0x000011EC, 0x000018CE,
    0x00000000, 0x00050051, 0x00000006, 0x000011EE, 0x000018CE, 0x00000001, 0x00050051, 0x00000006, 0x000011F0,
    0x000018CE, 0x00000002, 0x00050051, 0x00000006, 0x000011F9, 0x000018B9, 0x00000000, 0x00050051, 0x00000006,
    0x000011FB, 0x000018B9, 0x00000001, 0x00050051, 0x00000006, 0x000011FD, 0x000018B9, 0x00000002, 0x00050051,
    0x00000006, 0x00001206, 0x000018C0, 0x00000000, 0x00050051, 0x00000006, 0x00001208, 0x000018C0, 0x00000001,
    0x00050051, 0x00000006, 0x0000120A, 0x000018C0, 0x00000002, 0x00050051, 0x00000006, 0x00001213, 0x000018D5,
    0x00000000, 0x00050051, 0x00000006, 0x00001215, 0x000018D5, 0x00000001, 0x00050051, 0x00000006, 0x00001217,
    0x000018D5, 0x00000002, 0x00050051, 0x00000006, 0x00001220, 0x000018DC, 0x00000000, 0x00050051, 0x00000006,
    0x00001222, 0x000018DC, 0x00000001, 0x00050051, 0x00000006, 0x00001224, 0x000018DC, 0x00000002, 0x0007000C,
    0x00000006, 0x000018E2, 0x00000001, 0x00000025, 0x00001191, 0x0000119E, 0x0007000C, 0x00000006, 0x000018E3,
    0x00000001, 0x00000025, 0x0000116A, 0x000018E2, 0x0007000C, 0x00000006, 0x000018E8, 0x00000001, 0x00000025,
    0x000011AB, 0x000011D2, 0x0007000C, 0x00000006, 0x000018E9, 0x00000001, 0x00000025, 0x000018E3, 0x000018E8,
    0x0007000C, 0x00000006, 0x000018EE, 0x00000001, 0x00000025, 0x00001193, 0x000011A0, 0x0007000C, 0x00000006,
    0x000018EF, 0x00000001, 0x00000025, 0x0000116C, 0x000018EE, 0x0007000C, 0x00000006, 0x000018F4, 0x00000001,
    0x00000025, 0x000011AD, 0x000011D4, 0x0007000C, 0x00000006, 0x000018F5, 0x00000001, 0x00000025, 0x000018EF,
    0x000018F4, 0x0007000C, 0x00000006, 0x000018FA, 0x00000001, 0x00000025, 0x00001195, 0x000011A2, 0x0007000C,
    0x00000006, 0x000018FB, 0x00000001, 0x00000025, 0x0000116E, 0x000018FA, 0x0007000C, 0x00000006, 0x00001900,
    0x00000001, 0x00000025, 0x000011AF, 0x000011D6, 0x0007000C, 0x00000006, 0x00001901, 0x00000001, 0x00000025,
    0x000018FB, 0x00001900, 0x0007000C, 0x00000006, 0x00001906, 0x00000001, 0x00000025, 0x0000115D, 0x00001177,
    0x0007000C, 0x00000006, 0x00001907, 0x00000001, 0x00000025, 0x000018E9, 0x00001906, 0x0007000C, 0x00000006,
    0x0000190C, 0x00000001, 0x00000025, 0x000011C5, 0x000011DF, 0x0007000C, 0x00000006, 0x0000190D, 0x00000001,
    0x00000025, 0x00001907, 0x0000190C, 0x0007000C, 0x00000006, 0x00001912, 0x00000001, 0x00000025, 0x0000115F,
    0x00001179, 0x0007000C, 0x00000006, 0x00001913, 0x00000001, 0x00000025, 0x000018F5, 0x00001912, 0x0007000C,
    0x00000006, 0x00001918, 0x00000001, 0x00000025, 0x000011C7, 0x000011E1, 0x0007000C, 0x00000006, 0x00001919,
    0x00000001, 0x00000025, 0x00001913, 0x00001918, 0x0007000C, 0x00000006, 0x0000191E, 0x00000001, 0x00000025,
    0x00001161, 0x0000117B, 0x0007000C, 0x00000006, 0x0000191F, 0x00000001, 0x00000025, 0x00001901, 0x0000191E,
    0x0007000C, 0x00000006, 0x00001924, 0x00000001, 0x00000025, 0x000011C9, 0x000011E3, 0x0007000C, 0x00000006,
    0x00001925, 0x00000001, 0x00000025, 0x0000191F, 0x00001924, 0x00050081, 0x00000006, 0x00001273, 0x000018E9,
    0x0000190D, 0x00050081, 0x00000006, 0x00001276, 0x000018F5, 0x00001919, 0x00050081, 0x00000006, 0x00001279,
    0x00001901, 0x00001925, 0x0007000C, 0x00000006, 0x0000192A, 0x00000001, 0x00000028, 0x00001191, 0x0000119E,
    0x0007000C, 0x00000006, 0x0000192B, 0x00000001, 0x00000028, 0x0000116A, 0x0000192A, 0x0007000C, 0x00000006,
    0x00001930, 0x00000001, 0x00000028, 0x000011AB, 0x000011D2, 0x0007000C, 0x00000006, 0x00001931, 0x00000001,
    0x00000028, 0x0000192B, 0x00001930, 0x0007000C, 0x00000006, 0x00001936, 0x00000001, 0x00000028, 0x00001193,
    0x000011A0, 0x0007000C, 0x00000006, 0x00001937, 0x00000001, 0x00000028, 0x0000116C, 0x00001936, 0x0007000C,
    0x00000006, 0x0000193C, 0x00000001, 0x00000028, 0x000011AD, 0x000011D4, 0x0007000C, 0x00000006, 0x0000193D,
    0x00000001, 0x00000028, 0x00001937, 0x0000193C, 0x0007000C, 0x00000006, 0x00001942, 0x00000001, 0x00000028,
    0x00001195, 0x000011A2, 0x0007000C, 0x00000006, 0x00001943, 0x00000001, 0x00000028, 0x0000116E, 0x00001942,
    0x0007000C, 0x00000006, 0x00001948, 0x00000001, 0x00000028, 0x000011AF, 0x000011D6, 0x0007000C, 0x00000006,
    0x00001949, 0x00000001, 0x00000028, 0x00001943, 0x00001948, 0x0007000C, 0x00000006, 0x0000194E, 0x00000001,
    0x00000028, 0x0000115D, 0x00001177, 0x0007000C, 0x00000006, 0x0000194F, 0x00000001, 0x00000028, 0x00001931,
    0x0000194E, 0x0007000C, 0x00000006, 0x00001954, 0x00000001, 0x00000028, 0x000011C5, 0x000011DF, 0x0007000C,
    0x00000006, 0x00001955, 0x00000001, 0x00000028, 0x0000194F, 0x00001954, 0x0007000C, 0x00000006, 0x0000195A,
    0x00000001, 0x00000028, 0x0000115F, 0x00001179, 0x0007000C, 0x00000006, 0x0000195B, 0x00000001, 0x00000028,
    0x0000193D, 0x0000195A, 0x0007000C, 0x00000006, 0x00001960, 0x00000001, 0x00000028, 0x000011C7, 0x000011E1,
    0x0007000C, 0x00000006, 0x00001961, 0x00000001, 0x00000028, 0x0000195B, 0x00001960, 0x0007000C, 0x00000006,
    0x00001966, 0x00000001, 0x00000028, 0x00001161, 0x0000117B, 0x0007000C, 0x00000006, 0x00001967, 0x00000001,
    0x00000028, 0x00001949, 0x00001966, 0x0007000C, 0x00000006, 0x0000196C, 0x00000001, 0x00000028, 0x000011C9,
    0x000011E3, 0x0007000C, 0x00000006, 0x0000196D, 0x00000001, 0x00000028, 0x00001967, 0x0000196C, 0x00050081,
    0x00000006, 0x000012C1, 0x00001931, 0x00001955, 0x00050081, 0x00000006, 0x000012C4, 0x0000193D, 0x00001961,
    0x00050081, 0x00000006, 0x000012C7, 0x00001949, 0x0000196D, 0x0007000C, 0x00000006, 0x00001972, 0x00000001,
    0x00000025, 0x0000119E, 0x000011AB, 0x0007000C, 0x00000006, 0x00001973, 0x00000001, 0x00000025, 0x00001177,
    0x00001972, 0x0007000C, 0x00000006, 0x00001978, 0x00000001, 0x00000025, 0x000011B8, 0x000011DF, 0x0007000C,
    0x00000006, 0x00001979, 0x00000001, 0x00000025, 0x00001973, 0x00001978, 0x0007000C, 0x00000006, 0x0000197E,
    0x00000001, 0x00000025, 0x000011A0, 0x000011AD, 0x0007000C, 0x00000006, 0x0000197F, 0x00000001, 0x00000025,
    0x00001179, 0x0000197E, 0x0007000C, 0x00000006, 0x00001984, 0x00000001, 0x00000025, 0x000011BA, 0x000011E1,
    0x0007000C, 0x00000006, 0x00001985, 0x00000001, 0x00000025, 0x0000197F, 0x00001984, 0x0007000C, 0x00000006,
    0x0000198A, 0x00000001, 0x00000025, 0x000011A2, 0x000011AF, 0x0007000C, 0x00000006, 0x0000198B, 0x00000001,
    0x00000025, 0x0000117B, 0x0000198A, 0x0007000C, 0x00000006, 0x00001990, 0x00000001, 0x00000025, 0x000011BC,
    0x000011E3, 0x0007000C, 0x00000006, 0x00001991, 0x00000001, 0x00000025, 0x0000198B, 0x00001990, 0x0007000C,
    0x00000006, 0x00001996, 0x00000001, 0x00000025, 0x0000116A, 0x00001184, 0x0007000C, 0x00000006, 0x00001997,
    0x00000001, 0x00000025, 0x00001979, 0x00001996, 0x0007000C, 0x00000006, 0x0000199C, 0x00000001, 0x00000025,
    0x000011D2, 0x000011EC, 0x0007000C, 0x00000006, 0x0000199D, 0x00000001, 0x00000025, 0x00001997, 0x0000199C,
    0x0007000C, 0x00000006, 0x000019A2, 0x00000001, 0x00000025, 0x0000116C, 0x00001186, 0x0007000C, 0x00000006,
    0x000019A3, 0x00000001, 0x00000025, 0x00001985, 0x000019A2, 0x0007000C, 0x00000006, 0x000019A8, 0x00000001,
    0x00000025, 0x000011D4, 0x000011EE, 0x0007000C, 0x00000006, 0x000019A9, 0x00000001, 0x00000025, 0x000019A3,
    0x000019A8, 0x0007000C, 0x00000006, 0x000019AE, 0x00000001, 0x00000025, 0x0000116E, 0x00001188, 0x0007000C,
    0x00000006, 0x000019AF, 0x00000001, 0x00000025, 0x00001991, 0x000019AE, 0x0007000C, 0x00000006, 0x000019B4,
    0x00000001, 0x00000025, 0x000011D6, 0x000011F0, 0x0007000C, 0x00000006, 0x000019B5, 0x00000001, 0x00000025,
    0x000019AF, 0x000019B4, 0x00050081, 0x00000006, 0x0000130F, 0x00001979, 0x0000199D, 0x00050081, 0x00000006,
    0x00001312, 0x00001985, 0x000019A9, 0x00050081, 0x00000006, 0x00001315, 0x00001991, 0x000019B5, 0x0007000C,
    0x00000006, 0x000019BA, 0x00000001, 0x00000028, 0x0000119E, 0x000011AB, 0x0007000C, 0x00000006, 0x000019BB,
    0x00000001, 0x00000028, 0x00001177, 0x000019BA, 0x0007000C, 0x00000006, 0x000019C0, 0x00000001, 0x00000028,
    0x000011B8, 0x000011DF, 0x0007000C, 0x00000006, 0x000019C1, 0x00000001, 0x00000028, 0x000019BB, 0x000019C0,
    0x0007000C, 0x00000006, 0x000019C6, 0x00000001, 0x00000028, 0x000011A0, 0x000011AD, 0x0007000C, 0x00000006,
    0x000019C7, 0x00000001, 0x00000028, 0x00001179, 0x000019C6, 0x0007000C, 0x00000006, 0x000019CC, 0x00000001,
    0x00000028, 0x000011BA, 0x000011E1, 0x0007000C, 0x00000006, 0x000019CD, 0x00000001, 0x00000028, 0x000019C7,
    0x000019CC, 0x0007000C, 0x00000006, 0x000019D2, 0x00000001, 0x00000028, 0x000011A2, 0x000011AF, 0x0007000C,
    0x00000006, 0x000019D3, 0x00000001, 0x00000028, 0x0000117B, 0x000019D2, 0x0007000C, 0x00000006, 0x000019D8,
    0x00000001, 0x00000028, 0x000011BC, 0x000011E3, 0x0007000C, 0x00000006, 0x000019D9, 0x00000001, 0x00000028,
    0x000019D3, 0x000019D8, 0x0007000C, 0x00000006, 0x000019DE, 0x00000001, 0x00000028, 0x0000116A, 0x00001184,
    0x0007000C, 0x00000006, 0x000019DF, 0x00000001, 0x00000028, 0x000019C1, 0x000019DE, 0x0007000C, 0x00000006,
    0x000019E4, 0x00000001, 0x00000028, 0x000011D2, 0x000011EC, 0x0007000C, 0x00000006, 0x000019E5, 0x00000001,
    0x00000028, 0x000019DF, 0x000019E4, 0x0007000C, 0x00000006, 0x000019EA, 0x00000001, 0x00000028, 0x0000116C,
    0x00001186, 0x0007000C, 0x00000006, 0x000019EB, 0x00000001, 0x00000028, 0x000019CD, 0x000019EA, 0x0007000C,
    0x00000006, 0x000019F0, 0x00000001, 0x00000028, 0x000011D4, 0x000011EE, 0x0007000C, 0x00000006, 0x000019F1,
    0x00000001, 0x00000028, 0x000019EB, 0x000019F0, 0x0007000C, 0x00000006, 0x000019F6, 0x00000001, 0x00000028,
    0x0000116E, 0x00001188, 0x0007000C, 0x00000006, 0x000019F7, 0x00000001, 0x00000028, 0x000019D9, 0x000019F6,
    0x0007000C, 0x00000006, 0x000019FC, 0x00000001, 0x00000028, 0x000011D6, 0x000011F0, 0x0007000C, 0x00000006,
    0x000019FD, 0x00000001, 0x00000028, 0x000019F7, 0x000019FC, 0x00050081, 0x00000006, 0x0000135D, 0x000019C1,
    0x000019E5, 0x00050081, 0x00000006, 0x00001360, 0x000019CD, 0x000019F1, 0x00050081, 0x00000006, 0x00001363,
    0x000019D9, 0x000019FD, 0x0007000C, 0x00000006, 0x00001A02, 0x00000001, 0x00000025, 0x000011C5, 0x000011D2,
    0x0007000C, 0x00000006, 0x00001A03, 0x00000001, 0x00000025, 0x0000119E, 0x00001A02, 0x0007000C, 0x00000006,
    0x00001A08, 0x00000001, 0x00000025, 0x000011DF, 0x00001206, 0x0007000C, 0x00000006, 0x00001A09, 0x00000001,
    0x00000025, 0x00001A03, 0x00001A08, 0x0007000C, 0x00000006, 0x00001A0E, 0x00000001, 0x00000025, 0x000011C7,
    0x000011D4, 0x0007000C, 0x00000006, 0x00001A0F, 0x00000001, 0x00000025, 0x000011A0, 0x00001A0E, 0x0007000C,
    0x00000006, 0x00001A14, 0x00000001, 0x00000025, 0x000011E1, 0x00001208, 0x0007000C, 0x00000006, 0x00001A15,
    0x00000001, 0x00000025, 0x00001A0F, 0x00001A14, 0x0007000C, 0x00000006, 0x00001A1A, 0x00000001, 0x00000025,
    0x000011C9, 0x000011D6, 0x0007000C, 0x00000006, 0x00001A1B, 0x00000001, 0x00000025, 0x000011A2, 0x00001A1A,
    0x0007000C, 0x00000006, 0x00001A20, 0x00000001, 0x00000025, 0x000011E3, 0x0000120A, 0x0007000C, 0x00000006,
    0x00001A21, 0x00000001, 0x00000025, 0x00001A1B, 0x00001A20, 0x0007000C, 0x00000006, 0x00001A26, 0x00000001,
    0x00000025, 0x00001191, 0x000011AB, 0x0007000C, 0x00000006, 0x00001A27, 0x00000001, 0x00000025, 0x00001A09,
    0x00001A26, 0x0007000C, 0x00000006, 0x00001A2C, 0x00000001, 0x00000025, 0x000011F9, 0x00001213, 0x0007000C,
    0x00000006, 0x00001A2D, 0x00000001, 0x00000025, 0x00001A27, 0x00001A2C, 0x0007000C, 0x00000006, 0x00001A32,
    0x00000001, 0x00000025, 0x00001193, 0x000011AD, 0x0007000C, 0x00000006, 0x00001A33, 0x00000001, 0x00000025,
    0x00001A15, 0x00001A32, 0x0007000C, 0x00000006, 0x00001A38, 0x00000001, 0x00000025, 0x000011FB, 0x00001215,
    0x0007000C, 0x00000006, 0x00001A39, 0x00000001, 0x00000025, 0x00001A33, 0x00001A38, 0x0007000C, 0x00000006,
    0x00001A3E, 0x00000001, 0x00000025, 0x00001195, 0x000011AF, 0x0007000C, 0x00000006, 0x00001A3F, 0x00000001,
    0x00000025, 0x00001A21, 0x00001A3E, 0x0007000C, 0x00000006, 0x00001A44, 0x00000001, 0x00000025, 0x000011FD,
    0x00001217, 0x0007000C, 0x00000006, 0x00001A45, 0x00000001, 0x00000025, 0x00001A3F, 0x00001A44, 0x00050081,
    0x00000006, 0x000013AB, 0x00001A09, 0x00001A2D, 0x00050081, 0x00000006, 0x000013AE, 0x00001A15, 0x00001A39,
    0x00050081, 0x00000006, 0x000013B1, 0x00001A21, 0x00001A45, 0x0007000C, 0x00000006, 0x00001A4A, 0x00000001,
    0x00000028, 0x000011C5, 0x000011D2, 0x0007000C, 0x00000006, 0x00001A4B, 0x00000001, 0x00000028, 0x0000119E,
    0x00001A4A, 0x0007000C, 0x00000006, 0x00001A50, 0x00000001, 0x00000028, 0x000011DF, 0x00001206, 0x0007000C,
    0x00000006, 0x00001A51, 0x00000001, 0x00000028, 0x00001A4B, 0x00001A50, 0x0007000C, 0x00000006, 0x00001A56,
    0x00000001, 0x00000028, 0x000011C7, 0x000011D4, 0x0007000C, 0x00000006, 0x00001A57, 0x00000001, 0x00000028,
    0x000011A0, 0x00001A56, 0x0007000C, 0x00000006, 0x00001A5C, 0x00000001, 0x00000028, 0x000011E1, 0x00001208,
    0x0007000C, 0x00000006, 0x00001A5D, 0x00000001, 0x00000028, 0x00001A57, 0x00001A5C, 0x0007000C, 0x00000006,
    0x00001A62, 0x00000001, 0x00000028, 0x000011C9, 0x000011D6, 0x0007000C, 0x00000006, 0x00001A63, 0x00000001,
    0x00000028, 0x000011A2, 0x00001A62, 0x0007000C, 0x00000006, 0x00001A68, 0x00000001, 0x00000028, 0x000011E3,
    0x0000120A, 0x0007000C, 0x00000006, 0x00001A69, 0x00000001, 0x00000028, 0x00001A63, 0x00001A68, 0x0007000C,
    0x00000006, 0x00001A6E, 0x00000001, 0x00000028, 0x00001191, 0x000011AB, 0x0007000C, 0x00000006, 0x00001A6F,
    0x00000001, 0x00000028, 0x00001A51, 0x00001A6E, 0x0007000C, 0x00000006, 0x00001A74, 0x00000001, 0x00000028,
    0x000011F9, 0x00001213, 0x0007000C, 0x00000006, 0x00001A75, 0x00000001, 0x00000028, 0x00001A6F, 0x00001A74,
    0x0007000C, 0x00000006, 0x00001A7A, 0x00000001, 0x00000028, 0x00001193, 0x000011AD, 0x0007000C, 0x00000006,
    0x00001A7B, 0x00000001, 0x00000028, 0x00001A5D, 0x00001A7A, 0x0007000C, 0x00000006, 0x00001A80, 0x00000001,
    0x00000028, 0x000011FB, 0x00001215, 0x0007000C, 0x00000006, 0x00001A81, 0x00000001, 0x00000028, 0x00001A7B,
    0x00001A80, 0x0007000C, 0x00000006, 0x00001A86, 0x00000001, 0x00000028, 0x00001195, 0x000011AF, 0x0007000C,
    0x00000006, 0x00001A87, 0x00000001, 0x00000028, 0x00001A69, 0x00001A86, 0x0007000C, 0x00000006, 0x00001A8C,
    0x00000001, 0x00000028, 0x000011FD, 0x00001217, 0x0007000C, 0x00000006, 0x00001A8D, 0x00000001, 0x00000028,
    0x00001A87, 0x00001A8C, 0x00050081, 0x00000006, 0x000013F9, 0x00001A51, 0x00001A75, 0x00050081, 0x00000006,
    0x000013FC, 0x00001A5D, 0x00001A81, 0x00050081, 0x00000006, 0x000013FF, 0x00001A69, 0x00001A8D, 0x0007000C,
    0x00000006, 0x00001A92, 0x00000001, 0x00000025, 0x000011D2, 0x000011DF, 0x0007000C, 0x00000006, 0x00001A93,
    0x00000001, 0x00000025, 0x000011AB, 0x00001A92, 0x0007000C, 0x00000006, 0x00001A98, 0x00000001, 0x00000025,
    0x000011EC, 0x00001213, 0x0007000C, 0x00000006, 0x00001A99, 0x00000001, 0x00000025, 0x00001A93, 0x00001A98,
    0x0007000C, 0x00000006, 0x00001A9E, 0x00000001, 0x00000025, 0x000011D4, 0x000011E1, 0x0007000C, 0x00000006,
    0x00001A9F, 0x00000001, 0x00000025, 0x000011AD, 0x00001A9E, 0x0007000C, 0x00000006, 0x00001AA4, 0x00000001,
    0x00000025, 0x000011EE, 0x00001215, 0x0007000C, 0x00000006, 0x00001AA5, 0x00000001, 0x00000025, 0x00001A9F,
    0x00001AA4, 0x0007000C, 0x00000006, 0x00001AAA, 0x00000001, 0x00000025, 0x000011D6, 0x000011E3, 0x0007000C,
    0x00000006, 0x00001AAB, 0x00000001, 0x00000025, 0x000011AF, 0x00001AAA, 0x0007000C, 0x00000006, 0x00001AB0,
    0x00000001, 0x00000025, 0x000011F0, 0x00001217, 0x0007000C, 0x00000006, 0x00001AB1, 0x00000001, 0x00000025,
    0x00001AAB, 0x00001AB0, 0x0007000C, 0x00000006, 0x00001AB6, 0x00000001, 0x00000025, 0x0000119E, 0x000011B8,
    0x0007000C, 0x00000006, 0x00001AB7, 0x00000001, 0x00000025, 0x00001A99, 0x00001AB6, 0x0007000C, 0x00000006,
    0x00001ABC, 0x00000001, 0x00000025, 0x00001206, 0x00001220, 0x0007000C, 0x00000006, 0x00001ABD, 0x00000001,
    0x00000025, 0x00001AB7, 0x00001ABC, 0x0007000C, 0x00000006, 0x00001AC2, 0x00000001, 0x00000025, 0x000011A0,
    0x000011BA, 0x0007000C, 0x00000006, 0x00001AC3, 0x00000001, 0x00000025, 0x00001AA5, 0x00001AC2, 0x0007000C,
    0x00000006, 0x00001AC8, 0x00000001, 0x00000025, 0x00001208, 0x00001222, 0x0007000C, 0x00000006, 0x00001AC9,
    0x00000001, 0x00000025, 0x00001AC3, 0x00001AC8, 0x0007000C, 0x00000006, 0x00001ACE, 0x00000001, 0x00000025,
    0x000011A2, 0x000011BC, 0x0007000C, 0x00000006, 0x00001ACF, 0x00000001, 0x00000025, 0x00001AB1, 0x00001ACE,
    0x0007000C, 0x00000006, 0x00001AD4, 0x00000001, 0x00000025, 0x0000120A, 0x00001224, 0x0007000C, 0x00000006,
    0x00001AD5, 0x00000001, 0x00000025, 0x00001ACF, 0x00001AD4, 0x00050081, 0x00000006, 0x00001447, 0x00001A99,
    0x00001ABD, 0x00050081, 0x00000006, 0x0000144A, 0x00001AA5, 0x00001AC9, 0x00050081, 0x00000006, 0x0000144D,
    0x00001AB1, 0x00001AD5, 0x0007000C, 0x00000006, 0x00001ADA, 0x00000001, 0x00000028, 0x000011D2, 0x000011DF,
    0x0007000C, 0x00000006, 0x00001ADB, 0x00000001, 0x00000028, 0x000011AB, 0x00001ADA, 0x0007000C, 0x00000006,
    0x00001AE0, 0x00000001, 0x00000028, 0x000011EC, 0x00001213, 0x0007000C, 0x00000006, 0x00001AE1, 0x00000001,
    0x00000028, 0x00001ADB, 0x00001AE0, 0x0007000C, 0x00000006, 0x00001AE6, 0x00000001, 0x00000028, 0x000011D4,
    0x000011E1, 0x0007000C, 0x00000006, 0x00001AE7, 0x00000001, 0x00000028, 0x000011AD, 0x00001AE6, 0x0007000C,
    0x00000006, 0x00001AEC, 0x00000001, 0x00000028, 0x000011EE, 0x00001215, 0x0007000C, 0x00000006, 0x00001AED,
    0x00000001, 0x00000028, 0x00001AE7, 0x00001AEC, 0x0007000C, 0x00000006, 0x00001AF2, 0x00000001, 0x00000028,
    0x000011D6, 0x000011E3, 0x0007000C, 0x00000006, 0x00001AF3, 0x00000001, 0x00000028, 0x000011AF, 0x00001AF2,
    0x0007000C, 0x00000006, 0x00001AF8, 0x00000001, 0x00000028, 0x000011F0, 0x00001217, 0x0007000C, 0x00000006,
    0x00001AF9, 0x00000001, 0x00000028, 0x00001AF3, 0x00001AF8, 0x0007000C, 0x00000006, 0x00001AFE, 0x00000001,
    0x00000028, 0x0000119E, 0x000011B8, 0x0007000C, 0x00000006, 0x00001AFF, 0x00000001, 0x00000028, 0x00001AE1,
    0x00001AFE, 0x0007000C, 0x00000006, 0x00001B04, 0x00000001, 0x00000028, 0x00001206, 0x00001220, 0x0007000C,
    0x00000006, 0x00001B05, 0x00000001, 0x00000028, 0x00001AFF, 0x00001B04, 0x0007000C, 0x00000006, 0x00001B0A,
    0x00000001, 0x00000028, 0x000011A0, 0x000011BA, 0x0007000C, 0x00000006, 0x00001B0B, 0x00000001, 0x00000028,
    0x00001AED, 0x00001B0A, 0x0007000C, 0x00000006, 0x00001B10, 0x00000001, 0x00000028, 0x00001208, 0x00001222,
    0x0007000C, 0x00000006, 0x00001B11, 0x00000001, 0x00000028, 0x00001B0B, 0x00001B10, 0x0007000C, 0x00000006,
    0x00001B16, 0x00000001, 0x00000028, 0x000011A2, 0x000011BC, 0x0007000C, 0x00000006, 0x00001B17, 0x00000001,
    0x00000028, 0x00001AF9, 0x00001B16, 0x0007000C, 0x00000006, 0x00001B1C, 0x00000001, 0x00000028, 0x0000120A,
    0x00001224, 0x0007000C, 0x00000006, 0x00001B1D, 0x00000001, 0x00000028, 0x00001B17, 0x00001B1C, 0x00050081,
    0x00000006, 0x00001495, 0x00001AE1, 0x00001B05, 0x00050081, 0x00000006, 0x00001498, 0x00001AED, 0x00001B11,
    0x00050081, 0x00000006, 0x0000149B, 0x00001AF9, 0x00001B1D, 0x00050088, 0x00000006, 0x00001B22, 0x0000005F,
    0x000012C1, 0x00050088, 0x00000006, 0x00001B29, 0x0000005F, 0x000012C4, 0x00050088, 0x00000006, 0x00001B30,
    0x0000005F, 0x000012C7, 0x00050088, 0x00000006, 0x00001B37, 0x0000005F, 0x0000135D, 0x00050088, 0x00000006,
    0x00001B3E, 0x0000005F, 0x00001360, 0x00050088, 0x00000006, 0x00001B45, 0x0000005F, 0x00001363, 0x00050088,
    0x00000006, 0x00001B4C, 0x0000005F, 0x000013F9, 0x00050088, 0x00000006, 0x00001B53, 0x0000005F, 0x000013FC,
    0x00050088, 0x00000006, 0x00001B5A, 0x0000005F, 0x000013FF, 0x00050088, 0x00000006, 0x00001B61, 0x0000005F,
    0x00001495, 0x00050088, 0x00000006, 0x00001B68, 0x0000005F, 0x00001498, 0x00050088, 0x00000006, 0x00001B6F,
    0x0000005F, 0x0000149B, 0x00050083, 0x00000006, 0x000014B7, 0x000002A5, 0x000012C1, 0x0007000C, 0x00000006,
    0x000014B8, 0x00000001, 0x00000025, 0x00001273, 0x000014B7, 0x00050085, 0x00000006, 0x000014BA, 0x000014B8,
    0x00001B22, 0x0008000C, 0x00000006, 0x00001B7A, 0x00000001, 0x0000002B, 0x000014BA, 0x00000067, 0x0000005F,
    0x00050083, 0x00000006, 0x000014BF, 0x000002A5, 0x000012C4, 0x0007000C, 0x00000006, 0x000014C0, 0x00000001,
    0x00000025, 0x00001276, 0x000014BF, 0x00050085, 0x00000006, 0x000014C2, 0x000014C0, 0x00001B29, 0x0008000C,
    0x00000006, 0x00001B87, 0x00000001, 0x0000002B, 0x000014C2, 0x00000067, 0x0000005F, 0x00050083, 0x00000006,
    0x000014C7, 0x000002A5, 0x000012C7, 0x0007000C, 0x00000006, 0x000014C8, 0x00000001, 0x00000025, 0x00001279,
    0x000014C7, 0x00050085, 0x00000006, 0x000014CA, 0x000014C8, 0x00001B30, 0x0008000C, 0x00000006, 0x00001B94,
    0x00000001, 0x0000002B, 0x000014CA, 0x00000067, 0x0000005F, 0x00050083, 0x00000006, 0x000014CF, 0x000002A5,
    0x0000135D, 0x0007000C, 0x00000006, 0x000014D0, 0x00000001, 0x00000025, 0x0000130F, 0x000014CF, 0x00050085,
    0x00000006, 0x000014D2, 0x000014D0, 0x00001B37, 0x0008000C, 0x00000006, 0x00001BA1, 0x00000001, 0x0000002B,
    0x000014D2, 0x00000067, 0x0000005F, 0x00050083, 0x00000006, 0x000014D7, 0x000002A5, 0x00001360, 0x0007000C,
    0x00000006, 0x000014D8, 0x00000001, 0x00000025, 0x00001312, 0x000014D7, 0x00050085, 0x00000006, 0x000014DA,
    0x000014D8, 0x00001B3E, 0x0008000C, 0x00000006, 0x00001BAE, 0x00000001, 0x0000002B, 0x000014DA, 0x00000067,
    0x0000005F, 0x00050083, 0x00000006, 0x000014DF, 0x000002A5, 0x00001363, 0x0007000C, 0x00000006, 0x000014E0,
    0x00000001, 0x00000025, 0x00001315, 0x000014DF, 0x00050085, 0x00000006, 0x000014E2, 0x000014E0, 0x00001B45,
    0x0008000C, 0x00000006, 0x00001BBB, 0x00000001, 0x0000002B, 0x000014E2, 0x00000067, 0x0000005F, 0x00050083,
    0x00000006, 0x000014E7, 0x000002A5, 0x000013F9, 0x0007000C, 0x00000006, 0x000014E8, 0x00000001, 0x00000025,
    0x000013AB, 0x000014E7, 0x00050085, 0x00000006, 0x000014EA, 0x000014E8, 0x00001B4C, 0x0008000C, 0x00000006,
    0x00001BC8, 0x00000001, 0x0000002B, 0x000014EA, 0x00000067, 0x0000005F, 0x00050083, 0x00000006, 0x000014EF,
    0x000002A5, 0x000013FC, 0x0007000C, 0x00000006, 0x000014F0, 0x00000001, 0x00000025, 0x000013AE, 0x000014EF,
    0x00050085, 0x00000006, 0x000014F2, 0x000014F0, 0x00001B53, 0x0008000C, 0x00000006, 0x00001BD5, 0x00000001,
    0x0000002B, 0x000014F2, 0x00000067, 0x0000005F, 0x00050083, 0x00000006, 0x000014F7, 0x000002A5, 0x000013FF,
    0x0007000C, 0x00000006, 0x000014F8, 0x00000001, 0x00000025, 0x000013B1, 0x000014F7, 0x00050085, 0x00000006,
    0x000014FA, 0x000014F8, 0x00001B5A, 0x0008000C, 0x00000006, 0x00001BE2, 0x00000001, 0x0000002B, 0x000014FA,
    0x00000067, 0x0000005F, 0x00050083, 0x00000006, 0x000014FF, 0x000002A5, 0x00001495, 0x0007000C, 0x00000006,
    0x00001500, 0x00000001, 0x00000025, 0x00001447, 0x000014FF, 0x00050085, 0x00000006, 0x00001502, 0x00001500,
    0x00001B61, 0x0008000C, 0x00000006, 0x00001BEF, 0x00000001, 0x0000002B, 0x00001502, 0x00000067, 0x0000005F,
    0x00050083, 0x00000006, 0x00001507, 0x000002A5, 0x00001498, 0x0007000C, 0x00000006, 0x00001508, 0x00000001,
    0x00000025, 0x0000144A, 0x00001507, 0x00050085, 0x00000006, 0x0000150A, 0x00001508, 0x00001B68, 0x0008000C,
    0x00000006, 0x00001BFC, 0x00000001, 0x0000002B, 0x0000150A, 0x00000067, 0x0000005F, 0x00050083, 0x00000006,
    0x0000150F, 0x000002A5, 0x0000149B, 0x0007000C, 0x00000006, 0x00001510, 0x00000001, 0x00000025, 0x0000144D,
    0x0000150F, 0x00050085, 0x00000006, 0x00001512, 0x00001510, 0x00001B6F, 0x0008000C, 0x00000006, 0x00001C09,
    0x00000001, 0x0000002B, 0x00001512, 0x00000067, 0x0000005F, 0x0006000C, 0x00000006, 0x00001515, 0x00000001,
    0x0000001F, 0x00001B7A, 0x0006000C, 0x00000006, 0x00001517, 0x00000001, 0x0000001F, 0x00001B87, 0x0006000C,
    0x00000006, 0x00001519, 0x00000001, 0x0000001F, 0x00001B94, 0x0006000C, 0x00000006, 0x0000151B, 0x00000001,
    0x0000001F, 0x00001BA1, 0x0006000C, 0x00000006, 0x0000151D, 0x00000001, 0x0000001F, 0x00001BAE, 0x0006000C,
    0x00000006, 0x0000151F, 0x00000001, 0x0000001F, 0x00001BBB, 0x0006000C, 0x00000006, 0x00001521, 0x00000001,
    0x0000001F, 0x00001BC8, 0x0006000C, 0x00000006, 0x00001523, 0x00000001, 0x0000001F, 0x00001BD5, 0x0006000C,
    0x00000006, 0x00001525, 0x00000001, 0x0000001F, 0x00001BE2, 0x0006000C, 0x00000006, 0x00001527, 0x00000001,
    0x0000001F, 0x00001BEF, 0x0006000C, 0x00000006, 0x00001529, 0x00000001, 0x0000001F, 0x00001BFC, 0x0006000C,
    0x00000006, 0x0000152B, 0x00000001, 0x0000001F, 0x00001C09, 0x0004007C, 0x00000006, 0x0000152E, 0x00000C1F,
    0x00050085, 0x00000006, 0x00001531, 0x00001515, 0x0000152E, 0x00050085, 0x00000006, 0x00001534, 0x00001517,
    0x0000152E, 0x00050085, 0x00000006, 0x00001537, 0x00001519, 0x0000152E, 0x00050085, 0x00000006, 0x0000153A,
    0x0000151B, 0x0000152E, 0x00050085, 0x00000006, 0x0000153D, 0x0000151D, 0x0000152E, 0x00050085, 0x00000006,
    0x00001540, 0x0000151F, 0x0000152E, 0x00050085, 0x00000006, 0x00001543, 0x00001521, 0x0000152E, 0x00050085,
    0x00000006, 0x00001546, 0x00001523, 0x0000152E, 0x00050085, 0x00000006, 0x00001549, 0x00001525, 0x0000152E,
    0x00050085, 0x00000006, 0x0000154C, 0x00001527, 0x0000152E, 0x00050085, 0x00000006, 0x0000154F, 0x00001529,
    0x0000152E, 0x00050085, 0x00000006, 0x00001552, 0x0000152B, 0x0000152E, 0x00050051, 0x00000006, 0x00001555,
    0x0000111A, 0x00000000, 0x00050083, 0x00000006, 0x00001556, 0x0000005F, 0x00001555, 0x00050051, 0x00000006,
    0x00001559, 0x0000111A, 0x00000001, 0x00050083, 0x00000006, 0x0000155A, 0x0000005F, 0x00001559, 0x00050085,
    0x00000006, 0x0000155B, 0x00001556, 0x0000155A, 0x00050085, 0x00000006, 0x00001562, 0x00001555, 0x0000155A,
    0x00050085, 0x00000006, 0x00001569, 0x00001556, 0x00001559, 0x00050085, 0x00000006, 0x0000156E, 0x00001555,
    0x00001559, 0x00050083, 0x00000006, 0x00001572, 0x000012C4, 0x00001276, 0x00050081, 0x00000006, 0x00001573,
    0x000009AF, 0x00001572, 0x00050088, 0x00000006, 0x00001C1A, 0x0000005F, 0x00001573, 0x00050085, 0x00000006,
    0x00001576, 0x0000155B, 0x00001C1A, 0x00050083, 0x00000006, 0x0000157A, 0x00001360, 0x00001312, 0x00050081,
    0x00000006, 0x0000157B, 0x000009AF, 0x0000157A, 0x00050088, 0x00000006, 0x00001C21, 0x0000005F, 0x0000157B,
    0x00050085, 0x00000006, 0x0000157E, 0x00001562, 0x00001C21, 0x00050083, 0x00000006, 0x00001582, 0x000013FC,
    0x000013AE, 0x00050081, 0x00000006, 0x00001583, 0x000009AF, 0x00001582, 0x00050088, 0x00000006, 0x00001C28,
    0x0000005F, 0x00001583, 0x00050085, 0x00000006, 0x00001586, 0x00001569, 0x00001C28, 0x00050083, 0x00000006,
    0x0000158A, 0x00001498, 0x0000144A, 0x00050081, 0x00000006, 0x0000158B, 0x000009AF, 0x0000158A, 0x00050088,
    0x00000006, 0x00001C2F, 0x0000005F, 0x0000158B, 0x00050085, 0x00000006, 0x0000158E, 0x0000156E, 0x00001C2F,
    0x00050085, 0x00000006, 0x00001591, 0x00001531, 0x00001576, 0x00050085, 0x00000006, 0x00001594, 0x00001534,
    0x00001576, 0x00050085, 0x00000006, 0x00001597, 0x00001537, 0x00001576, 0x00050085, 0x00000006, 0x0000159A,
    0x0000153A, 0x0000157E, 0x00050085, 0x00000006, 0x0000159D, 0x0000153D, 0x0000157E, 0x00050085, 0x00000006,
    0x000015A0, 0x00001540, 0x0000157E, 0x00050085, 0x00000006, 0x000015A6, 0x00001543, 0x00001586, 0x00050081,
    0x00000006, 0x000015A7, 0x0000159A, 0x000015A6, 0x00050081, 0x00000006, 0x000015A9, 0x000015A7, 0x00001576,
    0x00050085, 0x00000006, 0x000015AF, 0x00001546, 0x00001586, 0x00050081, 0x00000006, 0x000015B0, 0x0000159D,
    0x000015AF, 0x00050081, 0x00000006, 0x000015B2, 0x000015B0, 0x00001576, 0x00050085, 0x00000006, 0x000015B8,
    0x00001549, 0x00001586, 0x00050081, 0x00000006, 0x000015B9, 0x000015A0, 0x000015B8, 0x00050081, 0x00000006,
    0x000015BB, 0x000015B9, 0x00001576, 0x00050085, 0x00000006, 0x000015C1, 0x0000154C, 0x0000158E, 0x00050081,
    0x00000006, 0x000015C2, 0x00001591, 0x000015C1, 0x00050081, 0x00000006, 0x000015C4, 0x000015C2, 0x0000157E,
    0x00050085, 0x00000006, 0x000015CA, 0x0000154F, 0x0000158E, 0x00050081, 0x00000006, 0x000015CB, 0x00001594,
    0x000015CA, 0x00050081, 0x00000006, 0x000015CD, 0x000015CB, 0x0000157E, 0x00050085, 0x00000006, 0x000015D3,
    0x00001552, 0x0000158E, 0x00050081, 0x00000006, 0x000015D4, 0x00001597, 0x000015D3, 0x00050081, 0x00000006,
    0x000015D6, 0x000015D4, 0x0000157E, 0x00050081, 0x00000006, 0x000015DF, 0x000015C2, 0x00001586, 0x00050081,
    0x00000006, 0x000015E8, 0x000015CB, 0x00001586, 0x00050081, 0x00000006, 0x000015F1, 0x000015D4, 0x00001586,
    0x00050081, 0x00000006, 0x000015FA, 0x000015A7, 0x0000158E, 0x00050081, 0x00000006, 0x00001603, 0x000015B0,
    0x0000158E, 0x00050081, 0x00000006, 0x0000160C, 0x000015B9, 0x0000158E, 0x00050081, 0x00000006, 0x00001ECC,
    0x00001591, 0x0000159A, 0x00050081, 0x00000006, 0x00001ECD, 0x00001ECC, 0x000015A6, 0x00050081, 0x00000006,
    0x00001ECE, 0x00001ECD, 0x000015C1, 0x00050085, 0x00000006, 0x0000162D, 0x000002A5, 0x00001ECE, 0x00050081,
    0x00000006, 0x0000162F, 0x0000162D, 0x000015A9, 0x00050081, 0x00000006, 0x00001631, 0x0000162F, 0x000015C4,
    0x00050081, 0x00000006, 0x00001633, 0x00001631, 0x000015DF, 0x00050081, 0x00000006, 0x00001635, 0x00001633,
    0x000015FA, 0x00050088, 0x00000006, 0x00001C3E, 0x0000005F, 0x00001635, 0x00050081, 0x00000006, 0x00001ECF,
    0x00001594, 0x0000159D, 0x00050081, 0x00000006, 0x00001ED0, 0x00001ECF, 0x000015AF, 0x00050081, 0x00000006,
    0x00001ED1, 0x00001ED0, 0x000015CA, 0x00050085, 0x00000006, 0x00001645, 0x000002A5, 0x00001ED1, 0x00050081,
    0x00000006, 0x00001647, 0x00001645, 0x000015B2, 0x00050081, 0x00000006, 0x00001649, 0x00001647, 0x000015CD,
    0x00050081, 0x00000006, 0x0000164B, 0x00001649, 0x000015E8, 0x00050081, 0x00000006, 0x0000164D, 0x0000164B,
    0x00001603, 0x00050088, 0x00000006, 0x00001C4D, 0x0000005F, 0x0000164D, 0x00050081, 0x00000006, 0x00001ED2,
    0x00001597, 0x000015A0, 0x00050081, 0x00000006, 0x00001ED3, 0x00001ED2, 0x000015B8, 0x00050081, 0x00000006,
    0x00001ED4, 0x00001ED3, 0x000015D3, 0x00050085, 0x00000006, 0x0000165D, 0x000002A5, 0x00001ED4, 0x00050081,
    0x00000006, 0x0000165F, 0x0000165D, 0x000015BB, 0x00050081, 0x00000006, 0x00001661, 0x0000165F, 0x000015D6,
    0x00050081, 0x00000006, 0x00001663, 0x00001661, 0x000015F1, 0x00050081, 0x00000006, 0x00001665, 0x00001663,
    0x0000160C, 0x00050088, 0x00000006, 0x00001C5C, 0x0000005F, 0x00001665, 0x00050081, 0x00000006, 0x00001ED5,
    0x0000116A, 0x00001191, 0x00050085, 0x00000006, 0x0000166F, 0x00001591, 0x00001ED5, 0x00050085, 0x00000006,
    0x00001673, 0x00001177, 0x0000159A, 0x00050081, 0x00000006, 0x00001674, 0x0000166F, 0x00001673, 0x00050085,
    0x00000006, 0x00001678, 0x000011B8, 0x0000159A, 0x00050081, 0x00000006, 0x00001679, 0x00001674, 0x00001678,
    0x00050085, 0x00000006, 0x0000167D, 0x000011C5, 0x000015A6, 0x00050081, 0x00000006, 0x0000167E, 0x00001679,
    0x0000167D, 0x00050085, 0x00000006, 0x00001682, 0x00001206, 0x000015A6, 0x00050081, 0x00000006, 0x00001683,
    0x0000167E, 0x00001682, 0x00050085, 0x00000006, 0x00001687, 0x000011EC, 0x000015C1, 0x00050081, 0x00000006,
    0x00001688, 0x00001683, 0x00001687, 0x00050085, 0x00000006, 0x0000168C, 0x00001213, 0x000015C1, 0x00050081,
    0x00000006, 0x0000168D, 0x00001688, 0x0000168C, 0x00050085, 0x00000006, 0x00001691, 0x0000119E, 0x000015A9,
    0x00050081, 0x00000006, 0x00001692, 0x0000168D, 0x00001691, 0x00050085, 0x00000006, 0x00001696, 0x000011AB,
    0x000015C4, 0x00050081, 0x00000006, 0x00001697, 0x00001692, 0x00001696, 0x00050085, 0x00000006, 0x0000169B,
    0x000011D2, 0x000015DF, 0x00050081, 0x00000006, 0x0000169C, 0x00001697, 0x0000169B, 0x00050085, 0x00000006,
    0x000016A0, 0x000011DF, 0x000015FA, 0x00050081, 0x00000006, 0x000016A1, 0x0000169C, 0x000016A0, 0x00050085,
    0x00000006, 0x000016A3, 0x000016A1, 0x00001C3E, 0x0008000C, 0x00000006, 0x00001C65, 0x00000001, 0x0000002B,
    0x000016A3, 0x00000067, 0x0000005F, 0x00050081, 0x00000006, 0x00001ED6, 0x0000116C, 0x00001193, 0x00050085,
    0x00000006, 0x000016AD, 0x00001594, 0x00001ED6, 0x00050085, 0x00000006, 0x000016B1, 0x00001179, 0x0000159D,
    0x00050081, 0x00000006, 0x000016B2, 0x000016AD, 0x000016B1, 0x00050085, 0x00000006, 0x000016B6, 0x000011BA,
    0x0000159D, 0x00050081, 0x00000006, 0x000016B7, 0x000016B2, 0x000016B6, 0x00050085, 0x00000006, 0x000016BB,
    0x000011C7, 0x000015AF, 0x00050081, 0x00000006, 0x000016BC, 0x000016B7, 0x000016BB, 0x00050085, 0x00000006,
    0x000016C0, 0x00001208, 0x000015AF, 0x00050081, 0x00000006, 0x000016C1, 0x000016BC, 0x000016C0, 0x00050085,
    0x00000006, 0x000016C5, 0x000011EE, 0x000015CA, 0x00050081, 0x00000006, 0x000016C6, 0x000016C1, 0x000016C5,
    0x00050085, 0x00000006, 0x000016CA, 0x00001215, 0x000015CA, 0x00050081, 0x00000006, 0x000016CB, 0x000016C6,
    0x000016CA, 0x00050085, 0x00000006, 0x000016CF, 0x000011A0, 0x000015B2, 0x00050081, 0x00000006, 0x000016D0,
    0x000016CB, 0x000016CF, 0x00050085, 0x00000006, 0x000016D4, 0x000011AD, 0x000015CD, 0x00050081, 0x00000006,
    0x000016D5, 0x000016D0, 0x000016D4, 0x00050085, 0x00000006, 0x000016D9, 0x000011D4, 0x000015E8, 0x00050081,
    0x00000006, 0x000016DA, 0x000016D5, 0x000016D9, 0x00050085, 0x00000006, 0x000016DE, 0x000011E1, 0x00001603,
    0x00050081, 0x00000006, 0x000016DF, 0x000016DA, 0x000016DE, 0x00050085, 0x00000006, 0x000016E1, 0x000016DF,
    0x00001C4D, 0x0008000C, 0x00000006, 0x00001C70, 0x00000001, 0x0000002B, 0x000016E1, 0x00000067, 0x0000005F,
    0x00050081, 0x00000006, 0x00001ED7, 0x0000116E, 0x00001195, 0x00050085, 0x00000006, 0x000016EB, 0x00001597,
    0x00001ED7, 0x00050085, 0x00000006, 0x000016EF, 0x0000117B, 0x000015A0, 0x00050081, 0x00000006, 0x000016F0,
    0x000016EB, 0x000016EF, 0x00050085, 0x00000006, 0x000016F4, 0x000011BC, 0x000015A0, 0x00050081, 0x00000006,
    0x000016F5, 0x000016F0, 0x000016F4, 0x00050085, 0x00000006, 0x000016F9, 0x000011C9, 0x000015B8, 0x00050081,
    0x00000006, 0x000016FA, 0x000016F5, 0x000016F9, 0x00050085, 0x00000006, 0x000016FE, 0x0000120A, 0x000015B8,
    0x00050081, 0x00000006, 0x000016FF, 0x000016FA, 0x000016FE, 0x00050085, 0x00000006, 0x00001703, 0x000011F0,
    0x000015D3, 0x00050081, 0x00000006, 0x00001704, 0x000016FF, 0x00001703, 0x00050085, 0x00000006, 0x00001708,
    0x00001217, 0x000015D3, 0x00050081, 0x00000006, 0x00001709, 0x00001704, 0x00001708, 0x00050085, 0x00000006,
    0x0000170D, 0x000011A2, 0x000015BB, 0x00050081, 0x00000006, 0x0000170E, 0x00001709, 0x0000170D, 0x00050085,
    0x00000006, 0x00001712, 0x000011AF, 0x000015D6, 0x00050081, 0x00000006, 0x00001713, 0x0000170E, 0x00001712,
    0x00050085, 0x00000006, 0x00001717, 0x000011D6, 0x000015F1, 0x00050081, 0x00000006, 0x00001718, 0x00001713,
    0x00001717, 0x00050085, 0x00000006, 0x0000171C, 0x000011E3, 0x0000160C, 0x00050081, 0x00000006, 0x0000171D,
    0x00001718, 0x0000171C, 0x00050085, 0x00000006, 0x0000171F, 0x0000171D, 0x00001C5C, 0x0008000C, 0x00000006,
    0x00001C7B, 0x00000001, 0x0000002B, 0x0000171F, 0x00000067, 0x0000005F, 0x0004003D, 0x00000B9F, 0x00000BCE,
    0x00000BA1, 0x0004007C, 0x00000076, 0x00000BD1, 0x00000BB9, 0x00040064, 0x0000006F, 0x00000BD7, 0x00000B95,
    0x0007005F, 0x0000007A, 0x00000BD8, 0x00000BD7, 0x00000BD1, 0x00000002, 0x00000078, 0x00050051, 0x00000006,
    0x00000BD9, 0x00000BD8, 0x00000003, 0x00070050, 0x0000007A, 0x00000BDD, 0x00001C7B, 0x00001C70, 0x00001C65,
    0x00000BD9, 0x00040063, 0x00000BCE, 0x00000BD1, 0x00000BDD, 0x000100FD, 0x00010038};

static VkResult create_pipeline(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack, VkShaderModule shaderModule)
{
    VkResult res;
#if defined(USE_STRUCT_CONVERSION)
    VkComputePipelineCreateInfo_host pipelineInfo = {0};
#else
    VkComputePipelineCreateInfo pipelineInfo = {0};
#endif

    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = swapchain->pipeline_layout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    res = device->funcs.p_vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &hack->pipeline);
    if(res != VK_SUCCESS){
        ERR("vkCreateComputePipelines: %d\n", res);
        return res;
    }

    return VK_SUCCESS;
}

static VkResult create_descriptor_set(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    VkResult res;
#if defined(USE_STRUCT_CONVERSION)
    VkDescriptorSetAllocateInfo_host descriptorAllocInfo = {0};
    VkWriteDescriptorSet_host descriptorWrites[2] = {{0}, {0}};
    VkDescriptorImageInfo_host userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
#else
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {0};
    VkWriteDescriptorSet descriptorWrites[2] = {{0}, {0}};
    VkDescriptorImageInfo userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
#endif

    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = swapchain->descriptor_pool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &swapchain->descriptor_set_layout;

    res = device->funcs.p_vkAllocateDescriptorSets(device->device, &descriptorAllocInfo, &hack->descriptor_set);
    if(res != VK_SUCCESS){
        ERR("vkAllocateDescriptorSets: %d\n", res);
        return res;
    }

    userDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    userDescriptorImageInfo.imageView = hack->user_view;
    userDescriptorImageInfo.sampler = swapchain->sampler;

    realDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    realDescriptorImageInfo.imageView = hack->blit_view;

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = hack->descriptor_set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &userDescriptorImageInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = hack->descriptor_set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &realDescriptorImageInfo;

    device->funcs.p_vkUpdateDescriptorSets(device->device, 2, descriptorWrites, 0, NULL);

    return VK_SUCCESS;
}

static void destroy_fs_hack_image(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    device->funcs.p_vkDestroyPipeline(device->device, hack->pipeline, NULL);
    device->funcs.p_vkFreeDescriptorSets(device->device, swapchain->descriptor_pool, 1, &hack->descriptor_set);
    device->funcs.p_vkDestroyImageView(device->device, hack->user_view, NULL);
    device->funcs.p_vkDestroyImageView(device->device, hack->blit_view, NULL);
    device->funcs.p_vkDestroyImage(device->device, hack->user_image, NULL);
    device->funcs.p_vkDestroyImage(device->device, hack->blit_image, NULL);
    if(hack->cmd)
        device->funcs.p_vkFreeCommandBuffers(device->device,
                swapchain->cmd_pools[hack->cmd_queue_idx],
                    1, &hack->cmd);
    device->funcs.p_vkDestroySemaphore(device->device, hack->blit_finished, NULL);
}

#if defined(USE_STRUCT_CONVERSION)
static VkResult init_fs_hack_images(VkDevice device, struct VkSwapchainKHR_T *swapchain, VkSwapchainCreateInfoKHR_host *createinfo)
#else
static VkResult init_fs_hack_images(VkDevice device, struct VkSwapchainKHR_T *swapchain, VkSwapchainCreateInfoKHR *createinfo)
#endif
{
    VkResult res;
    VkImage *real_images = NULL;
    VkDeviceSize userMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkSemaphoreCreateInfo semaphoreInfo = {0};
#if defined(USE_STRUCT_CONVERSION)
    VkMemoryRequirements_host userMemReq;
    VkMemoryAllocateInfo_host allocInfo = {0};
    VkPhysicalDeviceMemoryProperties_host memProperties;
    VkImageViewCreateInfo_host viewInfo = {0};
#else
    VkMemoryRequirements userMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
#endif
    uint32_t count, i = 0, user_memory_type = -1;

    res = device->funcs.p_vkGetSwapchainImagesKHR(device->device, swapchain->swapchain, &count, NULL);
    if(res != VK_SUCCESS)
    {
        WARN("vkGetSwapchainImagesKHR failed, res=%d\n", res);
        return res;
    }

    real_images = heap_alloc(count * sizeof(VkImage));
    swapchain->cmd_pools = heap_alloc_zero(sizeof(VkCommandPool) * device->max_queue_families);
    swapchain->fs_hack_images = heap_alloc_zero(sizeof(struct fs_hack_image) * count);
    if(!real_images || !swapchain->cmd_pools || !swapchain->fs_hack_images)
        goto fail;

    res = device->funcs.p_vkGetSwapchainImagesKHR(device->device, swapchain->swapchain, &count, real_images);
    if(res != VK_SUCCESS)
    {
        WARN("vkGetSwapchainImagesKHR failed, res=%d\n", res);
        goto fail;
    }

    /* create user images */
    for(i = 0; i < count; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        hack->swapchain_image = real_images[i];

        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        res = device->funcs.p_vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &hack->blit_finished);
        if(res != VK_SUCCESS)
        {
            WARN("vkCreateSemaphore failed, res=%d\n", res);
            goto fail;
        }

        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchain->user_extent.width;
        imageInfo.extent.height = swapchain->user_extent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = createinfo->imageArrayLayers;
        imageInfo.format = createinfo->imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = createinfo->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = createinfo->imageSharingMode;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.queueFamilyIndexCount = createinfo->queueFamilyIndexCount;
        imageInfo.pQueueFamilyIndices = createinfo->pQueueFamilyIndices;
        res = device->funcs.p_vkCreateImage(device->device, &imageInfo, NULL, &hack->user_image);
        if(res != VK_SUCCESS){
            ERR("vkCreateImage failed: %d\n", res);
            goto fail;
        }

        device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->user_image, &userMemReq);

        offs = userMemTotal % userMemReq.alignment;
        if(offs)
            userMemTotal += userMemReq.alignment - offs;

        userMemTotal += userMemReq.size;

        swapchain->n_images++;
    }

    /* allocate backing memory */
    device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceMemoryProperties(device->phys_dev->phys_dev, &memProperties);

    for (i = 0; i < memProperties.memoryTypeCount; i++){
        if((memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT){
            if(userMemReq.memoryTypeBits & (1 << i)){
                user_memory_type = i;
                break;
            }
        }
    }

    if(user_memory_type == -1){
        ERR("unable to find suitable memory type\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = userMemTotal;
    allocInfo.memoryTypeIndex = user_memory_type;

    res = device->funcs.p_vkAllocateMemory(device->device, &allocInfo, NULL, &swapchain->user_image_memory);
    if(res != VK_SUCCESS){
        ERR("vkAllocateMemory: %d\n", res);
        goto fail;
    }

    /* bind backing memory and create imageviews */
    userMemTotal = 0;
    for(i = 0; i < count; ++i){
        device->funcs.p_vkGetImageMemoryRequirements(device->device, swapchain->fs_hack_images[i].user_image, &userMemReq);

        offs = userMemTotal % userMemReq.alignment;
        if(offs)
            userMemTotal += userMemReq.alignment - offs;

        res = device->funcs.p_vkBindImageMemory(device->device, swapchain->fs_hack_images[i].user_image, swapchain->user_image_memory, userMemTotal);
        if(res != VK_SUCCESS){
            ERR("vkBindImageMemory: %d\n", res);
            goto fail;
        }

        userMemTotal += userMemReq.size;

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->fs_hack_images[i].user_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = createinfo->imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        res = device->funcs.p_vkCreateImageView(device->device, &viewInfo, NULL, &swapchain->fs_hack_images[i].user_view);
        if(res != VK_SUCCESS){
            ERR("vkCreateImageView(user): %d\n", res);
            goto fail;
        }
    }

    heap_free(real_images);

    return VK_SUCCESS;

fail:
    for(i = 0; i < swapchain->n_images; ++i)
        destroy_fs_hack_image(device, swapchain, &swapchain->fs_hack_images[i]);
    heap_free(real_images);
    heap_free(swapchain->cmd_pools);
    heap_free(swapchain->fs_hack_images);
    return res;
}

static VkResult init_blit_images(VkDevice device, struct VkSwapchainKHR_T *swapchain);
VkResult WINAPI wine_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    VkResult result;
#if defined(USE_STRUCT_CONVERSION)
    VkSwapchainCreateInfoKHR_host our_createinfo;
#else
    VkSwapchainCreateInfoKHR our_createinfo;
#endif
    VkExtent2D user_sz;
    struct VkSwapchainKHR_T *object;
    uint32_t i;

    TRACE("%p, %p, %p, %p\n", device, pCreateInfo, pAllocator, pSwapchain);

    if (!(object = heap_alloc_zero(sizeof(*object))))
    {
        ERR("Failed to allocate memory for swapchain\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    object->base.loader_magic = VULKAN_ICD_MAGIC_VALUE;

    convert_VkSwapchainCreateInfoKHR_win_to_host(pCreateInfo, &our_createinfo);

    if(our_createinfo.oldSwapchain)
        our_createinfo.oldSwapchain = ((struct VkSwapchainKHR_T *)(UINT_PTR)our_createinfo.oldSwapchain)->swapchain;

    if(vk_funcs->query_fs_hack &&
            vk_funcs->query_fs_hack(&object->real_extent, &user_sz, &object->blit_dst, &object->fs_hack_filter) &&
            our_createinfo.imageExtent.width == user_sz.width &&
            our_createinfo.imageExtent.height == user_sz.height)
    {
        uint32_t count;
        VkSurfaceCapabilitiesKHR caps = {0};

        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceQueueFamilyProperties(device->phys_dev->phys_dev, &count, NULL);

        device->queue_props = heap_alloc(sizeof(VkQueueFamilyProperties) * count);

        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceQueueFamilyProperties(device->phys_dev->phys_dev, &count, device->queue_props);

        result = device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->phys_dev->phys_dev, pCreateInfo->surface, &caps);
        if(result != VK_SUCCESS)
        {
            TRACE("vkGetPhysicalDeviceSurfaceCapabilities failed, res=%d\n", result);
            heap_free(object);
            return result;
        }

        object->surface_usage = caps.supportedUsageFlags;
        TRACE("surface usage flags: 0x%x\n", object->surface_usage);

        our_createinfo.imageExtent = object->real_extent;
        our_createinfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; /* XXX: check if supported by surface */

        if(our_createinfo.imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
                our_createinfo.imageFormat != VK_FORMAT_B8G8R8A8_SRGB){
            FIXME("swapchain image format is not BGRA8 UNORM/SRGB. Things may go badly. %d\n", our_createinfo.imageFormat);
        }

        object->fs_hack_enabled = TRUE;
    }

    result = device->funcs.p_vkCreateSwapchainKHR(device->device, &our_createinfo, NULL, &object->swapchain);
    if(result != VK_SUCCESS)
    {
        TRACE("vkCreateSwapchainKHR failed, res=%d\n", result);
        heap_free(object);
        return result;
    }

    if(object->fs_hack_enabled){
        object->user_extent = pCreateInfo->imageExtent;

        result = init_fs_hack_images(device, object, &our_createinfo);
        if(result != VK_SUCCESS){
            ERR("creating fs hack images failed: %d\n", result);
            device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);
            heap_free(object);
            return result;
        }

        /* FIXME: would be nice to do this on-demand, but games can use up all
         * memory so we fail to allocate later */
        result = init_blit_images(device, object);
        if(result != VK_SUCCESS){
            ERR("creating blit images failed: %d\n", result);
            wine_vkDestroySwapchainKHR(device, (VkSwapchainKHR)object, NULL);
            return result;
        }
    }

    if(result != VK_SUCCESS){
        heap_free(object);
        return result;
    }

    EnterCriticalSection(&device->swapchain_lock);
    for(i = 0; i < device->num_swapchains; ++i){
        if(!device->swapchains[i]){
            device->swapchains[i] = object;
            break;
        }
    }
    if(i == device->num_swapchains){
        struct VkSwapchainKHR_T **swapchains;
        swapchains = heap_realloc(device->swapchains, sizeof(struct VkSwapchainKHR_T *) * (device->num_swapchains + 1));
        if(!swapchains){
            device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);
            heap_free(object);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        swapchains[i] = object;
        device->swapchains = swapchains;
        device->num_swapchains += 1;
    }
    LeaveCriticalSection(&device->swapchain_lock);

    *pSwapchain = (uint64_t)(UINT_PTR)object;

    return result;
}

void WINAPI wine_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)swapchain;
    uint32_t i;

    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(swapchain), pAllocator);

    if(!object)
        return;

    EnterCriticalSection(&device->swapchain_lock);
    for(i = 0; i < device->num_swapchains; ++i){
        if(device->swapchains[i] == object){
            device->swapchains[i] = NULL;
            break;
        }
    }
    LeaveCriticalSection(&device->swapchain_lock);

    if(object->fs_hack_enabled){
        for(i = 0; i < object->n_images; ++i)
            destroy_fs_hack_image(device, object, &object->fs_hack_images[i]);

        for(i = 0; i < device->max_queue_families; ++i)
            if(object->cmd_pools[i])
                device->funcs.p_vkDestroyCommandPool(device->device, object->cmd_pools[i], NULL);

        device->funcs.p_vkDestroyPipelineLayout(device->device, object->pipeline_layout, NULL);
        device->funcs.p_vkDestroyDescriptorSetLayout(device->device, object->descriptor_set_layout, NULL);
        device->funcs.p_vkDestroyDescriptorPool(device->device, object->descriptor_pool, NULL);
        device->funcs.p_vkDestroySampler(device->device, object->sampler, NULL);
        device->funcs.p_vkFreeMemory(device->device, object->user_image_memory, NULL);
        device->funcs.p_vkFreeMemory(device->device, object->blit_image_memory, NULL);
        heap_free(object->cmd_pools);
        heap_free(object->fs_hack_images);
    }

    device->funcs.p_vkDestroySwapchainKHR(device->device, object->swapchain, NULL);

    heap_free(object);
}

VkResult WINAPI wine_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
    struct VkSwapchainKHR_T *object = (struct VkSwapchainKHR_T *)(UINT_PTR)swapchain;
    uint32_t i;

    TRACE("%p, 0x%s, %p, %p\n", device, wine_dbgstr_longlong(swapchain), pSwapchainImageCount, pSwapchainImages);

    if(pSwapchainImages && object->fs_hack_enabled){
        if(*pSwapchainImageCount > object->n_images)
            *pSwapchainImageCount = object->n_images;
        for(i = 0; i < *pSwapchainImageCount ; ++i)
            pSwapchainImages[i] = object->fs_hack_images[i].user_image;
        return *pSwapchainImageCount == object->n_images ? VK_SUCCESS : VK_INCOMPLETE;
    }

    return device->funcs.p_vkGetSwapchainImagesKHR(device->device, object->swapchain, pSwapchainImageCount, pSwapchainImages);
}

static uint32_t get_queue_index(VkQueue queue)
{
    uint32_t i;
    for(i = 0; i < queue->device->max_queue_families; ++i){
        if(queue->device->queues[i] == queue)
            return i;
    }
    WARN("couldn't find queue\n");
    return -1;
}

static VkCommandBuffer create_hack_cmd(VkQueue queue, struct VkSwapchainKHR_T *swapchain, uint32_t queue_idx)
{
#if defined(USE_STRUCT_CONVERSION)
    VkCommandBufferAllocateInfo_host allocInfo = {0};
#else
    VkCommandBufferAllocateInfo allocInfo = {0};
#endif
    VkCommandBuffer cmd;
    VkResult result;

    if(!swapchain->cmd_pools[queue_idx]){
        VkCommandPoolCreateInfo poolInfo = {0};

        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_idx;

        result = queue->device->funcs.p_vkCreateCommandPool(queue->device->device, &poolInfo, NULL, &swapchain->cmd_pools[queue_idx]);
        if(result != VK_SUCCESS){
            ERR("vkCreateCommandPool failed, res=%d\n", result);
            return NULL;
        }
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = swapchain->cmd_pools[queue_idx];
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    result = queue->device->funcs.p_vkAllocateCommandBuffers(queue->device->device, &allocInfo, &cmd);
    if(result != VK_SUCCESS){
        ERR("vkAllocateCommandBuffers failed, res=%d\n", result);
        return NULL;
    }

    return cmd;
}

static VkResult init_blit_images(VkDevice device, struct VkSwapchainKHR_T *swapchain)
{
    VkResult res;
    VkSamplerCreateInfo samplerInfo = {0};
    VkDescriptorPoolSize poolSizes[2] = {{0}, {0}};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    VkDescriptorSetLayoutBinding layoutBindings[2] = {{0}, {0}};
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {0};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    VkPushConstantRange pushConstants;
    VkShaderModuleCreateInfo shaderInfo = {0};
    VkShaderModule shaderModule = 0;
    VkDeviceSize blitMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
#if defined(USE_STRUCT_CONVERSION)
    VkMemoryRequirements_host blitMemReq;
    VkMemoryAllocateInfo_host allocInfo = {0};
    VkPhysicalDeviceMemoryProperties_host memProperties;
    VkImageViewCreateInfo_host viewInfo = {0};
#else
    VkMemoryRequirements blitMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
#endif
    uint32_t blit_memory_type = -1, i;

    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = swapchain->fs_hack_filter;
    samplerInfo.minFilter = swapchain->fs_hack_filter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    res = device->funcs.p_vkCreateSampler(device->device, &samplerInfo, NULL, &swapchain->sampler);
    if(res != VK_SUCCESS)
    {
        WARN("vkCreateSampler failed, res=%d\n", res);
        return res;
    }

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = swapchain->n_images;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = swapchain->n_images;

    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = swapchain->n_images;

    res = device->funcs.p_vkCreateDescriptorPool(device->device, &poolInfo, NULL, &swapchain->descriptor_pool);
    if(res != VK_SUCCESS){
        ERR("vkCreateDescriptorPool: %d\n", res);
        goto fail;
    }

    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].pImmutableSamplers = NULL;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].pImmutableSamplers = NULL;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 2;
    descriptorLayoutInfo.pBindings = layoutBindings;

    res = device->funcs.p_vkCreateDescriptorSetLayout(device->device, &descriptorLayoutInfo, NULL, &swapchain->descriptor_set_layout);
    if(res != VK_SUCCESS){
        ERR("vkCreateDescriptorSetLayout: %d\n", res);
        goto fail;
    }

    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.offset = 0;
    pushConstants.size = 4 * sizeof(float); /* 2 * vec2 */

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &swapchain->descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    res = device->funcs.p_vkCreatePipelineLayout(device->device, &pipelineLayoutInfo, NULL, &swapchain->pipeline_layout);
    if(res != VK_SUCCESS){
        ERR("vkCreatePipelineLayout: %d\n", res);
        goto fail;
    }

    if(!(swapchain->surface_usage & VK_IMAGE_USAGE_STORAGE_BIT)){
        TRACE("using intermediate blit images\n");
        /* create intermediate blit images */
        for(i = 0; i < swapchain->n_images; ++i){
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = swapchain->real_extent.width;
            imageInfo.extent.height = swapchain->real_extent.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            res = device->funcs.p_vkCreateImage(device->device, &imageInfo, NULL, &hack->blit_image);
            if(res != VK_SUCCESS){
                ERR("vkCreateImage failed: %d\n", res);
                goto fail;
            }

            device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->blit_image, &blitMemReq);

            offs = blitMemTotal % blitMemReq.alignment;
            if(offs)
                blitMemTotal += blitMemReq.alignment - offs;

            blitMemTotal += blitMemReq.size;
        }

        /* allocate backing memory */
        device->phys_dev->instance->funcs.p_vkGetPhysicalDeviceMemoryProperties(device->phys_dev->phys_dev, &memProperties);

        for(i = 0; i < memProperties.memoryTypeCount; i++){
            if((memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT){
                if(blitMemReq.memoryTypeBits & (1 << i)){
                    blit_memory_type = i;
                    break;
                }
            }
        }

        if(blit_memory_type == -1){
            ERR("unable to find suitable memory type\n");
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }

        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = blitMemTotal;
        allocInfo.memoryTypeIndex = blit_memory_type;

        res = device->funcs.p_vkAllocateMemory(device->device, &allocInfo, NULL, &swapchain->blit_image_memory);
        if(res != VK_SUCCESS){
            ERR("vkAllocateMemory: %d\n", res);
            goto fail;
        }

        /* bind backing memory and create imageviews */
        blitMemTotal = 0;
        for(i = 0; i < swapchain->n_images; ++i){
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            device->funcs.p_vkGetImageMemoryRequirements(device->device, hack->blit_image, &blitMemReq);

            offs = blitMemTotal % blitMemReq.alignment;
            if(offs)
                blitMemTotal += blitMemReq.alignment - offs;

            res = device->funcs.p_vkBindImageMemory(device->device, hack->blit_image, swapchain->blit_image_memory, blitMemTotal);
            if(res != VK_SUCCESS){
                ERR("vkBindImageMemory: %d\n", res);
                goto fail;
            }

            blitMemTotal += blitMemReq.size;
        }
    }else
        TRACE("blitting directly to swapchain images\n");

    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(blit_comp_spv);
    shaderInfo.pCode = blit_comp_spv;

    res = device->funcs.p_vkCreateShaderModule(device->device, &shaderInfo, NULL, &shaderModule);
    if(res != VK_SUCCESS){
        ERR("vkCreateShaderModule: %d\n", res);
        goto fail;
    }

    /* create imageviews */
    for(i = 0; i < swapchain->n_images; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hack->blit_image ? hack->blit_image : hack->swapchain_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        res = device->funcs.p_vkCreateImageView(device->device, &viewInfo, NULL, &hack->blit_view);
        if(res != VK_SUCCESS){
            ERR("vkCreateImageView(blit): %d\n", res);
            goto fail;
        }

        res = create_descriptor_set(device, swapchain, hack);
        if(res != VK_SUCCESS)
            goto fail;

        res = create_pipeline(device, swapchain, hack, shaderModule);
        if(res != VK_SUCCESS)
            goto fail;
    }

    device->funcs.p_vkDestroyShaderModule(device->device, shaderModule, NULL);

    return VK_SUCCESS;

fail:
    for(i = 0; i < swapchain->n_images; ++i){
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        device->funcs.p_vkDestroyPipeline(device->device, hack->pipeline, NULL);
        hack->pipeline = VK_NULL_HANDLE;

        device->funcs.p_vkFreeDescriptorSets(device->device, swapchain->descriptor_pool, 1, &hack->descriptor_set);
        hack->descriptor_set = VK_NULL_HANDLE;

        device->funcs.p_vkDestroyImageView(device->device, hack->blit_view, NULL);
        hack->blit_view = VK_NULL_HANDLE;

        device->funcs.p_vkDestroyImage(device->device, hack->blit_image, NULL);
        hack->blit_image = VK_NULL_HANDLE;
    }

    device->funcs.p_vkDestroyShaderModule(device->device, shaderModule, NULL);

    device->funcs.p_vkDestroyPipelineLayout(device->device, swapchain->pipeline_layout, NULL);
    swapchain->pipeline_layout = VK_NULL_HANDLE;

    device->funcs.p_vkDestroyDescriptorSetLayout(device->device, swapchain->descriptor_set_layout, NULL);
    swapchain->descriptor_set_layout = VK_NULL_HANDLE;

    device->funcs.p_vkDestroyDescriptorPool(device->device, swapchain->descriptor_pool, NULL);
    swapchain->descriptor_pool = VK_NULL_HANDLE;

    device->funcs.p_vkFreeMemory(device->device, swapchain->blit_image_memory, NULL);
    swapchain->blit_image_memory = VK_NULL_HANDLE;

    device->funcs.p_vkDestroySampler(device->device, swapchain->sampler, NULL);
    swapchain->sampler = VK_NULL_HANDLE;

    return res;
}

static VkResult record_compute_cmd(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    VkResult result;
    VkImageCopy region = {0};
#if defined(USE_STRUCT_CONVERSION)
    VkImageMemoryBarrier_host barriers[3] = {{0}};
    VkCommandBufferBeginInfo_host beginInfo = {0};
#else
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
#endif
    float constants[4];

    TRACE("recording compute command\n");

#if 0
    /* DOOM runs out of memory when allocating blit images after loading. */
    if(!swapchain->blit_image_memory){
        result = init_blit_images(device, swapchain);
        if(result != VK_SUCCESS)
            return result;
    }
#endif

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->funcs.p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* transition user image from GENERAL to SHADER_READ */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* transition blit image from whatever to GENERAL */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->blit_image ? hack->blit_image : hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    /* perform blit shader */
    device->funcs.p_vkCmdBindPipeline(hack->cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, hack->pipeline);

    device->funcs.p_vkCmdBindDescriptorSets(hack->cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, swapchain->pipeline_layout,
            0, 1, &hack->descriptor_set, 0, NULL);

    /* vec2: blit dst offset in real coords */
    constants[0] = swapchain->blit_dst.offset.x;
    constants[1] = swapchain->blit_dst.offset.y;
    /* vec2: blit dst extents in real coords */
    constants[2] = swapchain->blit_dst.extent.width;
    constants[3] = swapchain->blit_dst.extent.height;
    device->funcs.p_vkCmdPushConstants(hack->cmd,
            swapchain->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(constants), constants);

    /* local sizes in shader are 8 */
    device->funcs.p_vkCmdDispatch(hack->cmd, ceil(swapchain->real_extent.width / 8.),
            ceil(swapchain->real_extent.height / 8.), 1);

    /* transition user image from SHADER_READ to GENERAL */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            1, barriers
    );

    if(hack->blit_image){
        /* transition blit image layout from GENERAL to TRANSFER_SRC
         * and access from SHADER_WRITE_BIT to TRANSFER_READ_BIT  */
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = hack->blit_image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        /* transition swapchain image from whatever to PRESENT_SRC */
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = hack->swapchain_image;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.baseMipLevel = 0;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.baseArrayLayer = 0;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        device->funcs.p_vkCmdPipelineBarrier(
                hack->cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, NULL,
                0, NULL,
                2, barriers
        );

        /* copy from blit image to swapchain image */
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffset.x = 0;
        region.srcOffset.y = 0;
        region.srcOffset.z = 0;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffset.x = 0;
        region.dstOffset.y = 0;
        region.dstOffset.z = 0;
        region.extent.width = swapchain->real_extent.width;
        region.extent.height = swapchain->real_extent.height;
        region.extent.depth = 1;

        device->funcs.p_vkCmdCopyImage(hack->cmd,
                hack->blit_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                hack->swapchain_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                1, &region);
    }else{
        /* transition swapchain image from GENERAL to PRESENT_SRC */
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = hack->swapchain_image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = 0;

        device->funcs.p_vkCmdPipelineBarrier(
                hack->cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0, NULL,
                0, NULL,
                1, barriers
        );
    }

    result = device->funcs.p_vkEndCommandBuffer(hack->cmd);
    if(result != VK_SUCCESS){
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

static VkResult record_graphics_cmd(VkDevice device, struct VkSwapchainKHR_T *swapchain, struct fs_hack_image *hack)
{
    VkResult result;
    VkImageBlit blitregion = {0};
    VkImageSubresourceRange range = {0};
    VkClearColorValue black = {{0.f, 0.f, 0.f}};
#if defined(USE_STRUCT_CONVERSION)
    VkImageMemoryBarrier_host barriers[2] = {{0}};
    VkCommandBufferBeginInfo_host beginInfo = {0};
#else
    VkImageMemoryBarrier barriers[2] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
#endif

    TRACE("recording graphics command\n");

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->funcs.p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* transition user image from GENERAL to TRANSFER_SRC_OPTIMAL */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    /* transition real image from whatever to TRANSFER_DST_OPTIMAL */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    /* clear the image */
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    device->funcs.p_vkCmdClearColorImage(
            hack->cmd, hack->swapchain_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &black, 1, &range);

    /* perform blit */
    blitregion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitregion.srcSubresource.layerCount = 1;
    blitregion.srcOffsets[0].x = 0;
    blitregion.srcOffsets[0].y = 0;
    blitregion.srcOffsets[0].z = 0;
    blitregion.srcOffsets[1].x = swapchain->user_extent.width;
    blitregion.srcOffsets[1].y = swapchain->user_extent.height;
    blitregion.srcOffsets[1].z = 1;
    blitregion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitregion.dstSubresource.layerCount = 1;
    blitregion.dstOffsets[0].x = swapchain->blit_dst.offset.x;
    blitregion.dstOffsets[0].y = swapchain->blit_dst.offset.y;
    blitregion.dstOffsets[0].z = 0;
    blitregion.dstOffsets[1].x = swapchain->blit_dst.offset.x + swapchain->blit_dst.extent.width;
    blitregion.dstOffsets[1].y = swapchain->blit_dst.offset.y + swapchain->blit_dst.extent.height;
    blitregion.dstOffsets[1].z = 1;

    device->funcs.p_vkCmdBlitImage(hack->cmd,
            hack->user_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            hack->swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitregion, swapchain->fs_hack_filter);

    /* transition user image from TRANSFER_SRC_OPTIMAL to GENERAL */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition real image from TRANSFER_DST to PRESENT_SRC */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;

    device->funcs.p_vkCmdPipelineBarrier(
            hack->cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, NULL,
            0, NULL,
            2, barriers
    );

    result = device->funcs.p_vkEndCommandBuffer(hack->cmd);
    if(result != VK_SUCCESS){
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

VkResult WINAPI wine_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    VkResult res;
    VkPresentInfoKHR our_presentInfo;
    VkSwapchainKHR *arr;
    VkCommandBuffer *blit_cmds = NULL;
    VkSubmitInfo submitInfo = {0};
    VkSemaphore blit_sema;
    struct VkSwapchainKHR_T *swapchain;
    uint32_t i, n_hacks = 0;
    uint32_t queue_idx;

    TRACE("%p, %p\n", queue, pPresentInfo);

    our_presentInfo = *pPresentInfo;

    for(i = 0; i < our_presentInfo.swapchainCount; ++i){
        swapchain = (struct VkSwapchainKHR_T *)(UINT_PTR)our_presentInfo.pSwapchains[i];

        if(swapchain->fs_hack_enabled){
            struct fs_hack_image *hack = &swapchain->fs_hack_images[our_presentInfo.pImageIndices[i]];

            if(!blit_cmds){
                queue_idx = get_queue_index(queue);
                blit_cmds = heap_alloc(our_presentInfo.swapchainCount * sizeof(VkCommandBuffer));
                blit_sema = hack->blit_finished;
            }

            if(!hack->cmd || hack->cmd_queue_idx != queue_idx){
                if(hack->cmd)
                    queue->device->funcs.p_vkFreeCommandBuffers(queue->device->device,
                            swapchain->cmd_pools[hack->cmd_queue_idx],
                            1, &hack->cmd);

                hack->cmd_queue_idx = queue_idx;
                hack->cmd = create_hack_cmd(queue, swapchain, queue_idx);

                if(!hack->cmd){
                    heap_free(blit_cmds);
                    return VK_ERROR_DEVICE_LOST;
                }

                if(queue->device->queue_props[queue_idx].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    res = record_graphics_cmd(queue->device, swapchain, hack);
                else if(queue->device->queue_props[queue_idx].queueFlags & VK_QUEUE_COMPUTE_BIT)
                    res = record_compute_cmd(queue->device, swapchain, hack);
                else{
                    ERR("Present queue is neither graphics nor compute queue!\n");
                    res = VK_ERROR_DEVICE_LOST;
                }

                if(res != VK_SUCCESS){
                    queue->device->funcs.p_vkFreeCommandBuffers(queue->device->device,
                            swapchain->cmd_pools[hack->cmd_queue_idx],
                            1, &hack->cmd);
                    hack->cmd = NULL;
                    heap_free(blit_cmds);
                    return res;
                }
            }

            blit_cmds[n_hacks] = hack->cmd;

            ++n_hacks;
        }
    }

    if(n_hacks > 0){
        VkPipelineStageFlags waitStage, *waitStages, *waitStages_arr = NULL;

        if(pPresentInfo->waitSemaphoreCount > 1){
            waitStages_arr = heap_alloc(sizeof(VkPipelineStageFlags) * pPresentInfo->waitSemaphoreCount);
            for(i = 0; i < pPresentInfo->waitSemaphoreCount; ++i)
                waitStages_arr[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitStages = waitStages_arr;
        }else{
            waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitStages = &waitStage;
        }

        /* blit user image to real image */
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = n_hacks;
        submitInfo.pCommandBuffers = blit_cmds;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &blit_sema;

        res = queue->device->funcs.p_vkQueueSubmit(queue->queue, 1, &submitInfo, VK_NULL_HANDLE);
        if(res != VK_SUCCESS)
            ERR("vkQueueSubmit: %d\n", res);

        heap_free(waitStages_arr);
        heap_free(blit_cmds);

        our_presentInfo.waitSemaphoreCount = 1;
        our_presentInfo.pWaitSemaphores = &blit_sema;
    }

    arr = heap_alloc(our_presentInfo.swapchainCount * sizeof(VkSwapchainKHR));
    if(!arr){
        ERR("Failed to allocate memory for swapchain array\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for(i = 0; i < our_presentInfo.swapchainCount; ++i)
        arr[i] = ((struct VkSwapchainKHR_T *)(UINT_PTR)our_presentInfo.pSwapchains[i])->swapchain;

    our_presentInfo.pSwapchains = arr;

    res = queue->device->funcs.p_vkQueuePresentKHR(queue->queue, &our_presentInfo);

    heap_free(arr);

    return res;

}

void WINAPI wine_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer,
        VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
        VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
        const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
        const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
        const VkImageMemoryBarrier *pImageMemoryBarriers)
{
#if defined(USE_STRUCT_CONVERSION)
    VkBufferMemoryBarrier_host *pBufferMemoryBarriers_host;
#endif
    VkImageMemoryBarrier_host *pImageMemoryBarriers_host = NULL;
    uint32_t i, j, k;
    int old, new;

    TRACE("%p, %#x, %#x, %#x, %u, %p, %u, %p, %u, %p\n", commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);

#if defined(USE_STRUCT_CONVERSION)
    pBufferMemoryBarriers_host = convert_VkBufferMemoryBarrier_array_win_to_host(pBufferMemoryBarriers, bufferMemoryBarrierCount);
    pImageMemoryBarriers_host = convert_VkImageMemoryBarrier_array_win_to_host(pImageMemoryBarriers, imageMemoryBarrierCount);
#endif

    /* if the client is trying to transition a user image to PRESENT_SRC,
     * transition it to GENERAL instead. */
    EnterCriticalSection(&commandBuffer->device->swapchain_lock);
    for(i = 0; i < imageMemoryBarrierCount; ++i){
        old = pImageMemoryBarriers[i].oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        new = pImageMemoryBarriers[i].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if(old || new){
            for(j = 0; j < commandBuffer->device->num_swapchains; ++j){
                struct VkSwapchainKHR_T *swapchain = commandBuffer->device->swapchains[j];
                if(swapchain->fs_hack_enabled){
                    for(k = 0; k < swapchain->n_images; ++k){
                        struct fs_hack_image *hack = &swapchain->fs_hack_images[k];
                        if(pImageMemoryBarriers[i].image == hack->user_image){
#if !defined(USE_STRUCT_CONVERSION)
                            if(!pImageMemoryBarriers_host)
                                pImageMemoryBarriers_host = convert_VkImageMemoryBarrier_array_win_to_host(pImageMemoryBarriers, imageMemoryBarrierCount);
#endif
                            if(old)
                                pImageMemoryBarriers_host[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                            if(new)
                                pImageMemoryBarriers_host[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                            goto next;
                        }
                    }
                }
            }
        }
next:   ;
    }
    LeaveCriticalSection(&commandBuffer->device->swapchain_lock);

    commandBuffer->device->funcs.p_vkCmdPipelineBarrier(commandBuffer->command_buffer,
            srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount,
            pMemoryBarriers, bufferMemoryBarrierCount,
#if defined(USE_STRUCT_CONVERSION)
            pBufferMemoryBarriers_host, imageMemoryBarrierCount, pImageMemoryBarriers_host
#else
            pBufferMemoryBarriers, imageMemoryBarrierCount,
            pImageMemoryBarriers_host ? (VkImageMemoryBarrier*)pImageMemoryBarriers_host : pImageMemoryBarriers
#endif
            );

#if defined(USE_STRUCT_CONVERSION)
    free_VkBufferMemoryBarrier_array(pBufferMemoryBarriers_host, bufferMemoryBarrierCount);
#else
    if(pImageMemoryBarriers_host)
#endif
        free_VkImageMemoryBarrier_array(pImageMemoryBarriers_host, imageMemoryBarrierCount);
}

VkDevice WINAPI __wine_get_native_VkDevice(VkDevice device)
{
    return device->device;
}

VkInstance WINAPI __wine_get_native_VkInstance(VkInstance instance)
{
    return instance->instance;
}

VkPhysicalDevice WINAPI __wine_get_native_VkPhysicalDevice(VkPhysicalDevice phys_dev)
{
    return phys_dev->phys_dev;
}

VkQueue WINAPI __wine_get_native_VkQueue(VkQueue queue)
{
    return queue->queue;
}

VkPhysicalDevice WINAPI __wine_get_wrapped_VkPhysicalDevice(VkInstance instance, VkPhysicalDevice native_phys_dev)
{
    uint32_t i;
    for(i = 0; i < instance->phys_dev_count; ++i){
        if(instance->phys_devs[i]->phys_dev == native_phys_dev)
            return instance->phys_devs[i];
    }
    WARN("Unknown native physical device: %p\n", native_phys_dev);
    return NULL;
}
