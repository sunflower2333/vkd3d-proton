/* SPDX-License-Identifier: LGPL-2.1-or-later */
void APIENTRY native_stream_output(D3D12DDI_HCOMMANDLIST h, UINT first, UINT count,
        const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *views) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    if (first > 4 || count > 4 - first) {
        command.run([&] {
            vkdu_object_destroy(command.value); command.value = nullptr;
            return E_INVALIDARG;
        });
        return;
    }
    D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW copy[4]{};
    vkdu_stream_output_buffer buffers[4]{};
    if (views) std::memcpy(copy, views, count * sizeof(*views));
    auto *ctx = command.op.ctx;
    HRESULT hr = ctx->native_adapter ? native_heap_generation(ctx) : S_OK;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (UINT i = 0; i < count && SUCCEEDED(hr); ++i) {
        const auto v = copy[i];
        if (!v.SizeInBytes) continue; /* Both locations are explicitly ignored. */
        if (!v.BufferLocation || !v.BufferFilledSizeLocation || v.BufferLocation % 4 ||
                v.SizeInBytes % 4 || v.BufferFilledSizeLocation % 4) { hr = E_INVALIDARG; break; }
        auto *data = find_buffer_address(ctx, v.BufferLocation, v.SizeInBytes);
        auto *counter = find_buffer_address(ctx, v.BufferFilledSizeLocation, 4);
        if (!data || !counter || !vkdu_object_retain(data->backend)) { hr = E_INVALIDARG; break; }
        buffers[i] = {data->backend, nullptr, v.BufferLocation - data->address,
            v.SizeInBytes, v.BufferFilledSizeLocation - counter->address};
        if (!vkdu_object_retain(counter->backend)) { hr = E_INVALIDARG; break; }
        buffers[i].counter = counter->backend;
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    command.run([&] {
        if (SUCCEEDED(hr)) hr = vkdu_command_stream_output(command.value, first, count, buffers);
        for (UINT i = 0; i < count; ++i) {
            vkdu_object_destroy(buffers[i].buffer); vkdu_object_destroy(buffers[i].counter);
        }
        vkdu_object_destroy(command.value); command.value = nullptr;
        return hr;
    });
}
