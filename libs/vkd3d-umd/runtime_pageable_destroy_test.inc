/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_pageable_destroy() {
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty());
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{}; D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate;
    kt.pfnDeallocateCb = heap_deallocate; kt.pfnDeallocate2Cb = heap_deallocate2;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock;
    kt.pfnRenderCb = heap_render; kt.pfnSignalSynchronizationObject2Cb = heap_signal;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    NativeDevice storage{}; D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = &storage; create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    D3D12DDIARG_CREATEHEAP_0001 desc{};
    desc.ByteSize = desc.Alignment = 65536; desc.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE; desc.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    desc.CreationNodeMask = desc.VisibleNodeMask = 1;
    D3D12DDIARG_CREATERESOURCE_0003 resource{};
    resource.ResourceType = D3D12DDI_RT_BUFFER; resource.Width = 4096;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR;
    NativeHeapSlot slot{}; Object object{};
    D3D12DDI_HHEAP heap{&slot}; D3D12DDI_HRESOURCE buffer{&object};
    const auto initial_allocations = allocations, initial_contexts = heap_context_creates;
    ctx->kernel_callbacks.pfnDeallocate2Cb = nullptr;
    // Storage sizing cannot advertise the missing callback as a zero-byte
    // object; creation must reject it before any kernel allocation or slot write.
    const auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, nullptr);
    REQUIRE(sizes.Heap == sizeof(NativeHeapSlot) && !sizes.Resource);
    REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, heap, {}, nullptr, nullptr, {}) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(allocations == initial_allocations && heap_context_creates == initial_contexts &&
        !slot.magic && !ctx->native_heaps && kernel_heaps.empty() && kernel_resources.empty());
    ctx->kernel_callbacks.pfnDeallocate2Cb = heap_deallocate2;
    for (unsigned kind = 0; kind < 3; ++kind) {
        D3D12DDI_HRTRESOURCE runtime{};
        if (kind) runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x8da000 + kind));
        const auto before = pageable_deallocation_calls, legacy = legacy_deallocation_calls;
        REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, heap, runtime,
            kind == 2 ? &resource : nullptr, nullptr, kind == 2 ? buffer : D3D12DDI_HRESOURCE{}) == S_OK);
        table.pfnDestroyHeapAndResource(create.hDrvDevice, heap, kind == 2 ? buffer : D3D12DDI_HRESOURCE{});
        REQUIRE(!ctx->native_heaps && !ctx->resources && kernel_heaps.empty() && kernel_resources.empty());
        REQUIRE(pageable_deallocation_calls == before + 1 && legacy_deallocation_calls == legacy);
    }
    const auto before = pageable_deallocation_calls, legacy = legacy_deallocation_calls;
    mwd_allocation internal{};
    REQUIRE(native_runtime_allocate(ctx, 4096, 4096, 0, 6, &internal) == S_OK);
    REQUIRE(native_runtime_release(ctx, internal.token) == S_OK);
    REQUIRE(pageable_deallocation_calls == before && legacy_deallocation_calls == legacy + 1);
    for (unsigned kind = 0; kind < 2; ++kind) {
        D3D12DDI_HRTRESOURCE runtime{};
        if (kind) runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x8da100));
        REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, heap, runtime, nullptr, nullptr, {}) == S_OK);
        auto *record = native_heap_find(ctx, &slot); REQUIRE(record);
        const auto allocation = record->allocation;
        const auto calls = pageable_deallocation_calls, old_calls = legacy_deallocation_calls;
        pageable_deallocation_result = kind ? S_FALSE : E_OUTOFMEMORY;
        table.pfnDestroyHeapAndResource(create.hDrvDevice, heap, {});
        REQUIRE(!slot.magic && !record->slot && record->allocation == allocation && kernel_heaps.count(allocation));
        REQUIRE(ctx->last_error == (kind ? E_FAIL : E_OUTOFMEMORY) &&
            pageable_deallocation_calls == calls + 1 && legacy_deallocation_calls == old_calls);
        // Runtime storage can be overwritten immediately; the independent
        // cleanup record, exact handle and destruction policy must survive.
        slot.magic = 0xa5a5a5a5;
        pageable_deallocation_result = S_OK;
        REQUIRE(native_runtime_release(ctx, record) == S_OK && !ctx->native_heaps && kernel_heaps.empty() && kernel_resources.empty());
        REQUIRE(slot.magic == 0xa5a5a5a5 && pageable_deallocation_calls == calls + 2 && legacy_deallocation_calls == old_calls);
    }
    struct Slots { NativeHeapSlot heap{}; Object resource{}; };
    auto *slots = static_cast<Slots *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(slots); new (slots) Slots{};
    D3D12DDI_HHEAP final_heap{&slots->heap}; D3D12DDI_HRESOURCE final_resource{&slots->resource};
    D3D12DDI_HRTRESOURCE final_runtime{}; final_runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x8da200));
    REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, final_heap, final_runtime,
        &resource, nullptr, final_resource) == S_OK);
    REQUIRE(retain_live(ctx));
    std::weak_ptr<NativeAdapter> weak = ctx->native_adapter;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    bool retired = false;
    const auto calls = pageable_deallocation_calls, old_calls = legacy_deallocation_calls;
    nested_deallocate = [&]() {
        if (retired) fixture_abort(__LINE__);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        std::memset(&storage, 0xa5, sizeof(storage));
        if (!VirtualFree(slots, 0, MEM_RELEASE)) fixture_abort(__LINE__);
        retired = heap_callbacks_retired = true;
    };
    table.pfnDestroyHeapAndResource(create.hDrvDevice, final_heap, final_resource);
    nested_deallocate = {};
    REQUIRE(retired && ctx->native_retiring && !ctx->runtime_device.handle && !ctx->native_heaps && !ctx->resources &&
        kernel_heaps.empty() && kernel_resources.empty() && pageable_deallocation_calls == calls + 1 &&
        legacy_deallocation_calls == old_calls);
    release(ctx);
    REQUIRE(weak.expired()); heap_callbacks_retired = false;
    puts("PASS pageable heap/resource Deallocate2 flags3, absent callback, exact association/allocation, failed close retry after slot expiry, reentrant device destruction/VirtualFree; internal ICD allocation retains legacy callback");
    return 0;
}
