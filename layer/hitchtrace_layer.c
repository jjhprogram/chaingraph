// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * VK_LAYER_hitchtrace: an implicit Vulkan layer that gives hitchtrace a frame
 * boundary in any Vulkan application, without rebuilding it.
 *
 *	app --> loader --> [VK_LAYER_hitchtrace] --> ... --> ICD
 *	                       |
 *	                       +-- hitch_frame_mark(frame_id)   present ENTRY
 *	                       +-- (down-chain vkQueuePresentKHR)
 *	                       +-- hitch_present_end(frame_id)  present RETURN
 *
 * Why a layer and not a uprobe on the loader's exported vkQueuePresentKHR:
 * an app that resolves entry points through vkGetDeviceProcAddr (volk, DXVK,
 * winevulkan, vkcube as shipped) never enters the loader's export, so that
 * uprobe counts zero. /usr/bin/vkcube here has no dynamic import of the symbol
 * at all. Layers sit in the device dispatch chain no matter how the app got
 * its pointers, so this fires for every present.
 *
 * Frame model (plan 2.2 as amended by review issue 8):
 *	frame N = [present_enter(N-1), present_enter(N))
 * hitch_frame_mark() IS present entry and IS the frame boundary: the frame it
 * names ends there and the next one opens. hitch_present_end() closes the
 * present bracket, so the blocked time between the two is attributable to
 * HT_BLOCK_PRESENT -- the display pacing the app -- and lands in the window of
 * the frame that just opened.
 *
 * frame_id is process-global, not per device: the BPF side keys frame state by
 * tgid, so one monotonic sequence per process is what it wants. It counts
 * presents, not "visual frames" -- an app with two swapchains presenting twice
 * per rendered image produces two ids, exactly like PresentMon's
 * MsBetweenPresents. Say so in the run manifest.
 *
 * Hot path: one relaxed atomic increment, one lock-free scan of a static
 * dispatch table, two marker calls, one indirect call down the chain. No
 * allocation, no locking, no logging, no getenv. Presents from several threads
 * (the norm: one thread per queue) only contend on the frame-id counter.
 *
 * With nothing attached the two markers are two empty calls.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define HT_LAYER_NAME		"VK_LAYER_hitchtrace"
#define HT_LAYER_DESCRIPTION	"hitchtrace frame markers (uprobe targets)"
#define HT_IMPL_VERSION		1

#define HT_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------ */
/* the frame markers: the uprobe targets                               */
/* ------------------------------------------------------------------ */

/*
 * void hitch_frame_mark(unsigned long frame_id)
 * void hitch_present_end(unsigned long frame_id)
 *
 * Same contract as tests/hitchbench.c, and deliberately dull and deliberately
 * unoptimizable: external linkage with default visibility so the names are in
 * .symtab AND .dynsym (a uprobe resolves them by name), noinline/noclone so no
 * caller gets a specialized copy, used so they survive even if nothing calls
 * them, and an asm barrier plus a store to a volatile so the compiler cannot
 * infer they are pure and drop the calls. At the uprobe (function entry) the
 * frame id is simply the first argument in the ABI's first argument register.
 *
 * The counters are the no-BPF proof that the markers really executed: they are
 * incremented here, inside the function the uprobe would attach to, not by the
 * caller. One relaxed lock xadd per present (tens of nanoseconds against a
 * present measured in microseconds to milliseconds); it is the hitchbench
 * volatile store, strengthened so it counts correctly when several threads
 * present at once. Nothing reads them unless HITCHTRACE_DEBUG=1.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define HT_MARKER __attribute__((noinline, noclone, used)) HT_EXPORT
#else
#define HT_MARKER __attribute__((noinline, used)) HT_EXPORT
#endif

static volatile unsigned long hitch_frame_seen;
static volatile unsigned long hitch_present_seen;
static atomic_ulong hitch_frame_calls;
static atomic_ulong hitch_present_calls;

HT_MARKER void hitch_frame_mark(unsigned long frame_id)
{
	__asm__ __volatile__("" : : "r"(frame_id) : "memory");
	hitch_frame_seen = frame_id;
	atomic_fetch_add_explicit(&hitch_frame_calls, 1, memory_order_relaxed);
}

HT_MARKER void hitch_present_end(unsigned long frame_id)
{
	__asm__ __volatile__("" : : "r"(frame_id) : "memory");
	hitch_present_seen = frame_id;
	atomic_fetch_add_explicit(&hitch_present_calls, 1, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* dispatch tables                                                     */
