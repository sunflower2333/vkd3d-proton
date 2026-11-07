/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Production functions are extracted unchanged. Only OS/Vulkan/resource peers
 * and their storage are controlled here; this is host ownership evidence. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <vulkan/vulkan.h>
#include "mesa_wddm_runtime.h"

typedef unsigned long ULONG;
typedef int HRESULT;
#define STDMETHODCALLTYPE
#define VKD3D_UNUSED __attribute__((unused))
#define VKD3D_CONFIG_FLAG_IS_SET(x) false
#define VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(d, c) ((void)(d), (void)(c))
#define VKD3D_COMMAND_BUFFER_SUBMIT_INFO_DEVICE_MASK_DEFAULT 0
#define VKD3D_COMMAND_BUFFER_SUBMIT_INFO_DEVICE_MASK_FALLBACK_QUEUE 1
#define VKD3D_COMMAND_COST_MERGE_THRESHOLD 100
#define VKD3D_WAIT_SEMAPHORES_EXTERNAL 1
#define VKD3D_WAIT_SEMAPHORES_SERIALIZING 2
#define VKD3D_SCRATCH_POOL_KIND_COUNT 1
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define FAILED(hr) ((hr) < 0)
#define E_OUTOFMEMORY (-1)
#define DXGI_ERROR_DEVICE_REMOVED (-2)
#define S_OK 0
#define TRACE(...) ((void)0)
#define INFO(...) ((void)0)
#define WARN(...) ((void)0)
#define VK_CALL(f) (vk_procs->f)
static unsigned errors;
#define ERR(...) (++errors)
static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%u: %s\n", __func__, __LINE__, #x); ++failures; } } while (0)
struct d3d12_device;
struct d3d12_command_queue;
struct vkd3d_fence_worker;
struct d3d12_command_allocator;
struct vkd3d_queue_timeline_trace_cookie { unsigned value; };
typedef void (*vkd3d_waiting_fence_callback)(struct vkd3d_fence_worker *, void *, bool);
struct vkd3d_fence_wait_info {
    VkSemaphore vk_semaphore; uint64_t vk_semaphore_value;
    vkd3d_waiting_fence_callback release_callback;
    union { uint64_t align; unsigned char userdata[64]; };
};
struct vkd3d_waiting_fence {
    struct vkd3d_fence_wait_info fence_info;
    struct vkd3d_queue_timeline_trace_cookie timeline_cookie;
};
struct vkd3d_waiting_fence_submission_info {
    struct d3d12_command_allocator **command_allocators;
    size_t num_command_allocators;
};
struct vkd3d_fence_worker {
    pthread_mutex_t mutex; pthread_cond_t cond;
    struct vkd3d_waiting_fence *enqueued_fences;
    size_t enqueued_fences_size, enqueued_fence_count;
    struct d3d12_device *device; struct d3d12_command_queue *queue;
};
struct vkd3d_queue {
    VkSemaphore submission_timeline;
    uint64_t submission_timeline_count;
    unsigned vk_family_index, acquires, releases;
    bool deny_acquire;
};
struct vkd3d_vk_device_procs {
    PFN_vkQueueSubmit2 vkQueueSubmit2;
    PFN_vkWaitSemaphores vkWaitSemaphores;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkResetCommandPool vkResetCommandPool;
};
struct d3d12_device {
    struct vkd3d_vk_device_procs vk_procs;
    void *wddm_runtime_owner; HRESULT removed;
    VkDevice vk_device;
    struct { bool NV_low_latency2; } vk_info;
    struct { struct vkd3d_queue *vkd3d_queue; } memory_transfers;
    struct { void *low_latency_swapchain; } swapchain_info;
    int low_latency_swapchain_spinlock, queue_timeline_trace, debug_ring;
    void *descriptor_qa_global_info;
    pthread_mutex_t mutex;
    struct { VkCommandPool vk_command_pool; unsigned vk_family_index; } cached_command_allocators[1];
    unsigned cached_command_allocator_count, references;
};
struct d3d12_command_allocator_command_pool {
    VkCommandPool vk_command_pool; unsigned vk_family_index;
    struct { unsigned command_buffer_count; VkCommandBuffer *command_buffers; } pending, recycled;
};
struct d3d12_command_allocator {
    unsigned internal_refcount, refcount;
    struct d3d12_device *device;
    int destruction_notifier, private_store;
    void *current_command_list;
    void *buffer_views, *views, *pipelines, *descriptor_heaps, *wddm_recording_owners, *meta_allocs;
    int *query_pools;
    unsigned query_pool_count;
    struct d3d12_command_allocator_command_pool primary_pool, fallback_pool;
    struct { unsigned scratch_buffer_count; int *scratch_buffers; } scratch_pools[VKD3D_SCRATCH_POOL_KIND_COUNT];
};
typedef struct d3d12_command_allocator ID3D12CommandAllocator;
struct d3d12_command_queue {
    struct d3d12_device *device;
    struct vkd3d_queue *vkd3d_queue;
    void *wddm_queue_token;
    bool stagger_submissions, serializing_semaphore_signaled;
    VkSemaphore serializing_semaphore;
    uint64_t last_submission_timeline_value;
    struct vkd3d_fence_worker fence_worker;
};
struct d3d12_command_queue_submission_execute {
    VkCommandBufferSubmitInfo *cmd; uint32_t *cmd_cost;
    unsigned cmd_count;
    struct d3d12_command_allocator **command_allocators; size_t num_command_allocators;
    bool split_submission; uint64_t low_latency_frame_id;
    struct vkd3d_queue_timeline_trace_cookie timeline_cookie;
};

