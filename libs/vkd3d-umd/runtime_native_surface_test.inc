/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual provider/packet implementation with controlled runtime callbacks.
 * No AHB, real KMD, system D3D12 runtime or GPU execution is simulated as proof. */
struct NativeSurfaceFixture {
    D3DKMT_HANDLE allocation, resource;
    HANDLE runtime;
    uint64_t size, address = 0;
    bool imported = false;
};
static std::map<uint64_t, NativeSurfaceFixture> native_surfaces;
struct NativeSurfaceAssociation { uint64_t key; D3DKMT_HANDLE allocation, resource; HANDLE runtime; };
static std::vector<NativeSurfaceAssociation> surface_associations;
static unsigned surface_resolves, surface_imports, surface_releases, surface_closes;
static HRESULT surface_resolve_result = S_OK, surface_import_result = S_OK, surface_release_result = S_OK;
static bool surface_bad_resolve, surface_bad_share;
static std::function<void()> surface_callback;
static HRESULT APIENTRY surface_escape(HANDLE adapter, const D3DDDICB_ESCAPE *args) {
    uint32_t opcode = 0;
    if (args->PrivateDriverDataSize >= 20) std::memcpy(&opcode,
        static_cast<unsigned char *>(args->pPrivateDriverData) + 16, 4);
    if (opcode != 6 && opcode != 7 && opcode != 10) return heap_escape(adapter, args);
    valid_kernel_callback(args->hDevice);
    if (adapter != expected_adapter) fixture_abort(__LINE__);
    if (opcode == 10) {
        auto *info = static_cast<NativeSurfaceResolve *>(args->pPrivateDriverData);
        if (args->PrivateDriverDataSize != sizeof(*info) || args->hContext ||
                !native_header_valid(info->header, sizeof(*info)) || info->flags || info->resource ||
                info->generation != generation || info->reserved2) fixture_abort(__LINE__);
        ++surface_resolves;
        auto found = native_surfaces.find(info->key);
        if (found == native_surfaces.end() || found->second.size != info->size) return E_INVALIDARG;
        const NativeSurfaceAssociation *association = nullptr;
        for (const auto &value : surface_associations)
            if (value.key == info->key && value.allocation == info->allocation) association = &value;
        if (!association) return E_INVALIDARG;
        info->resource = association->resource + (surface_bad_resolve ? 1 : 0);
        return surface_resolve_result;
    }
    auto *info = static_cast<NativeSurfaceShare *>(args->pPrivateDriverData);
    if (args->PrivateDriverDataSize != sizeof(*info) || args->hContext != expected_context ||
            !native_header_valid(info->header, sizeof(*info)) || info->flags ||
            info->generation != generation || info->context || info->reserved ||
            info->reserved2[0] || info->reserved2[1] || info->reserved2[2]) fixture_abort(__LINE__);
    auto found = native_surfaces.find(info->key);
    if (found == native_surfaces.end() || found->second.size != info->size || !info->address ||
            info->address % 4096) fixture_abort(__LINE__);
    auto &surface = found->second;
    HRESULT result;
    if (opcode == 6) {
        if (surface.imported) fixture_abort(__LINE__);
        surface.imported = true; surface.address = info->address; ++surface_imports;
        kernel_heaps.at(surface.allocation).address = info->address;
        result = surface_import_result;
    } else {
        if (surface.address != info->address) fixture_abort(__LINE__);
        ++surface_releases; result = surface_release_result;
        if (result == S_OK) surface.imported = false;
    }
    info->context = surface_bad_share ? 84 : 83;
    if (surface_callback) {
        auto callback = std::move(surface_callback); surface_callback = {};
        callback();
    }
    return result;
}
static HRESULT surface_deallocate_impl(HANDLE runtime, const D3DDDICB_DEALLOCATE *args) {
    for (auto it = surface_associations.begin(); it != surface_associations.end(); ++it) {
        if (args->HandleList && args->NumAllocations && *args->HandleList == it->allocation)
            fixture_abort(__LINE__); // Never destroy a foreign raw allocation.
        if (args->hResource != it->runtime) continue;
        valid_kernel_callback(runtime);
        if (args->NumAllocations || args->HandleList) fixture_abort(__LINE__);
        ++surface_closes; ++deallocations;
        if (fail_deallocate) { --fail_deallocate; return DXGI_ERROR_WAS_STILL_DRAWING; }
        unsigned active_key_opens = 0;
        for (const auto &other : surface_associations) active_key_opens += other.key == it->key;
        if (active_key_opens == 1 && native_surfaces.at(it->key).imported) {
            std::fputs("FAIL last runtime association closed before native mapping release\n", stderr);
            std::exit(1);
        }
        const auto allocation = it->allocation;
        if (!kernel_resources.erase(it->runtime)) fixture_abort(__LINE__);
        surface_associations.erase(it);
        bool still_open = false;
        for (const auto &other : surface_associations) still_open |= other.allocation == allocation;
        if (!still_open) kernel_heaps.erase(allocation);
        if (nested_deallocate) { auto callback = nested_deallocate; callback(); }
        return S_OK;
    }
    return heap_deallocate_impl(runtime, args);
}
static HRESULT APIENTRY surface_deallocate(HANDLE runtime, const D3DDDICB_DEALLOCATE *args) {
    ++legacy_deallocation_calls;
    return surface_deallocate_impl(runtime, args);
}
static HRESULT APIENTRY surface_deallocate2(HANDLE runtime, const D3DDDICB_DEALLOCATE2 *args) {
    valid_kernel_callback(runtime);
    if (args->Flags.Value != 3) fixture_abort(__LINE__);
    ++pageable_deallocation_calls;
    if (pageable_deallocation_result != S_OK) return pageable_deallocation_result;
    D3DDDICB_DEALLOCATE compatible{};
    compatible.hResource = args->hResource; compatible.NumAllocations = args->NumAllocations;
    compatible.HandleList = args->HandleList;
    return surface_deallocate_impl(runtime, &compatible);
}
static HRESULT APIENTRY surface_render(HANDLE runtime, D3DDDICB_RENDER *args) {
    valid_kernel_callback(runtime);
    proxy_fixture_render(args->hContext);
    if (args->hContext != expected_render_context || args->NumAllocations != 2 || args->NumPatchLocations != 1 ||
            args->CommandOffset || args->Flags.Value) fixture_abort(__LINE__);
    auto *header = static_cast<NativeRenderInfo *>(args->pNewCommandBuffer);
    auto *bytes = static_cast<unsigned char *>(args->pNewCommandBuffer);
    auto *owned = reinterpret_cast<NativeRenderReference *>(bytes + 64);
    auto *imported = reinterpret_cast<NativeImportedReference *>(bytes + 96);
    if (!native_header_valid(header->header, args->CommandLength) || header->generation != generation ||
            header->opcode != 1 || header->flags != 1 || header->references_offset != 64 ||
            header->references_count != 1 || header->stream_offset != 136 || header->stream_size != 16 ||
            header->reserved[0] != 96 || header->reserved[1] != 1 || header->reserved[2] != 2 || header->reserved[3] ||
            owned->index || owned->flags != 1 || owned->offset || owned->length != 4096 || owned->patch_offset ||
            owned->reserved || args->pNewPatchLocationList[0].AllocationIndex ||
            args->pNewPatchLocationList[0].AllocationOffset || args->pNewPatchLocationList[0].PatchOffset != 136 ||
            args->pNewAllocationList[0].WriteOperation || !args->pNewAllocationList[1].WriteOperation ||
            !kernel_heaps.count(args->pNewAllocationList[0].hAllocation)) fixture_abort(__LINE__);
    auto found = native_surfaces.find(imported->key);
    if (found == native_surfaces.end() || !found->second.imported || imported->address != found->second.address ||
            imported->size != found->second.size || imported->generation != generation || imported->access != 3 ||
            imported->allocation_index_plus_one != 2 ||
            !kernel_heaps.count(args->pNewAllocationList[1].hAllocation)) fixture_abort(__LINE__);
    ++renders;
    args->pNewCommandBuffer = dma_commands[1].data(); args->NewCommandBufferSize = 65536;
    args->pNewAllocationList = allocation_lists[1].data(); args->NewAllocationListSize = 1024;
    args->pNewPatchLocationList = patch_lists[1].data(); args->NewPatchLocationListSize = 1024;
    if (nested_render) nested_render();
    return render_result;
}