/* ------------------------------------------------------------------ */

/*
 * Every dispatchable Vulkan handle starts with a pointer to its loader
 * dispatch table, and every object made from a device (its queues, its command
 * buffers) carries the device's. So *(void **)handle is the key, and a queue
 * finds its device with it -- no need to intercept vkGetDeviceQueue.
 *
 * Static tables, so nothing is ever allocated or freed and a lookup can never
 * race a free. Slots are published key-last with a release store and read with
 * an acquire load, so the present path scans them without taking the lock. The
 * lock only serializes writers (create/destroy) and is never held across a
 * down-chain call.
 */
#define HT_MAX_INSTANCES	16
#define HT_MAX_DEVICES		32

struct ht_instance {
	_Atomic(void *)		key;		/* NULL: free slot */
	VkInstance		instance;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkDestroyInstance	next_destroy_instance;
};

struct ht_device {
	_Atomic(void *)		key;		/* NULL: free slot */
	VkDevice		device;
	PFN_vkGetDeviceProcAddr	next_gdpa;
	PFN_vkDestroyDevice	next_destroy_device;
	PFN_vkQueuePresentKHR	next_queue_present;
};

static struct ht_instance ht_instances[HT_MAX_INSTANCES];
static struct ht_device ht_devices[HT_MAX_DEVICES];
static pthread_mutex_t ht_table_lock = PTHREAD_MUTEX_INITIALIZER;

/* one process-wide monotonic present counter; see the header comment */
static atomic_ulong ht_frame_id;

/* debug bookkeeping, read once at instance create, never in the hot path */
static int ht_debug;
static struct timespec ht_t0;
static atomic_ulong ht_queue_key_misses;

static inline void *ht_key(const void *dispatchable)
{
	return *(void **)(void *)(uintptr_t)dispatchable;
}

static struct ht_instance *ht_find_instance(void *key)
{
	for (int i = 0; i < HT_MAX_INSTANCES; i++)
		if (atomic_load_explicit(&ht_instances[i].key,
					 memory_order_acquire) == key)
			return &ht_instances[i];
	return NULL;
}

static struct ht_device *ht_find_device(void *key)
{
	for (int i = 0; i < HT_MAX_DEVICES; i++)
		if (atomic_load_explicit(&ht_devices[i].key,
					 memory_order_acquire) == key)
			return &ht_devices[i];
	return NULL;
}

/*
 * The queue's key is the device's key. If some ICD ever breaks that and there
 * is exactly one live device -- the case in every app that presents -- use it
 * rather than dropping the present on the floor, and count the miss so
 * HITCHTRACE_DEBUG=1 reports it.
 */
static struct ht_device *ht_device_for_queue(void *key)
{
	struct ht_device *only = NULL;
	int live = 0;

	for (int i = 0; i < HT_MAX_DEVICES; i++) {
		void *k = atomic_load_explicit(&ht_devices[i].key,
					       memory_order_acquire);
		if (k == key)
			return &ht_devices[i];
		if (k) {
			only = &ht_devices[i];
			live++;
		}
	}
	atomic_fetch_add_explicit(&ht_queue_key_misses, 1,
				  memory_order_relaxed);
	return live == 1 ? only : NULL;
}

static void ht_forget_instance(void *key)
{
	pthread_mutex_lock(&ht_table_lock);
	for (int i = 0; i < HT_MAX_INSTANCES; i++)
		if (atomic_load_explicit(&ht_instances[i].key,
					 memory_order_relaxed) == key)
			atomic_store_explicit(&ht_instances[i].key, NULL,
					      memory_order_release);
	pthread_mutex_unlock(&ht_table_lock);
}

static void ht_forget_device(void *key)
{
	pthread_mutex_lock(&ht_table_lock);
	for (int i = 0; i < HT_MAX_DEVICES; i++)
		if (atomic_load_explicit(&ht_devices[i].key,
					 memory_order_relaxed) == key)
			atomic_store_explicit(&ht_devices[i].key, NULL,
					      memory_order_release);
	pthread_mutex_unlock(&ht_table_lock);
}

