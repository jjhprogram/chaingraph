// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * minigame: a small game-shaped Vulkan workload for hitchtrace case studies.
 *
 *	mg_main --jobs--> mg_worker0..N-1
 *	   |
 *	   +-- vkQueuePresentKHR()   <- frame marks come from the hitchtrace layer
 *
 * It has no stall injectors. A frame does what a simple game does:
 *
 *	limiter   sleep to the frame cap (absolute deadline, 60 fps by default)
 *	sim       move the asteroids (main thread, cheap)
 *	jobs      split the screen into tiles, hand them to the worker pool
 *		  through one mutex-protected queue, and wait for all of them;
 *		  each tile is drawn in software (per-pixel, per-object)
 *	upload    copy the finished frame into a Vulkan staging buffer and
 *		  record/submit one buffer-to-swapchain-image copy
 *	present   vkQueuePresentKHR
 *
 * so whatever makes a frame late has to come from the environment the case
 * study sets up around it: CPU contention, slow storage, memory pressure.
 *
 * Drawing in software and presenting through Vulkan is deliberate. It needs
 * no shader compiler, it gives the workers real CPU work to be preempted
 * in, and on lavapipe it keeps the driver's own rasterization out of the
 * present call.
 *
 * The game also keeps its own per-frame phase timings (-o FILE, JSONL) --
 * what an engine's built-in profiler would show for the same frames -- so a
 * case study can compare the two views.
 *
 * usage: minigame [-d SECONDS] [-w WORKERS] [-f FPS] [-n OBJECTS]
 *                 [-t TILES] [-o FRAMELOG] [-h]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#define WIN_W		640
#define WIN_H		360
#define MAX_WORKERS	16
#define MAX_TILES	256
#define MAX_OBJECTS	512
#define NSEC		1000000000ULL

static int opt_duration = 20;
static int opt_workers = 3;
static int opt_fps = 60;
static int opt_objects = 48;
static int opt_tiles = 24;
static const char *opt_framelog;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "minigame: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NSEC + ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / NSEC, .tv_nsec = t % NSEC };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* ------------------------------------------------------------------ game */

struct object {
	float x, y, vx, vy, r;
	uint8_t cr, cg, cb;
};

static struct object objects[MAX_OBJECTS];
static uint32_t fb_w, fb_h;
static uint32_t *fb;		/* BGRA, what the workers draw into */

static uint32_t rng_state = 0x9e3779b9;

static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static float frand(float lo, float hi)
{
	return lo + (hi - lo) * (rng() >> 8) / (float)(1 << 24);
}

static void game_init(void)
{
	int i;

	for (i = 0; i < opt_objects; i++) {
		struct object *o = &objects[i];

		o->x = frand(0, fb_w);
		o->y = frand(0, fb_h);
		o->vx = frand(-90, 90);
		o->vy = frand(-90, 90);
		o->r = frand(8, 30);
		o->cr = 120 + rng() % 120;
		o->cg = 110 + rng() % 100;
		o->cb = 90 + rng() % 80;
	}
}

static void game_sim(float dt)
{
	int i;

	for (i = 0; i < opt_objects; i++) {
		struct object *o = &objects[i];

		o->x += o->vx * dt;
		o->y += o->vy * dt;
		if (o->x < -o->r)
			o->x += fb_w + 2 * o->r;
		if (o->x > fb_w + o->r)
			o->x -= fb_w + 2 * o->r;
		if (o->y < -o->r)
			o->y += fb_h + 2 * o->r;
		if (o->y > fb_h + o->r)
			o->y -= fb_h + 2 * o->r;
	}
}

/* Draw rows [y0, y1) of the frame: a star field with lit asteroids over it. */
static void draw_tile(uint32_t y0, uint32_t y1)
{
	uint32_t x, y;
	int i;

	for (y = y0; y < y1; y++) {
		for (x = 0; x < fb_w; x++) {
			uint32_t h = (x * 73856093u) ^ (y * 19349663u);
			uint8_t r = 6, g = 8, b = 18;

			if ((h % 617) == 0)
				r = g = b = 150 + h % 100;
			for (i = 0; i < opt_objects; i++) {
				const struct object *o = &objects[i];
				float dx = x - o->x, dy = y - o->y;
				float d2 = dx * dx + dy * dy, rr = o->r * o->r;
				float nz, lit;

				if (d2 >= rr)
					continue;
				/* a sphere lit from the upper left */
				nz = sqrtf(rr - d2) / o->r;
				lit = 0.25f + 0.75f * fmaxf(0, (-dx - dy) / (1.5f * o->r) * 0.6f + nz * 0.7f);
				if (lit > 1)
					lit = 1;
				r = o->cr * lit;
				g = o->cg * lit;
				b = o->cb * lit;
			}
			fb[y * fb_w + x] = 0xff000000u | r << 16 | g << 8 | b;
		}
	}
}

