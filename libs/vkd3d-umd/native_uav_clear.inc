/* SPDX-License-Identifier: LGPL-2.1-or-later */
template<class Value> void native_clear_uav(D3D12DDI_HCOMMANDLIST h,
        D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu_handle,
        D3D12DDI_HRESOURCE resource_handle, const Value values[4], UINT count,
        const D3D12DDI_RECT *rects, bool floating) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    auto fail = [&](HRESULT hr) {
        command.run([&] { vkdu_object_destroy(command.value); command.value = nullptr; return hr; });
    };
    if (!values || (count && !rects)) { fail(E_INVALIDARG); return; }
    Value copy[4]; std::memcpy(copy, values, sizeof(copy));
    vkdu_rect stack[16]; std::unique_ptr<vkdu_rect[]> allocated;
    auto *translated = stack;
    if (count > 16) {
        if (count > SIZE_MAX / sizeof(vkdu_rect)) { fail(E_OUTOFMEMORY); return; }
        allocated.reset(new (std::nothrow) vkdu_rect[count]);
        if (!allocated) { fail(E_OUTOFMEMORY); return; }
        translated = allocated.get();
    }
    for (UINT i = 0; i < count; ++i)
        translated[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    auto *ctx = command.op.ctx;
    uint32_t gpu_index = 0, cpu_index = 0;
    vkdu_object *gpu = nullptr, *cpu = nullptr, *resource = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *g = find_heap(ctx, gpu_handle.ptr, true, &gpu_index);
    auto *c = find_heap(ctx, cpu_handle.ptr, false, &cpu_index);
    auto *r = find_resource(ctx, resource_handle.pDrvPrivate);
    if (g && g->descriptor_type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            (g->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
            vkdu_object_retain(g->backend)) gpu = g->backend;
    if (c && c->descriptor_type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            !(c->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
            vkdu_object_retain(c->backend)) cpu = c->backend;
    if (r && vkdu_object_retain(r->backend)) resource = r->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    command.run([&] {
        const HRESULT hr = gpu && cpu && resource ? vkdu_command_clear_uav(command.value,
                gpu, gpu_index, cpu, cpu_index, resource, floating, copy, count, translated) : E_INVALIDARG;
        vkdu_object_destroy(gpu); vkdu_object_destroy(cpu); vkdu_object_destroy(resource);
        vkdu_object_destroy(command.value); command.value = nullptr;
        return hr;
    });
}
void APIENTRY native_clear_uav_uint(D3D12DDI_HCOMMANDLIST h, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE resource, const UINT values[4],
        UINT count, const D3D12DDI_RECT *rects) {
    native_clear_uav(h, gpu, cpu, resource, values, count, rects, false);
}
void APIENTRY native_clear_uav_float(D3D12DDI_HCOMMANDLIST h, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE resource, const FLOAT values[4],
        UINT count, const D3D12DDI_RECT *rects) {
    native_clear_uav(h, gpu, cpu, resource, values, count, rects, true);
}