#include "runtime_native_residency_test.inc"
#include "runtime_native_present_test.inc"
#include "runtime_native_open_test.inc"

static int test_native_surface_provider() {
    native_surface_negative_active = true;
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty() && pending_events.empty() &&
        proxy_contexts.empty() && proxy_fence_contexts.empty() && !proxy_creating_context && !proxy_submitting_context);
    proxy_queue_fixture = true;
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = surface_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = surface_deallocate;
    kt.pfnDeallocate2Cb = surface_deallocate2;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock; kt.pfnRenderCb = surface_render;
    kt.pfnSignalSynchronizationObject2Cb = heap_signal;
    kt.pfnCreateSynchronizationObject2Cb = proxy_sync_create;
    kt.pfnDestroySynchronizationObjectCb = proxy_sync_destroy;
    kt.pfnSignalSynchronizationObjectFromGpuCb = proxy_gpu_signal;
    kt.pfnWaitForSynchronizationObjectFromGpuCb = proxy_gpu_wait;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    um.pfnSetCommandListErrorCb = test_command_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    const auto backend_creates_before = creates;
    const auto native_attempts_before = native_backend_attempts;
    native_backend_result = S_OK;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(!ctx->native_surfaces && creates == backend_creates_before + 1 &&
        native_backend_attempts == native_attempts_before);
    // Isolated provider implementation coverage only. VQDC Flags1 does not
    // admit native shares or Present; no production factory enables this bit.
    // This explicit fixture injection preserves the implementation tests while
    // the separate protocol test verifies actual creation keeps the gate off.
    ctx->native_surfaces = true;
    std::puts("FIXTURE ONLY: injected native surface provider; VQDC owned-domain gate remains closed in production");
    const auto closes_before = surface_closes, allocations_before = allocations;
    const uint64_t key = 0x1337;
    const D3DKMT_HANDLE allocation = 60000, km_resource = 70000;
    const HANDLE rt_resource = reinterpret_cast<HANDLE>(uintptr_t(0x776600));
    auto runtime_open = [&]() {
        native_surfaces.emplace(key, NativeSurfaceFixture{allocation, km_resource, rt_resource, 65536});
        kernel_heaps.emplace(allocation, KernelHeap{0, 65536, false, rt_resource});
        kernel_resources.insert(rt_resource);
        surface_associations.push_back({key, allocation, km_resource, rt_resource});
    };
    auto runtime_close = [&]() {
        if (native_surfaces.at(key).imported) fixture_abort(__LINE__);
        native_surfaces.erase(key); kernel_heaps.erase(allocation); kernel_resources.erase(rt_resource);
        surface_associations.clear();
    };
    NativeHeap *heap = nullptr;
    auto import = [&]() { return native_surface_import(ctx, allocation, km_resource, key, 65536, rt_resource, &heap); };
    runtime_open();
    ctx->native_surfaces = false;
    REQUIRE(import() == DXGI_ERROR_UNSUPPORTED && !heap && !surface_imports);
    ctx->native_surfaces = true;
    REQUIRE(native_surface_import(ctx, allocation, km_resource, key, 65535, rt_resource, &heap) == E_INVALIDARG && !heap);
    REQUIRE(native_surface_import(ctx, allocation, km_resource, key, 65536, nullptr, &heap) == E_INVALIDARG && !heap);
    surface_bad_resolve = true;
    REQUIRE(import() == E_INVALIDARG && !heap && !ctx->native_heaps && !surface_imports);
    surface_bad_resolve = false; surface_resolve_result = S_FALSE;
    REQUIRE(import() == E_FAIL && !heap && !ctx->native_heaps && !surface_imports);
    surface_resolve_result = S_OK;
    // Import may have reached the host despite a failed callback. Abort releases
    // that mapping without stealing the runtime's unpublished open association.
    surface_import_result = E_OUTOFMEMORY;
    REQUIRE(import() == E_OUTOFMEMORY && !heap && !ctx->native_heaps);
    REQUIRE(surface_imports == 1 && surface_releases == 1 && surface_closes == closes_before);
    REQUIRE(kernel_heaps.count(allocation) && kernel_resources.count(rt_resource));
    surface_import_result = S_OK; surface_bad_share = true;
    REQUIRE(import() == E_FAIL && !heap && ctx->native_heaps && ctx->native_heaps->native_key == key);
    // Malformed release replies keep the VA quarantined even if the host acted.
    surface_bad_share = false;
    REQUIRE(native_runtime_release(ctx, ctx->native_heaps) == S_OK && !ctx->native_heaps);
    REQUIRE(surface_closes == closes_before);
    REQUIRE(import() == S_OK && heap && !heap->resource_created && heap->users == 1);
    REQUIRE(native_runtime_release(ctx, heap) == S_OK && !ctx->native_heaps);
    REQUIRE(surface_closes == closes_before && allocations == allocations_before);
    runtime_close();

    runtime_open();
    REQUIRE(import() == S_OK && native_surface_complete_open(ctx, heap) == S_OK);
    REQUIRE(native_surface_complete_open(ctx, heap) == E_INVALIDARG);
    const auto imported_address = heap->address;
    NativeHeap *alias = nullptr;
    REQUIRE(native_surface_import(ctx, allocation + 1, km_resource, key, 65536,
        reinterpret_cast<HANDLE>(uintptr_t(0x776601)), &alias) == E_INVALIDARG && !alias);
    void *mapping = reinterpret_cast<void *>(uintptr_t(1)); uint32_t mapped_handle = 1;
    REQUIRE(native_runtime_map(ctx, heap, &mapping, &mapped_handle) == E_INVALIDARG && !mapping && !mapped_handle);
    mwd_allocation retained{};
    REQUIRE(native_runtime_retain(ctx, heap, &retained) == S_OK && retained.token == heap && retained.handle == allocation &&
        retained.address == imported_address && retained.size == 65536 && retained.flags == 20 && heap->users == 2);
    REQUIRE(native_runtime_release(ctx, heap) == S_OK && heap->users == 1);
    mwd_allocation ordinary{};
    REQUIRE(native_runtime_allocate(ctx, 4096, 4096, 0, 6, &ordinary) == S_OK);
    GuardedProxyQueue guarded_queue{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 proxy_args{};
    proxy_args.NodeMask = 1; proxy_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    proxy_args.hDrvCommandQueue.pDrvPrivate = &guarded_queue.object;
    proxy_args.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x99330));
    const auto contexts_before_proxy = runtime_context_creates, destroys_before_proxy = runtime_context_destroys;
    const auto syncs_before_proxy = proxy_sync_creates, sync_destroys_before_proxy = proxy_sync_destroys;
    const auto gpu_signals_before = proxy_gpu_signals, gpu_waits_before = proxy_gpu_waits;
    for (unsigned failure = 1; failure != 3; ++failure) {
        fail_proxy_sync_create = failure;
        std::array<unsigned char, sizeof(Object)> unpublished;
        std::memcpy(unpublished.data(), &guarded_queue.object, unpublished.size());
        const HRESULT hr = native_queue_create(ctx, &proxy_args);
        // This is an exact before/after snapshot of runtime-owned bytes, not
        // equality between separately initialized C++ objects and their padding.
        const bool unchanged = !std::memcmp(&guarded_queue.object, unpublished.data(), unpublished.size());
        std::printf("PROXY_ROLLBACK fence=%u hr=%08x slot_unchanged=%u guards=%u routes_empty=%u contexts_empty=%u fences_empty=%u creating_empty=%u\n",
            failure, static_cast<unsigned>(hr), unchanged, guarded_queue.intact(), !ctx->native_runtime_queues,
            proxy_contexts.empty(), proxy_fence_contexts.empty(), !proxy_creating_context);
        REQUIRE(hr == E_OUTOFMEMORY);
        REQUIRE(guarded_queue.intact() && unchanged);
        REQUIRE(!ctx->native_runtime_queues && proxy_contexts.empty() && proxy_fence_contexts.empty() && !proxy_creating_context);
    }
    REQUIRE(native_queue_create(ctx, &proxy_args) == S_OK && guarded_queue.intact());
    auto *proxy_route = guarded_queue.object.runtime_route;
    REQUIRE(proxy_route && proxy_route->proxy && !proxy_route->pending && proxy_route->hop_in && proxy_route->hop_out &&
        proxy_route->hop_in != proxy_route->hop_out && proxy_route->native_heap_context != expected_context &&
        !proxy_route->native_commands && !proxy_route->native_allocation_list && !proxy_route->native_patch_list &&
        proxy_contexts.at(proxy_route->native_heap_context).runtime == proxy_args.hRTCommandQueue.handle);
    std::array<unsigned char, 16> stream{};
    // Caller order differs from runtime allocation-list order: ordinary first.
    mwd_reference refs[2] = {{heap, 0, 65536, 3, MWD_NATIVE_SURFACE_REFERENCE}, {ordinary.token, 0, 4096, 1, 0}};
    auto submit = [&]() {
        proxy_submitting_context = proxy_route->native_heap_context;
        const auto hr = native_runtime_submit_queue(ctx, proxy_route, 1, stream.data(), stream.size(), refs, 2);
        proxy_submitting_context = nullptr;
        return hr;
    };
    const auto renders_before = renders;
    refs[0].patch_offset = 0;
    REQUIRE(submit() == E_INVALIDARG && renders == renders_before);
    refs[0].patch_offset = MWD_NATIVE_SURFACE_REFERENCE; refs[0].length = 4096;
    REQUIRE(submit() == E_INVALIDARG && renders == renders_before);
    refs[0].length = 65536; ctx->native_surfaces = false;
    REQUIRE(submit() == E_INVALIDARG && renders == renders_before);
    ctx->native_surfaces = true;
    defer_completion = true;
    nested_render = [&]() {
        if (heap->users != 2 || heap->submitted != 1 || native_runtime_release(ctx, heap) != S_OK ||
                native_runtime_release(ctx, ordinary.token) != S_OK) fixture_abort(__LINE__);
        refs[0] = {}; refs[1] = {}; stream.fill(0xa5);
    };
    REQUIRE(submit() == S_OK && ctx->native_submission_count == 1 && surface_closes == closes_before);
    REQUIRE(proxy_gpu_signals == gpu_signals_before + 2 && proxy_gpu_waits == gpu_waits_before + 2 &&
        proxy_route->hop_value == 2 && proxy_route->submissions == 1 &&
        proxy_contexts.at(proxy_route->native_heap_context).value == 2 &&
        !proxy_contexts.at(proxy_route->native_heap_context).phase && guarded_queue.intact());
    nested_render = {};
    uint32_t completed = 99;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && !completed && kernel_heaps.count(allocation));
    complete_events(); surface_release_result = E_ACCESSDENIED;
    REQUIRE(native_runtime_completed(ctx, &completed) == E_ACCESSDENIED && !completed &&
        ctx->native_submission_count == 1 && surface_closes == closes_before);
    surface_release_result = S_OK; fail_deallocate = 1;
    REQUIRE(native_runtime_completed(ctx, &completed) == DXGI_ERROR_WAS_STILL_DRAWING && !completed &&
        ctx->native_submission_count == 1 && kernel_heaps.count(allocation));
    const auto releases_before_retry = surface_releases;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == 1 && !ctx->native_heaps &&
        !ctx->native_submissions && kernel_heaps.empty() && kernel_resources.empty());
    REQUIRE(surface_releases == releases_before_retry && surface_closes == closes_before + 2);
    REQUIRE(!proxy_route->submissions);
    native_queue_destroy(ctx, &guarded_queue.object);
    REQUIRE(guarded_queue.intact() && !ctx->native_queue_objects && !ctx->native_runtime_queues &&
        proxy_contexts.empty() && proxy_fence_contexts.empty() &&
        runtime_context_creates == contexts_before_proxy + 3 && runtime_context_destroys == destroys_before_proxy + 3 &&
        proxy_sync_creates == syncs_before_proxy + 3 && proxy_sync_destroys == sync_destroys_before_proxy + 3);
    std::puts("PASS CPU proxy queue callbacks: standard context, typed monitored-fence ownership, creation rollback, ordered owner Render hops and slot canaries");
    runtime_close(); defer_completion = false;

    // The identity callback may synchronously release the original owner. A
    // retained token must be pinned before QueryAdapterInfo can run that code.
    runtime_open();
    REQUIRE(import() == S_OK && native_surface_complete_open(ctx, heap) == S_OK);
    identity_query_callback = [&]() {
        if (heap->users != 2) {
            std::fputs("FAIL native token not pinned before reentrant identity callback\n", stderr);
            std::exit(1);
        }
        if (native_runtime_release(ctx, heap) != S_OK) fixture_abort(__LINE__);
    };
    REQUIRE(native_runtime_retain(ctx, heap, &retained) == S_OK && retained.token == heap && heap->users == 1);
    REQUIRE(!identity_query_callback && native_runtime_release(ctx, retained.token) == S_OK && !ctx->native_heaps);
    runtime_close();
    // Distinct runtime opens can reuse either the same allocation handle or
    // different handles. Each closes exactly its own runtime association; the
    // shared mapping and VA survive until every alias and DMA hold retires.
    for (unsigned mode = 0; mode != 4; ++mode) {
        runtime_open();
        REQUIRE(import() == S_OK && native_surface_complete_open(ctx, heap) == S_OK);
        auto *first = heap;
        const HANDLE alias_runtime = reinterpret_cast<HANDLE>(uintptr_t(0x776601));
        const auto alias_allocation = allocation + (mode & 1);
        surface_associations.push_back({key, alias_allocation, km_resource, alias_runtime});
        kernel_heaps.emplace(alias_allocation, KernelHeap{heap->address, 65536, false, alias_runtime});
        kernel_resources.insert(alias_runtime);
        const auto imports_before_alias = surface_imports, releases_before_alias = surface_releases;
        const auto closes_before_alias = surface_closes;
        REQUIRE(native_surface_import(ctx, alias_allocation, km_resource, key, 65536, alias_runtime, &alias) == S_OK && alias);
        REQUIRE(alias != first && alias->address == first->address && alias->native_mapping == first->native_mapping &&
            alias->native_mapping.use_count() == 2 && surface_imports == imports_before_alias);
        REQUIRE(native_surface_complete_open(ctx, alias) == S_OK);
        if (mode == 2) {
            nested_deallocate = [&]() {
                nested_deallocate = {};
                if (native_runtime_release(ctx, alias) != S_OK) fixture_abort(__LINE__);
            };
            REQUIRE(native_runtime_release(ctx, first) == S_OK && !ctx->native_heaps);
        } else if (mode == 3) {
            REQUIRE(native_runtime_allocate(ctx, 4096, 4096, 0, 6, &ordinary) == S_OK);
            mwd_reference merged[3] = {{alias, 0, 65536, 1, MWD_NATIVE_SURFACE_REFERENCE},
                {ordinary.token, 0, 4096, 1, 0}, {first, 0, 65536, 2, MWD_NATIVE_SURFACE_REFERENCE}};
            const auto serial = ctx->native_last_submitted + 1;
            defer_completion = true;
            REQUIRE(native_runtime_submit(ctx, serial, stream.data(), stream.size(), merged, 3) == S_OK &&
                ctx->native_submission_count == 1 && ctx->native_submissions->count == 3);
            REQUIRE(native_runtime_release(ctx, first) == S_OK && native_runtime_release(ctx, alias) == S_OK &&
                native_runtime_release(ctx, ordinary.token) == S_OK && surface_closes == closes_before_alias);
            complete_events();
            REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == serial && !ctx->native_heaps);
            defer_completion = false;
        } else {
            auto *first_close = mode ? alias : first;
            auto *last_close = mode ? first : alias;
            const auto first_runtime = first_close->runtime_resource, last_runtime = last_close->runtime_resource;
            REQUIRE(native_runtime_release(ctx, first_close) == S_OK && surface_releases == releases_before_alias &&
                !kernel_resources.count(first_runtime) && kernel_resources.count(last_runtime));
            REQUIRE(native_runtime_retain(ctx, last_close, &retained) == S_OK && retained.address == last_close->address);
            REQUIRE(native_runtime_release(ctx, retained.token) == S_OK && native_runtime_release(ctx, last_close) == S_OK);
        }
        REQUIRE(surface_closes == closes_before_alias + 2 && surface_releases == releases_before_alias + 1 &&
            !ctx->native_heaps && surface_associations.empty() && kernel_heaps.empty() && kernel_resources.empty());
        runtime_close();
    }
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty());
    REQUIRE(test_native_open_ddi(ctx, create.hDrvDevice) == 0);
    REQUIRE(proxy_contexts.empty() && proxy_fence_contexts.empty() && !proxy_creating_context);
    std::weak_ptr<NativeAdapter> weak = ctx->native_adapter;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK && !weak.expired());
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(!context(create.hDrvDevice) && weak.expired());
    // Cancellation inside import, final host release, or per-open Deallocate
    // must never make another runtime callback after DestroyDevice returns.
    for (unsigned terminal = 0; terminal != 8; ++terminal) {
        memory.fill(0);
        REQUIRE(OpenAdapter12(&open) == S_OK && adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        ctx = context(create.hDrvDevice); ctx->native_surfaces = true;
        ++ctx->references; // Inspect cleanup before releasing our last test hold.
        weak = ctx->native_adapter;
        REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK && !weak.expired());
        if (terminal < 3) runtime_open();
        const auto close_count = surface_closes, release_count = surface_releases;
        const auto context_count = heap_context_destroys, deallocation_count = deallocations;
        auto terminate = [&]() {
            adapter.pfnDestroyDevice(create.hDrvDevice);
            heap_callbacks_retired = true; memory.fill(0xa5);
        };
        if (terminal >= 6) REQUIRE(test_native_present_cancel(ctx, create.hDrvDevice, terminal == 7, terminate) == 0);
        else if (terminal >= 4) REQUIRE(test_native_alias_paging_cancel(ctx, create.hDrvDevice, terminal == 5, terminate) == 0);
        else if (terminal == 3) REQUIRE(test_native_open_cancel(ctx, create.hDrvDevice, terminate) == 0);
        else if (!terminal) surface_callback = terminate;
        else {
            REQUIRE(import() == S_OK && native_surface_complete_open(ctx, heap) == S_OK);
            if (terminal == 1) surface_callback = terminate;
            else nested_deallocate = terminate;
        }
        if (terminal < 3) REQUIRE((terminal ? native_runtime_release(ctx, heap) : import()) == DXGI_ERROR_DEVICE_REMOVED);
        surface_callback = {}; nested_deallocate = {};
        REQUIRE(ctx->native_retiring && !ctx->runtime_device.handle && !context(create.hDrvDevice));
        REQUIRE(surface_closes == close_count + (terminal == 2) &&
            surface_releases == release_count + (terminal == 1 || terminal == 2) &&
            deallocations == deallocation_count + (terminal == 2) && heap_context_destroys == context_count);
        // Only the runtime now owns surviving KMT associations. Dropping the
        // provider token frees local storage without reaching those callbacks.
        if (terminal < 3)
            REQUIRE(ctx->native_heaps && native_runtime_release(ctx, ctx->native_heaps) == S_OK && !ctx->native_heaps);
        release(ctx);
        REQUIRE(weak.expired());
        REQUIRE(proxy_contexts.empty() && proxy_fence_contexts.empty() && !proxy_creating_context);
        native_surfaces.clear(); surface_associations.clear(); kernel_heaps.clear(); kernel_resources.clear();
        heap_callbacks_retired = false;
    }
    std::puts("PASS native surface resolve/import, publish-only per-open close, mixed DMA event lifetime and reentrant token pin");
    std::puts("PASS native surface cancellation at import/release/deallocate/Open backend: exact termination, no late callback, expired adapter owner");
    std::puts("PASS native surface alias opens: same/distinct handles, independent close, recursive close, merged packet and every token retained");
    native_surface_negative_active = false;
    proxy_queue_fixture = false;
    native_backend_result = DXGI_ERROR_UNSUPPORTED;
    return 0;
}