/* ------------------------------------------------------------ job system */

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static int q_jobs[MAX_TILES], q_head, q_tail, q_pending;
static bool q_quit;

static void *worker(void *arg)
{
	char name[16];

	snprintf(name, sizeof(name), "mg_worker%ld", (long)arg);
	pthread_setname_np(pthread_self(), name);

	for (;;) {
		int tile;

		pthread_mutex_lock(&q_lock);
		while (q_head == q_tail && !q_quit)
			pthread_cond_wait(&q_cv, &q_lock);
		if (q_quit) {
			pthread_mutex_unlock(&q_lock);
			return NULL;
		}
		tile = q_jobs[q_head++];
		pthread_mutex_unlock(&q_lock);

		draw_tile(fb_h * tile / opt_tiles, fb_h * (tile + 1) / opt_tiles);

		pthread_mutex_lock(&q_lock);
		if (--q_pending == 0)
			pthread_cond_signal(&done_cv);
		pthread_mutex_unlock(&q_lock);
	}
}

/* Hand every tile to the pool and wait until the frame is drawn. */
static void run_jobs(void)
{
	int i;

	pthread_mutex_lock(&q_lock);
	q_head = q_tail = 0;
	for (i = 0; i < opt_tiles; i++)
		q_jobs[q_tail++] = i;
	q_pending = opt_tiles;
	pthread_cond_broadcast(&q_cv);
	while (q_pending)
		pthread_cond_wait(&done_cv, &q_lock);
	pthread_mutex_unlock(&q_lock);
}

/* ---------------------------------------------------------------- vulkan */

static xcb_connection_t *xcb;
static xcb_window_t window;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage sc_images[8];
static uint32_t sc_count;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkBuffer staging;
static VkDeviceMemory staging_mem;
static void *staging_map;
static VkSemaphore sem_acquired, sem_drawn;
static VkFence fence;

#define VK(call) do {							\
	VkResult vk_res__ = (call);					\
	if (vk_res__ != VK_SUCCESS)					\
		die("%s failed: %d", #call, vk_res__);			\
} while (0)

static void window_init(void)
{
	const xcb_setup_t *setup;
	xcb_screen_t *screen;
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t values[2];

	xcb = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(xcb))
		die("cannot connect to the X server (is DISPLAY set?)");
	setup = xcb_get_setup(xcb);
	screen = xcb_setup_roots_iterator(setup).data;
	values[0] = screen->black_pixel;
	values[1] = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	window = xcb_generate_id(xcb);
	xcb_create_window(xcb, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0,
			  WIN_W, WIN_H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  screen->root_visual, mask, values);
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
			    XCB_ATOM_STRING, 8, 8, "minigame");
	xcb_map_window(xcb, window);
	xcb_flush(xcb);
}

static void window_poll(void)
{
	xcb_generic_event_t *ev;

	while ((ev = xcb_poll_for_event(xcb)))
		free(ev);
}

static uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (i = 0; i < mp.memoryTypeCount; i++)
		if ((type_bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	die("no suitable memory type");
	return 0;
}

static void vulkan_init(void)
{
	const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
				   VK_KHR_XCB_SURFACE_EXTENSION_NAME };
	const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "minigame",
		.apiVersion = VK_API_VERSION_1_0,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = inst_ext,
	};
	VkXcbSurfaceCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = xcb,
		.window = window,
	};
	VkQueueFamilyProperties qf[16];
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR formats[64];
	VkSurfaceFormatKHR format;
	VkMemoryRequirements mr;
	uint32_t n, i;
	float prio = 1;

	VK(vkCreateInstance(&ici, NULL, &inst));
	n = 1;
	if (vkEnumeratePhysicalDevices(inst, &n, &phys) < 0 || n == 0)
		die("no Vulkan device");
	VK(vkCreateXcbSurfaceKHR(inst, &sci, NULL, &surface));

	n = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf);
	for (i = 0; i < n; i++) {
		VkBool32 ok = VK_FALSE;

		vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &ok);
		if (ok && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break;
	}
	if (i == n)
		die("no queue family can present");
	qfam = i;

	{
		VkDeviceQueueCreateInfo qci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = qfam,
			.queueCount = 1,
			.pQueuePriorities = &prio,
		};
		VkDeviceCreateInfo dci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &qci,
			.enabledExtensionCount = 1,
			.ppEnabledExtensionNames = dev_ext,
		};

		VK(vkCreateDevice(phys, &dci, NULL, &dev));
	}
	/* resolved through the device, as engines do: this is the path that
	 * bypasses the loader's exported vkQueuePresentKHR */
	vkGetDeviceQueue(dev, qfam, 0, &queue);

	VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		die("swapchain images cannot be copied into");
	n = 64;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats) < 0 || !n)
		die("no surface format");
	format = formats[0];
	for (i = 0; i < n; i++)
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
			format = formats[i];
	fb_w = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : WIN_W;
	fb_h = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : WIN_H;

	{
		VkSwapchainCreateInfoKHR sc = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = surface,
			.minImageCount = caps.minImageCount + 1,
			.imageFormat = format.format,
			.imageColorSpace = format.colorSpace,
			.imageExtent = { fb_w, fb_h },
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = caps.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = VK_PRESENT_MODE_FIFO_KHR,
			.clipped = VK_TRUE,
		};

		if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount)
			sc.minImageCount = caps.maxImageCount;
		VK(vkCreateSwapchainKHR(dev, &sc, NULL, &swapchain));
	}
	sc_count = 8;
	if (vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_images) < 0)
		die("vkGetSwapchainImagesKHR failed");

	{
		VkCommandPoolCreateInfo pci = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = qfam,
		};
		VkCommandBufferAllocateInfo cai = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkBufferCreateInfo bci = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = (VkDeviceSize)fb_w * fb_h * 4,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VkMemoryAllocateInfo mai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		};
		VkSemaphoreCreateInfo semci = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		VkFenceCreateInfo fci = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};

		VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
		cai.commandPool = pool;
		VK(vkAllocateCommandBuffers(dev, &cai, &cmd));

		VK(vkCreateBuffer(dev, &bci, NULL, &staging));
		vkGetBufferMemoryRequirements(dev, staging, &mr);
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = find_memory(mr.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VK(vkAllocateMemory(dev, &mai, NULL, &staging_mem));
		VK(vkBindBufferMemory(dev, staging, staging_mem, 0));
		VK(vkMapMemory(dev, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_map));

		VK(vkCreateSemaphore(dev, &semci, NULL, &sem_acquired));
		VK(vkCreateSemaphore(dev, &semci, NULL, &sem_drawn));
		VK(vkCreateFence(dev, &fci, NULL, &fence));
	}
}

static void image_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
			  VkAccessFlags src_access, VkAccessFlags dst_access,
			  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = dst_access,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Copy the finished frame to the next swapchain image; returns its index. */