static unsigned allocations, fail_allocation, driver_calls, fail_driver_call, notifications, fail_acquire;
static unsigned allocator_frees, array_frees, pool_frees, trace_completions;
static void *tracked_allocator, *tracked_arrays[4];
static uint64_t completed[8];
static bool use_stagger;
static void *vkd3d_calloc(size_t n, size_t size) {
    return ++allocations == fail_allocation ? NULL : calloc(n, size);
}
static void vkd3d_free(void *p) {
    if (p && p == tracked_allocator) { ++allocator_frees; tracked_allocator = NULL; }
    for (unsigned i = 0; i < ARRAY_SIZE(tracked_arrays); ++i)
        if (p && p == tracked_arrays[i]) { ++array_frees; tracked_arrays[i] = NULL; }
    free(p);
}
static bool vkd3d_array_reserve(void **p, size_t *size, size_t count, size_t elem) {
    if (*size >= count) return true;
    void *next = realloc(*p, count * elem);
    if (!next) return false;
    *p = next; *size = count; return true;
}
static HRESULT hresult_from_errno(int error) { return -error; }
static unsigned InterlockedDecrement(unsigned *p) { CHECK(*p); return --*p; }
static struct d3d12_command_allocator *impl_from_ID3D12CommandAllocator(ID3D12CommandAllocator *p) { return p; }
static void d3d_destruction_notifier_free(int *p) { (void)p; }
static void d3d_destruction_notifier_notify(int *p) { (void)p; ++notifications; }
static void vkd3d_private_store_destroy(int *p) { (void)p; }
static void d3d12_command_list_allocator_destroyed(void *p) { (void)p; }
static void d3d12_command_allocator_free_resources(struct d3d12_command_allocator *p) { (void)p; }
static void d3d12_device_return_scratch_buffer(struct d3d12_device *d, unsigned i, int *p) { (void)d; (void)i; (void)p; }
static void d3d12_device_return_query_pool(struct d3d12_device *d, void *p) { (void)d; (void)p; }
static void d3d12_device_release(struct d3d12_device *d) { CHECK(d->references); --d->references; }
static VkQueue vkd3d_queue_acquire(struct vkd3d_queue *q) { ++q->acquires; return q->deny_acquire || q->acquires == fail_acquire ? VK_NULL_HANDLE : (VkQueue)q; }
static void vkd3d_queue_release(struct vkd3d_queue *q) { ++q->releases; }
static VkFence vkd3d_queue_get_signal_fence_proxy_locked(struct vkd3d_queue *q) { (void)q; return VK_NULL_HANDLE; }
static bool d3d12_command_queue_needs_staggered_submissions(struct d3d12_command_queue *q) { (void)q; return use_stagger; }
static void d3d12_command_queue_wait_staggered_submission(struct d3d12_command_queue *q) { (void)q; }
static void d3d12_device_mark_as_removed(struct d3d12_device *d, HRESULT hr, const char *s, int v) { (void)s; (void)v; d->removed = hr; }
static void d3d12_command_queue_finalize_waits(struct d3d12_command_queue *q, VkResult v) { (void)q; (void)v; }
static void d3d12_command_queue_gather_wait_semaphores_locked(struct d3d12_command_queue *q, VkSubmitInfo2 *s, unsigned f) { (void)q; (void)s; (void)f; }
static void vkd3d_queue_timeline_trace_complete_execute(int *p, struct vkd3d_fence_worker *w, struct vkd3d_queue_timeline_trace_cookie c) { (void)p; (void)w; (void)c; ++trace_completions; }
static void vkd3d_queue_timeline_trace_begin_execute(int *p, struct vkd3d_queue_timeline_trace_cookie c) { (void)p; (void)c; }
static void vkd3d_queue_timeline_trace_begin_execute_overhead(int *p, struct vkd3d_queue_timeline_trace_cookie c) { (void)p; (void)c; }
static void vkd3d_queue_timeline_trace_end_execute_overhead(int *p, struct vkd3d_queue_timeline_trace_cookie c) { (void)p; (void)c; }
static void spinlock_acquire(int *p) { (void)p; }
static void spinlock_release(int *p) { (void)p; }
struct dxgi_vk_swap_chain;
static void dxgi_vk_swap_chain_incref(struct dxgi_vk_swap_chain *p) { (void)p; }
static void dxgi_vk_swap_chain_decref(struct dxgi_vk_swap_chain *p) { (void)p; }
static bool d3d12_device_has_slow_cpu_timeline_semaphores(struct d3d12_device *p) { (void)p; return false; }
static VkResult vkd3d_queue_wait_submission_timeline(struct vkd3d_queue *q, uint64_t v, uint64_t timeout) { (void)timeout; return completed[(uintptr_t)q->submission_timeline] >= v ? VK_SUCCESS : VK_TIMEOUT; }
static void vkd3d_queue_update_observed_cpu_timeline(struct vkd3d_queue *q, uint64_t v) { (void)q; (void)v; }
static void vkd3d_shader_debug_ring_kick(int *p, struct d3d12_device *d, bool b) { (void)p; (void)d; (void)b; }
static void vkd3d_descriptor_debug_kick_qa_check(void *p) { (void)p; }
static VkResult VKAPI_CALL submit(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) {
    (void)q; (void)f; CHECK(n && s); ++driver_calls;
    CHECK(s[0].pNext && ((const struct mwd_submit_info *)s[0].pNext)->sType == MWD_STYPE_SUBMIT);
    return driver_calls == fail_driver_call ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult VKAPI_CALL wait_semaphores(VkDevice d, const VkSemaphoreWaitInfo *s, uint64_t timeout) {
    (void)d; (void)timeout; CHECK(s->semaphoreCount == 1);
    CHECK(completed[(uintptr_t)s->pSemaphores[0]] >= s->pValues[0]);
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) ++pool_frees; }
static void VKAPI_CALL free_buffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *b) { (void)d; (void)p; (void)n; (void)b; }
static VkResult VKAPI_CALL reset_pool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; (void)p; (void)f; return VK_SUCCESS; }

