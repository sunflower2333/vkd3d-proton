/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Only retained peers cross a backend call. Runtime slots and input arrays can
 * disappear synchronously in Vulkan/runtime callbacks. */
void APIENTRY native_graphics_table(D3D12DDI_HCOMMANDLIST h, UINT index,
        D3D12DDI_GPU_DESCRIPTOR_HANDLE address) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    auto *ctx = command.op.ctx;
    vkdu_object *value = nullptr;
    uint32_t first = 0;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, address.ptr, true, &first);
    if (heap && vkdu_object_retain(heap->backend)) value = heap->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    if (!value) { command.report(E_INVALIDARG); return; }
    command.run([&] {
        const auto hr = vkdu_command_graphics_table(command.value, index, value, first);
        vkdu_object_destroy(value);
        return hr;
    });
}

template<unsigned type> void APIENTRY native_graphics_descriptor(D3D12DDI_HCOMMANDLIST h,
        UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS address) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    auto *ctx = command.op.ctx;
    vkdu_object *value = nullptr;
    uint64_t offset = 0;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *resource = find_buffer_address(ctx, address);
    if (resource && vkdu_object_retain(resource->backend)) {
        value = resource->backend;
        offset = address - resource->address;
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    if (!value) { command.report(E_INVALIDARG); return; }
    command.run([&] {
        const auto hr = type == 2 ? vkdu_command_graphics_cbv(command.value, index, value, offset) :
            type == 3 ? vkdu_command_graphics_srv(command.value, index, value, offset) :
            vkdu_command_graphics_uav(command.value, index, value, offset);
        vkdu_object_destroy(value);
        return hr;
    });
}

void APIENTRY native_graphics_constants(D3D12DDI_HCOMMANDLIST h, UINT index, UINT count,
        const VOID *values, UINT offset) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    if (count > 64 || offset > 64 - count || (count && !values)) {
        command.report(E_INVALIDARG); return;
    }
    uint32_t copied[64];
    if (count) std::memcpy(copied, values, size_t(count) * sizeof(copied[0]));
    command.run([&] { return vkdu_command_graphics_constants(command.value, index, offset, count, copied); });
}

void APIENTRY native_graphics_constant(D3D12DDI_HCOMMANDLIST h, UINT index, UINT value, UINT offset) {
    native_graphics_constants(h, index, 1, &value, offset);
}