/* ------------------------------------------------------------------ */
/* the hot path                                                        */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
ht_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	struct ht_device *dev = ht_device_for_queue(ht_key(queue));
	unsigned long frame_id;
	VkResult res;

	if (!dev)			/* see ht_device_for_queue() */
		return VK_ERROR_DEVICE_LOST;

	frame_id = atomic_fetch_add_explicit(&ht_frame_id, 1,
					     memory_order_relaxed);

	hitch_frame_mark(frame_id);		/* frame boundary, present in */
	res = dev->next_queue_present(queue, pPresentInfo);
	hitch_present_end(frame_id);		/* present out */

	return res;
}

/* ------------------------------------------------------------------ */
/* chain setup                                                         */
/* ------------------------------------------------------------------ */

static VkLayerInstanceCreateInfo *
ht_instance_chain_info(const VkInstanceCreateInfo *info, VkLayerFunction func)
{
	VkLayerInstanceCreateInfo *ci = (VkLayerInstanceCreateInfo *)info->pNext;

	while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
		       ci->function == func))
		ci = (VkLayerInstanceCreateInfo *)ci->pNext;
	return ci;
}

static VkLayerDeviceCreateInfo *
ht_device_chain_info(const VkDeviceCreateInfo *info, VkLayerFunction func)
{
	VkLayerDeviceCreateInfo *ci = (VkLayerDeviceCreateInfo *)info->pNext;

	while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
		       ci->function == func))
		ci = (VkLayerDeviceCreateInfo *)ci->pNext;
	return ci;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ht_GetInstanceProcAddr(VkInstance instance, const char *pName);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ht_GetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR VkResult VKAPI_CALL