// PRODUCTION_FUNCTIONS

static void init_queue(struct d3d12_command_queue *q, struct d3d12_device *d, struct vkd3d_queue *vk, unsigned semaphore) {
    memset(q, 0, sizeof(*q)); memset(vk, 0, sizeof(*vk));
    q->device = d; q->vkd3d_queue = vk; q->wddm_queue_token = q;
    vk->submission_timeline = (VkSemaphore)(uintptr_t)semaphore;
    q->fence_worker.device = d; q->fence_worker.queue = q;
    pthread_mutex_init(&q->fence_worker.mutex, NULL); pthread_cond_init(&q->fence_worker.cond, NULL);
}
static void destroy_queue(struct d3d12_command_queue *q) {
    CHECK(!q->fence_worker.enqueued_fence_count);
    free(q->fence_worker.enqueued_fences);
    pthread_mutex_destroy(&q->fence_worker.mutex); pthread_cond_destroy(&q->fence_worker.cond);
}
static struct d3d12_command_allocator *make_allocator(struct d3d12_device *d, unsigned submissions) {
    struct d3d12_command_allocator *a = calloc(1, sizeof(*a));
    a->device = d; a->refcount = 1; a->internal_refcount = 1 + submissions;
    a->primary_pool.vk_command_pool = (VkCommandPool)(uintptr_t)55;
    tracked_allocator = a; ++d->references; return a;
}
static struct d3d12_command_allocator **make_array(struct d3d12_command_allocator *a, unsigned slot) {
    struct d3d12_command_allocator **array = calloc(1, sizeof(*array));
    array[0] = a; CHECK(!tracked_arrays[slot]); tracked_arrays[slot] = array; return array;
}
static void run_execute(struct d3d12_command_queue *q, struct d3d12_command_allocator **array, bool split) {
    VkCommandBufferSubmitInfo commands[3] = {0}; uint32_t costs[3] = {100, 100, 100};
    for (unsigned i = 0; i < 3; ++i) { commands[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO; commands[i].commandBuffer = (VkCommandBuffer)(uintptr_t)(i + 1); }
    struct d3d12_command_queue_submission_execute exec = {.cmd = commands, .cmd_cost = costs, .cmd_count = 3,
        .command_allocators = array, .num_command_allocators = 1, .split_submission = split};
    VkCommandBufferSubmitInfo transition = {0}; VkSemaphoreSubmitInfo semaphore = {0};
    d3d12_command_queue_execute(q, &exec, &transition, &semaphore);
}
static void complete_worker(struct d3d12_command_queue *q) {
    completed[(uintptr_t)q->vkd3d_queue->submission_timeline] = q->vkd3d_queue->submission_timeline_count;
    for (size_t i = 0; i < q->fence_worker.enqueued_fence_count; ++i)
        vkd3d_wait_for_gpu_timeline_semaphore(&q->fence_worker, &q->fence_worker.enqueued_fences[i]);
    q->fence_worker.enqueued_fence_count = 0;
}
static void reset_observers(void) {
    CHECK(!tracked_allocator);
    for (unsigned i = 0; i < ARRAY_SIZE(tracked_arrays); ++i) CHECK(!tracked_arrays[i]);
    allocations = fail_allocation = driver_calls = fail_driver_call = notifications = fail_acquire = 0;
    allocator_frees = array_frees = pool_frees = trace_completions = errors = 0;
    memset(completed, 0, sizeof(completed)); use_stagger = false;
}
static void discard_unretired(struct d3d12_command_queue *q, struct d3d12_command_allocator *a, unsigned slot) {
    /* Test teardown after the controlled driver is idle. Not a production recovery path. */
    if (tracked_arrays[slot]) {
        struct vkd3d_waiting_fence_submission_info info = {tracked_arrays[slot], 1};
        vkd3d_waiting_fence_release_submission(&q->fence_worker, &info, true);
    }
    if (tracked_allocator && a->refcount) d3d12_command_allocator_Release(a);
    /* Repair only negative-control leaks so sanitizer noise cannot replace the assertion oracle. */
    if (tracked_allocator) { a->internal_refcount = 1; d3d12_command_allocator_dec_ref(a); }
}
int main(void) {
    struct d3d12_device d = {.vk_procs = {submit, wait_semaphores, destroy_pool, free_buffers, reset_pool}, .references = 1};
    d.wddm_runtime_owner = &d;
    struct d3d12_command_queue q, second; struct vkd3d_queue vk, vk_second;
    for (unsigned split = 0; split < 2; ++split) for (unsigned allocation = 1; allocation <= 2; ++allocation) {
        reset_observers(); init_queue(&q, &d, &vk, 1);
        struct d3d12_command_allocator *a = make_allocator(&d, 1);
        struct d3d12_command_allocator **array = make_array(a, 0);
        fail_allocation = allocation; run_execute(&q, array, split);
        CHECK(!driver_calls && !q.fence_worker.enqueued_fence_count);
        CHECK(a->internal_refcount == 1 && array_frees == 1 && trace_completions == 1);
        d3d12_command_allocator_Release(a);
        CHECK(allocator_frees == 1 && pool_frees == 1 && notifications == 1 && errors >= 1);
        discard_unretired(&q, a, 0); destroy_queue(&q);
    }
    puts("PASS production Execute routing allocation failures: direct/split, both allocations, exact ref/array/pool cleanup");
    reset_observers(); init_queue(&q, &d, &vk, 1);
    struct d3d12_command_allocator *initial = make_allocator(&d, 1);
    vk.deny_acquire = true; run_execute(&q, make_array(initial, 0), false);
    CHECK(!driver_calls && !q.fence_worker.enqueued_fence_count && initial->internal_refcount == 1 && array_frees == 1);
    d3d12_command_allocator_Release(initial);
    CHECK(allocator_frees == 1 && pool_frees == 1);
    discard_unretired(&q, initial, 0); destroy_queue(&q);
    puts("PASS production initial queue acquire failure releases never-submitted allocator");
    reset_observers(); init_queue(&q, &d, &vk, 1);
    struct d3d12_command_allocator *a = make_allocator(&d, 1);
    run_execute(&q, make_array(a, 0), false);
    CHECK(driver_calls == 1 && q.fence_worker.enqueued_fence_count == 1 && !array_frees);
    d3d12_command_allocator_Release(a);
    CHECK(!allocator_frees && !pool_frees && a->internal_refcount == 1 && errors == 1);
    complete_worker(&q);
    CHECK(allocator_frees == 1 && array_frees == 1 && pool_frees == 1 && trace_completions == 1);
    discard_unretired(&q, a, 0); destroy_queue(&q);
    puts("PASS production delayed worker: public Release keeps original ERR and retains pool until actual callback");
    reset_observers(); init_queue(&q, &d, &vk, 1); init_queue(&second, &d, &vk_second, 2);
    a = make_allocator(&d, 2);
    run_execute(&q, make_array(a, 0), false); run_execute(&second, make_array(a, 1), true);
    d3d12_command_allocator_Release(a); complete_worker(&q);
    CHECK(!allocator_frees && a->internal_refcount == 1 && array_frees == 1);
    complete_worker(&second);
    CHECK(allocator_frees == 1 && array_frees == 2 && pool_frees == 1 && trace_completions == 2);
    discard_unretired(&q, a, 0); destroy_queue(&q); destroy_queue(&second);
    puts("PASS production two-queue ownership: first callback retains shared allocator, final callback frees once");
    for (unsigned partial = 0; partial < 2; ++partial) {
        reset_observers(); init_queue(&q, &d, &vk, 1); a = make_allocator(&d, 1);
        if (partial) fail_allocation = 3; else fail_driver_call = 1;
        run_execute(&q, make_array(a, 0), partial);
        CHECK(driver_calls == 1 && !q.fence_worker.enqueued_fence_count);
        CHECK(a->internal_refcount == 2 && !array_frees && !allocator_frees);
        discard_unretired(&q, a, 0); destroy_queue(&q);
    }
    puts("PASS attempted/partial-submit guard: new cleanup never frees potentially in-flight allocator; recovery remains unqualified");
    for (unsigned split = 0; split < 2; ++split) {
        reset_observers(); init_queue(&q, &d, &vk, 1); use_stagger = true;
        a = make_allocator(&d, 1); fail_allocation = 3;
        run_execute(&q, make_array(a, 0), split);
        CHECK(driver_calls == 1 && a->internal_refcount == 2 && !array_frees && !q.fence_worker.enqueued_fence_count);
        discard_unretired(&q, a, 0); destroy_queue(&q);
    }
    reset_observers(); init_queue(&q, &d, &vk, 1); use_stagger = true;
    a = make_allocator(&d, 1); fail_acquire = 2;
    run_execute(&q, make_array(a, 0), false);
    CHECK(driver_calls == 1 && a->internal_refcount == 2 && !array_frees && !q.fence_worker.enqueued_fence_count);
    discard_unretired(&q, a, 0); destroy_queue(&q);
    puts("PASS staggered partial/enqueue and reacquire failure guards retain attempted work; recovery remains unqualified");
    CHECK(d.references == 1);
    printf("production allocator submission ownership: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
