/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real embedded backend events and queue work, never system D3D12 acceptance. */
#include "backend.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
static constexpr int32_t timeout_hr = static_cast<int32_t>(0x887a000a);
static constexpr int32_t cancelled_hr = static_cast<int32_t>(0x800703e3);
static constexpr int32_t removed_hr = static_cast<int32_t>(0x887a0005);
static void require(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "FAIL real fence event: %s\n", what); std::exit(1); }
}
#define CHECK(expr) require((expr) >= 0, #expr)

int main() {
#ifdef _WIN32
    HMODULE library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto loader = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr")) : nullptr;
#else
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    auto loader = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
#endif
    require(loader != nullptr, "Vulkan loader");
    vkdu_device *device = nullptr, *foreign = nullptr;
    CHECK(vkdu_test_device_create(loader, &device)); CHECK(vkdu_test_device_create(loader, &foreign));
    vkdu_object *producer = nullptr, *consumer = nullptr, *gate = nullptr, *done = nullptr, *foreign_fence = nullptr;
    CHECK(vkdu_queue_create(device, 0, &producer)); CHECK(vkdu_queue_create(device, 0, &consumer));
    CHECK(vkdu_fence_create(device, 0, &gate)); CHECK(vkdu_fence_create(device, 0, &done));
    CHECK(vkdu_fence_create(foreign, 0, &foreign_fence));
    require(vkdu_queue_signal(producer, foreign_fence, 1) < 0, "cross-device signal rejected");
    require(vkdu_queue_wait(consumer, foreign_fence, 1) < 0, "cross-device wait rejected");
    require(vkdu_queue_signal(producer, done, UINT64_MAX) < 0, "reserved signal rejected");
    vkdu_fence_event *event = nullptr;
    CHECK(vkdu_fence_event_create(done, 1, &event));
    CHECK(vkdu_queue_wait(consumer, gate, 1));
    CHECK(vkdu_queue_signal(consumer, done, 1));
    require(vkdu_fence_event_wait(event, 30) == timeout_hr, "future queue wait must block completion");
    uint64_t value = UINT64_MAX;
    CHECK(vkdu_fence_completed(done, &value)); require(value == 0, "pending value cannot count as completion");
    CHECK(vkdu_queue_signal(producer, gate, 1));
    CHECK(vkdu_fence_event_wait(event, 5000));
    CHECK(vkdu_fence_completed(done, &value)); require(value == 1, "real backend queue completion");
    CHECK(vkdu_fence_signal_cpu(done, 0));
    CHECK(vkdu_fence_event_wait(event, 0)); // The earlier completion remains latched across a rewind.
    vkdu_fence_event_destroy(event);
    puts("PASS real fence event: delayed queue wait/signal, completion value and rewind latch");

    // Destroying a registration before its signal must unlink its event under
    // the actual fence lock. Handle reuse must never spuriously wake a new wait.
    for (unsigned i = 1; i <= 64; ++i) {
        vkdu_fence_event *old = nullptr, *replacement = nullptr;
        CHECK(vkdu_fence_event_create(done, i * 2, &old));
        require(vkdu_fence_event_wait(old, 0) == timeout_hr, "old event initially unsignaled");
        vkdu_fence_event_destroy(old);
        CHECK(vkdu_fence_event_create(done, i * 2 + 1, &replacement));
        CHECK(vkdu_fence_signal_cpu(done, i * 2));
        require(vkdu_fence_event_wait(replacement, 0) == timeout_hr, "cancelled handle must not signal reused registration");
        CHECK(vkdu_fence_signal_cpu(done, i * 2 + 1));
        CHECK(vkdu_fence_event_wait(replacement, 1000));
        vkdu_fence_event_destroy(replacement);
    }
    puts("PASS real fence event: 64 timeout/cancel/handle-reuse rounds");

    CHECK(vkdu_fence_event_create(done, 1000, &event));
    std::atomic<bool> entered{false};
    int32_t wait_result = 0;
    std::thread waiter([&]() { entered = true; wait_result = vkdu_fence_event_wait(event, 5000); });
    while (!entered.load()) std::this_thread::yield();
    vkdu_fence_event_cancel(event);
    waiter.join(); require(wait_result == cancelled_hr, "concurrent cancellation must report cancellation");
    vkdu_fence_event_destroy(event);

    // The event owns the underlying fence independently of its original
    // backend wrapper. Dropping the wrapper cannot invent fence completion.
    CHECK(vkdu_fence_event_create(done, 1001, &event));
    vkdu_object_destroy(done); done = nullptr;
    require(vkdu_fence_event_wait(event, 20) == timeout_hr, "destroyed caller fence retains pending event without success");
    vkdu_fence_event_destroy(event);
    puts("PASS real fence event: concurrent cancellation and pending owner destruction");

    CHECK(vkdu_fence_create(device, 0, &done));
    CHECK(vkdu_fence_event_create(done, 7, &event));
    entered = false; wait_result = 0;
    std::thread lost_waiter([&]() { entered = true; wait_result = vkdu_fence_event_wait(event, 5000); });
    while (!entered.load()) std::this_thread::yield();
    require(vkdu_device_remove(device, removed_hr) == removed_hr, "actual WDDM loss propagation into embedded device");
    lost_waiter.join(); require(wait_result == removed_hr, "device loss wakes bounded event wait with error");
    require(vkdu_fence_completed(done, &value) == removed_hr && value == UINT64_MAX, "device loss completion sentinel");
    require(vkdu_queue_signal(producer, done, 7) == removed_hr, "lost signal rejected");
    require(vkdu_queue_wait(consumer, done, 7) == removed_hr, "lost queue wait rejected");
    vkdu_fence_event_destroy(event);
    puts("PASS real fence event: device loss, UINT64_MAX completion and rejected queue work");

    vkdu_object_destroy(done); vkdu_object_destroy(gate);
    vkdu_object_destroy(producer); vkdu_object_destroy(consumer);
    vkdu_object_destroy(foreign_fence); vkdu_device_destroy(foreign); vkdu_device_destroy(device);
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    puts("PASS CPU_VULKAN_FENCE_LIFECYCLE_ONLY; OS monitored-fence import and ordinary VIOGPU D3D12 remain unvalidated");
    return 0;
}
