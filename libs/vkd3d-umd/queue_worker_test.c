/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include "mesa_wddm_runtime.h"
typedef int VkResult;
typedef int HRESULT;
typedef uintptr_t VkQueue;
typedef uintptr_t VkFence;
typedef struct { int sType; const void *pNext; uint32_t commandBufferInfoCount;
   const int *pCommandBufferInfos; uint32_t waitSemaphoreInfoCount, signalSemaphoreInfoCount;
   const void *pWaitSemaphoreInfos, *pSignalSemaphoreInfos; } VkSubmitInfo2;
enum { VK_SUCCESS = 0, S_OK = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1,
       VK_ERROR_INITIALIZATION_FAILED = -3, DXGI_ERROR_DEVICE_REMOVED = -4,
       VK_STRUCTURE_TYPE_SUBMIT_INFO_2 = 1, VK_NULL_HANDLE = 0, VKD3D_SUBMISSION_DRAIN = 4 };
struct vkd3d_vk_device_procs { VkResult (*vkQueueSubmit2)(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence); };
struct d3d12_device { struct vkd3d_vk_device_procs vk_procs; void *wddm_runtime_owner; HRESULT removed; };
struct d3d12_command_queue_submission { int type; };
struct d3d12_command_queue {
   struct d3d12_device *device; void *wddm_queue_token;
   pthread_mutex_t queue_lock; pthread_cond_t queue_cond;
   uint64_t drain_count, queue_drain_count;
   bool requested;
};
typedef struct d3d12_command_queue ID3D12CommandQueue;
#define VK_CALL(f) (vk_procs->f)
#define ERR(...) ((void)0)
static int failures, submissions, allocations, fail_allocation;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void *vkd3d_calloc(size_t n, size_t size) {
   if (++allocations == fail_allocation) return NULL;
   return calloc(n, size);
}
static void vkd3d_free(void *ptr) { free(ptr); }
static void d3d12_device_mark_as_removed(struct d3d12_device *device, HRESULT hr, const char *fmt, int code) {
   (void)fmt; (void)code; device->removed = hr;
}
static HRESULT d3d12_device_removed_reason(struct d3d12_device *device) { return device->removed; }
static struct d3d12_command_queue *impl_from_ID3D12CommandQueue(ID3D12CommandQueue *queue) { return queue; }
static void d3d12_command_queue_add_submission_locked(struct d3d12_command_queue *queue,
       const struct d3d12_command_queue_submission *submission) {
   CHECK(submission->type == VKD3D_SUBMISSION_DRAIN);
   queue->requested = true;
   pthread_cond_broadcast(&queue->queue_cond);
}
// PRODUCTION_FUNCTIONS

static void *expected_token, *expected_owner;
static const void *expected_next;
static VkFence expected_fence;
static VkResult submit_result;
static VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo2 *infos, VkFence fence) {
   CHECK(queue == 123 && count && fence == expected_fence);
   ++submissions;
   for (uint32_t i = 0; i < count; ++i) {
      if (!expected_token) { CHECK(infos[i].pNext == expected_next); continue; }
      const struct mwd_submit_info *route = infos[i].pNext;
      CHECK(route && route->sType == MWD_STYPE_SUBMIT);
      if (!route || route->sType != MWD_STYPE_SUBMIT) continue;
      CHECK(route->queue == expected_token && route->owner == expected_owner && route->pNext == expected_next);
      CHECK(infos[i].commandBufferInfoCount >= 1);
   }
   return submit_result;
}
static atomic_bool enqueued;
static void *worker(void *opaque) {
   struct d3d12_command_queue *queue = opaque;
   pthread_mutex_lock(&queue->queue_lock);
   while (!queue->requested) pthread_cond_wait(&queue->queue_cond, &queue->queue_lock);
   pthread_mutex_unlock(&queue->queue_lock);
   struct timespec delay = {0, 25000000}; nanosleep(&delay, NULL);
   // No GPU event is ever signaled. A correct drain only waits software enqueue.
   pthread_mutex_lock(&queue->queue_lock);
   atomic_store(&enqueued, true); ++queue->queue_drain_count;
   pthread_cond_broadcast(&queue->queue_cond);
   pthread_mutex_unlock(&queue->queue_lock);
   return NULL;
}
int main(void) {
   struct d3d12_device device = {{submit}, NULL, 0};
   struct d3d12_command_queue first = {.device = &device};
   struct d3d12_command_queue second = {.device = &device};
   struct mwd_submit_info sentinel = {.sType = 567};
   int commands[3] = {1, 2, 3};
   VkSubmitInfo2 info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = &sentinel,
      .commandBufferInfoCount = 3, .pCommandBufferInfos = commands};
   expected_next = &sentinel;
   CHECK(d3d12_command_queue_submit_wddm(&first, 123, 1, &info, 0) == VK_SUCCESS);
   device.wddm_runtime_owner = expected_owner = &device;
   first.wddm_queue_token = &first; second.wddm_queue_token = &second;
   expected_token = &first;
   CHECK(d3d12_command_queue_submit_wddm(&first, 123, 1, &info, 0) == VK_SUCCESS);
   CHECK(info.pNext == &sentinel && !sentinel.pNext);
   expected_token = &second;
   CHECK(d3d12_command_queue_submit_wddm(&second, 123, 1, &info, 0) == VK_SUCCESS);
   int before = submissions;
   CHECK(d3d12_command_queue_submit_split_locked(&second, 123, 1, &info, 0) == VK_SUCCESS);
   CHECK(submissions == before + 3);
   before = submissions; fail_allocation = allocations + 1;
   CHECK(d3d12_command_queue_submit_wddm(&second, 123, 1, &info, 0) == VK_ERROR_OUT_OF_HOST_MEMORY);
   CHECK(submissions == before);
   CHECK(d3d12_command_queue_submit_wddm(&second, 123, 0, &info, 0) == VK_ERROR_INITIALIZATION_FAILED);
   submit_result = -4;
   CHECK(d3d12_command_queue_submit_wddm(&second, 123, 1, &info, 0) == -4 && device.removed == -4);
   device.removed = 0;
   pthread_mutex_init(&first.queue_lock, NULL); pthread_cond_init(&first.queue_cond, NULL);
   pthread_t thread; pthread_create(&thread, NULL, worker, &first);
   CHECK(vkd3d_wddm_queue_drain_enqueue(&first) == S_OK);
   CHECK(atomic_load(&enqueued));
   // Also allow the intentionally skipped-drain control to exit normally.
   pthread_mutex_lock(&first.queue_lock); first.requested = true;
   pthread_cond_broadcast(&first.queue_cond); pthread_mutex_unlock(&first.queue_lock);
   pthread_join(thread, NULL);
   pthread_mutex_destroy(&first.queue_lock); pthread_cond_destroy(&first.queue_cond);
   printf("production logical queue routing/split/enqueue drain: %s\n", failures ? "FAIL" : "PASS");
   return failures != 0;
}