static uint32_t upload_frame(void)
{
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { fb_w, fb_h, 1 },
	};
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &sem_acquired,
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &sem_drawn,
	};
	uint32_t idx;

	/* the previous frame's copy must be done before its buffers are reused */
	VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
	VK(vkResetFences(dev, 1, &fence));
	memcpy(staging_map, fb, (size_t)fb_w * fb_h * 4);

	VK(vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquired,
				 VK_NULL_HANDLE, &idx));
	VK(vkResetCommandBuffer(cmd, 0));
	VK(vkBeginCommandBuffer(cmd, &bi));
	image_barrier(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED,
		      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		      VK_ACCESS_TRANSFER_WRITE_BIT,
		      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyBufferToImage(cmd, staging, sc_images[idx],
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	image_barrier(sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		      VK_ACCESS_TRANSFER_WRITE_BIT, 0,
		      VK_PIPELINE_STAGE_TRANSFER_BIT,
		      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	VK(vkEndCommandBuffer(cmd));
	VK(vkQueueSubmit(queue, 1, &si, fence));
	return idx;
}

static void present_frame(uint32_t idx)
{
	VkPresentInfoKHR pi = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &sem_drawn,
		.swapchainCount = 1,
		.pSwapchains = &swapchain,
		.pImageIndices = &idx,
	};
	VkResult res = vkQueuePresentKHR(queue, &pi);

	if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		die("vkQueuePresentKHR failed: %d", res);
}

/* ------------------------------------------------------- the frame loop */

/*
 * The game's own view of a frame, as an engine profiler would record it.
 * A frame is present entry to present entry, the same window hitchtrace uses,
 * so the phases of frame N are: present (the call that opened it), limiter,
 * sim, jobs, upload.
 */
struct frame_log {
	uint64_t t_end_ns;	/* present entry that closed this frame */
	uint64_t frame_ns, present_ns, limiter_ns, sim_ns, jobs_ns, upload_ns;
};

static void usage(void)
{
	fprintf(stderr,
		"usage: minigame [-d SECONDS] [-w WORKERS] [-f FPS] [-n OBJECTS]\n"
		"                [-t TILES] [-o FRAMELOG]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	pthread_t workers[MAX_WORKERS];
	struct frame_log *log, cur = { 0 };
	uint64_t period, t_stop, deadline, t_prev_present = 0, t0, t1;
	size_t nlog = 0, caplog;
	uint32_t idx;
	long i;
	int c;

	while ((c = getopt(argc, argv, "d:w:f:n:t:o:h")) != -1) {
		switch (c) {
		case 'd': opt_duration = atoi(optarg); break;
		case 'w': opt_workers = atoi(optarg); break;
		case 'f': opt_fps = atoi(optarg); break;
		case 'n': opt_objects = atoi(optarg); break;
		case 't': opt_tiles = atoi(optarg); break;
		case 'o': opt_framelog = optarg; break;
		default: usage();
		}
	}
	if (opt_workers < 1 || opt_workers > MAX_WORKERS || opt_fps < 1 ||
	    opt_objects < 1 || opt_objects > MAX_OBJECTS ||
	    opt_tiles < 1 || opt_tiles > MAX_TILES || opt_duration < 1)
		usage();

	pthread_setname_np(pthread_self(), "mg_main");
	window_init();
	vulkan_init();
	fb = calloc((size_t)fb_w * fb_h, 4);
	if (!fb)
		die("out of memory");
	game_init();
	for (i = 0; i < opt_workers; i++)
		if (pthread_create(&workers[i], NULL, worker, (void *)i))
			die("cannot start a worker");

	period = NSEC / opt_fps;
	caplog = (size_t)opt_duration * opt_fps * 4 + 64;
	log = calloc(caplog, sizeof(*log));
	if (!log)
		die("out of memory");

	fprintf(stderr, "minigame pid=%d %ux%u workers=%d tiles=%d objects=%d "
		"cap=%d fps duration=%d s\n", getpid(), fb_w, fb_h, opt_workers,
		opt_tiles, opt_objects, opt_fps, opt_duration);

	deadline = now_ns();
	t_stop = deadline + (uint64_t)opt_duration * NSEC;
	while (now_ns() < t_stop) {
		/* limiter: an absolute grid, restarted after an overrun */
		t0 = now_ns();
		deadline += period;
		if (deadline < t0)
			deadline = t0;
		else
			sleep_until(deadline);
		t1 = now_ns();
		cur.limiter_ns = t1 - t0;

		window_poll();
		game_sim(1.0f / opt_fps);
		t0 = now_ns();
		cur.sim_ns = t0 - t1;

		run_jobs();
		t1 = now_ns();
		cur.jobs_ns = t1 - t0;

		idx = upload_frame();
		t0 = now_ns();
		cur.upload_ns = t0 - t1;

		/* present entry: this frame ends, the next begins */
		if (t_prev_present && nlog < caplog) {
			cur.t_end_ns = t0;
			cur.frame_ns = t0 - t_prev_present;
			log[nlog++] = cur;
		}
		t_prev_present = t0;
		present_frame(idx);
		cur.present_ns = now_ns() - t0;
	}

	pthread_mutex_lock(&q_lock);
	q_quit = true;
	pthread_cond_broadcast(&q_cv);
	pthread_mutex_unlock(&q_lock);
	for (i = 0; i < opt_workers; i++)
		pthread_join(workers[i], NULL);
	vkDeviceWaitIdle(dev);

	if (opt_framelog) {
		FILE *f = fopen(opt_framelog, "w");
		size_t k;

		if (!f)
			die("cannot write %s: %s", opt_framelog, strerror(errno));
		for (k = 0; k < nlog; k++)
			fprintf(f, "{\"frame\":%zu,\"t_end_ns\":%llu,\"frame_us\":%llu,"
				"\"present_us\":%llu,\"limiter_us\":%llu,\"sim_us\":%llu,"
				"\"jobs_us\":%llu,\"upload_us\":%llu}\n", k,
				(unsigned long long)log[k].t_end_ns,
				(unsigned long long)log[k].frame_ns / 1000,
				(unsigned long long)log[k].present_ns / 1000,
				(unsigned long long)log[k].limiter_ns / 1000,
				(unsigned long long)log[k].sim_ns / 1000,
				(unsigned long long)log[k].jobs_ns / 1000,
				(unsigned long long)log[k].upload_ns / 1000);
		fclose(f);
	}
	fprintf(stderr, "minigame: %zu frames\n", nlog);
	return 0;
}