ht_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
		  const VkAllocationCallbacks *pAllocator,
		  VkInstance *pInstance)
{
	VkLayerInstanceCreateInfo *chain =
		ht_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkCreateInstance next_create;
	struct ht_instance *slot = NULL;
	VkResult res;

	if (!chain || !chain->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* advance the chain for the next layer down before calling it */
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	res = next_create(pCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS)
		return res;

	pthread_mutex_lock(&ht_table_lock);
	for (int i = 0; i < HT_MAX_INSTANCES; i++) {
		if (!atomic_load_explicit(&ht_instances[i].key,
					  memory_order_relaxed)) {
			slot = &ht_instances[i];
			break;
		}
	}
	if (slot) {
		slot->instance = *pInstance;
		slot->next_gipa = next_gipa;
		slot->next_destroy_instance =
			(PFN_vkDestroyInstance)next_gipa(*pInstance,
							 "vkDestroyInstance");
		atomic_store_explicit(&slot->key, ht_key(*pInstance),
				      memory_order_release);
	}
	pthread_mutex_unlock(&ht_table_lock);

	if (!slot) {
		/*
		 * No slot: we cannot chain this instance's calls, and the
		 * loader will still route them through us. Fail loudly rather
		 * than hand the app NULL function pointers.
		 */
		PFN_vkDestroyInstance destroy =
			(PFN_vkDestroyInstance)next_gipa(*pInstance,
							 "vkDestroyInstance");
		if (destroy)
			destroy(*pInstance, pAllocator);
		*pInstance = VK_NULL_HANDLE;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/*
	 * Once per instance, not per frame: the only getenv in the layer.
	 * HITCHTRACE=1 is the loader's enable_environment, so being here at
	 * all already means the user asked for us.
	 */
	{
		const char *dbg = getenv("HITCHTRACE_DEBUG");

		if (dbg && dbg[0] == '1') {
			ht_debug = 1;
			clock_gettime(CLOCK_MONOTONIC, &ht_t0);
			fprintf(stderr,
				"hitchtrace layer: active pid=%d markers=hitch_frame_mark,hitch_present_end\n",
				(int)getpid());
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
ht_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	void *key = ht_key(instance);
	struct ht_instance *inst = ht_find_instance(key);
	PFN_vkDestroyInstance next = inst ? inst->next_destroy_instance : NULL;

	if (ht_debug) {
		struct timespec now;
		double secs;
		unsigned long frames =
			atomic_load_explicit(&hitch_frame_calls,
					     memory_order_relaxed);
		unsigned long ends =
			atomic_load_explicit(&hitch_present_calls,
					     memory_order_relaxed);
		unsigned long misses =
			atomic_load_explicit(&ht_queue_key_misses,
					     memory_order_relaxed);

		clock_gettime(CLOCK_MONOTONIC, &now);
		secs = (double)(now.tv_sec - ht_t0.tv_sec) +
		       (double)(now.tv_nsec - ht_t0.tv_nsec) / 1e9;
		fprintf(stderr,
			"hitchtrace layer: pid=%d frame_mark=%lu present_end=%lu "
			"last_frame_id=%lu queue_key_misses=%lu elapsed=%.3fs presents_per_s=%.1f\n",
			(int)getpid(), frames, ends, hitch_present_seen, misses,
			secs, secs > 0.0 ? (double)frames / secs : 0.0);
	}

	ht_forget_instance(key);
	if (next)
		next(instance, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL
ht_CreateDevice(VkPhysicalDevice physicalDevice,
		const VkDeviceCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	VkLayerDeviceCreateInfo *chain =
		ht_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
	struct ht_instance *inst = ht_find_instance(ht_key(physicalDevice));
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkGetDeviceProcAddr next_gdpa;
	PFN_vkCreateDevice next_create;
	struct ht_device *slot = NULL;
	VkResult res;

	if (!chain || !chain->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	next_create = (PFN_vkCreateDevice)
		next_gipa(inst ? inst->instance : NULL, "vkCreateDevice");
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;

	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	pthread_mutex_lock(&ht_table_lock);
	for (int i = 0; i < HT_MAX_DEVICES; i++) {
		if (!atomic_load_explicit(&ht_devices[i].key,
					  memory_order_relaxed)) {
			slot = &ht_devices[i];
			break;
		}
	}
	if (slot) {
		slot->device = *pDevice;
		slot->next_gdpa = next_gdpa;
		slot->next_destroy_device = (PFN_vkDestroyDevice)
			next_gdpa(*pDevice, "vkDestroyDevice");
		slot->next_queue_present = (PFN_vkQueuePresentKHR)
			next_gdpa(*pDevice, "vkQueuePresentKHR");
		atomic_store_explicit(&slot->key, ht_key(*pDevice),
				      memory_order_release);
	}
	pthread_mutex_unlock(&ht_table_lock);

	if (!slot) {
		PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)
			next_gdpa(*pDevice, "vkDestroyDevice");
		if (destroy)
			destroy(*pDevice, pAllocator);
		*pDevice = VK_NULL_HANDLE;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
ht_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	void *key = ht_key(device);
	struct ht_device *dev = ht_find_device(key);
	PFN_vkDestroyDevice next = dev ? dev->next_destroy_device : NULL;

	ht_forget_device(key);
	if (next)
		next(device, pAllocator);
}

/* ------------------------------------------------------------------ */
/* enumeration                                                         */
/* ------------------------------------------------------------------ */

static const VkLayerProperties ht_layer_props = {
	.layerName = HT_LAYER_NAME,
	.specVersion = VK_HEADER_VERSION_COMPLETE,
	.implementationVersion = HT_IMPL_VERSION,
	.description = HT_LAYER_DESCRIPTION,
};

static VkResult ht_copy_layer_props(uint32_t *pCount, VkLayerProperties *pProps)
{
	if (!pProps) {
		*pCount = 1;
		return VK_SUCCESS;
	}
	if (*pCount < 1) {
		*pCount = 0;
		return VK_INCOMPLETE;
	}
	*pCount = 1;
	pProps[0] = ht_layer_props;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ht_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
				    VkLayerProperties *pProperties)
{
	return ht_copy_layer_props(pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL
ht_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
				  uint32_t *pPropertyCount,
				  VkLayerProperties *pProperties)
{
	(void)physicalDevice;
	return ht_copy_layer_props(pPropertyCount, pProperties);
}

/* the layer adds no extensions of its own */
static VKAPI_ATTR VkResult VKAPI_CALL
ht_EnumerateInstanceExtensionProperties(const char *pLayerName,
					uint32_t *pPropertyCount,
					VkExtensionProperties *pProperties)
{
	(void)pProperties;
	if (pLayerName && !strcmp(pLayerName, HT_LAYER_NAME)) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}
	return VK_ERROR_LAYER_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ht_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
				      const char *pLayerName,
				      uint32_t *pPropertyCount,
				      VkExtensionProperties *pProperties)
{
	struct ht_instance *inst;
	PFN_vkEnumerateDeviceExtensionProperties next;

	if (pLayerName && !strcmp(pLayerName, HT_LAYER_NAME)) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	inst = ht_find_instance(ht_key(physicalDevice));
	if (!inst)
		return VK_ERROR_INITIALIZATION_FAILED;
	next = (PFN_vkEnumerateDeviceExtensionProperties)
		inst->next_gipa(inst->instance,
				"vkEnumerateDeviceExtensionProperties");
	if (!next)
		return VK_ERROR_INITIALIZATION_FAILED;
	return next(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

/* ------------------------------------------------------------------ */
/* proc addr                                                           */
/* ------------------------------------------------------------------ */

#define HT_PROC(name) \
	if (!strcmp(pName, "vk" #name)) \
		return (PFN_vkVoidFunction)ht_##name

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ht_GetDeviceProcAddr(VkDevice device, const char *pName)
{
	struct ht_device *dev;

	HT_PROC(GetDeviceProcAddr);
	HT_PROC(DestroyDevice);

	dev = device ? ht_find_device(ht_key(device)) : NULL;
	if (!dev)
		return NULL;

	/*
	 * Only hand out the hook if the chain below really has the function:
	 * a device without VK_KHR_swapchain must still see NULL here.
	 */
	if (!strcmp(pName, "vkQueuePresentKHR"))
		return dev->next_queue_present ?
			(PFN_vkVoidFunction)ht_QueuePresentKHR : NULL;

	return dev->next_gdpa(device, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ht_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
	struct ht_instance *inst;

	/* global entry points: answerable with instance == NULL */
	HT_PROC(GetInstanceProcAddr);
	HT_PROC(CreateInstance);
	HT_PROC(EnumerateInstanceLayerProperties);
	HT_PROC(EnumerateInstanceExtensionProperties);

	/* instance level */
	HT_PROC(DestroyInstance);
	HT_PROC(CreateDevice);
	HT_PROC(EnumerateDeviceLayerProperties);
	HT_PROC(EnumerateDeviceExtensionProperties);

	/* device level, reachable through the instance for convenience */
	HT_PROC(GetDeviceProcAddr);
	HT_PROC(DestroyDevice);
	if (!strcmp(pName, "vkQueuePresentKHR"))
		return (PFN_vkVoidFunction)ht_QueuePresentKHR;

	if (!instance)
		return NULL;
	inst = ht_find_instance(ht_key(instance));
	if (!inst)
		return NULL;
	return inst->next_gipa(instance, pName);
}

#undef HT_PROC

/* ------------------------------------------------------------------ */
/* loader interface                                                    */
/* ------------------------------------------------------------------ */

HT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (!pVersionStruct ||
	    pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion >
	    CURRENT_LOADER_LAYER_INTERFACE_VERSION)
		pVersionStruct->loaderLayerInterfaceVersion =
			CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
		pVersionStruct->pfnGetInstanceProcAddr = ht_GetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = ht_GetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
	}

	return VK_SUCCESS;
}

/*
 * Interface version 1 fallback, and what a loader that ignores negotiation
 * looks for by name.
 */
HT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	return ht_GetInstanceProcAddr(instance, pName);
}

HT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	return ht_GetDeviceProcAddr(device, pName);
}

HT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
				   VkLayerProperties *pProperties)
{
	return ht_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

HT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
				       uint32_t *pPropertyCount,
				       VkExtensionProperties *pProperties)
{
	return ht_EnumerateInstanceExtensionProperties(pLayerName,
						       pPropertyCount,
						       pProperties);
}

HT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
				 uint32_t *pPropertyCount,
				 VkLayerProperties *pProperties)
{
	return ht_EnumerateDeviceLayerProperties(physicalDevice, pPropertyCount,
						 pProperties);
}

HT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
				     const char *pLayerName,
				     uint32_t *pPropertyCount,
				     VkExtensionProperties *pProperties)
{
	return ht_EnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
						     pPropertyCount,
						     pProperties);
}
