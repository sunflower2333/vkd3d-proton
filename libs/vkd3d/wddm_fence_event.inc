/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Embedded fence events have independent ownership. Registration removal and
 * GPU completion serialize under the same fence mutex before a handle closes.
 * This is NOT a replacement for Windows' GPUVA-backed monitored fence. */
struct vkd3d_wddm_fence_event
{
    struct d3d12_fence *fence;
    vkd3d_native_sync_handle handle;
    LONG references;
    uint32_t cancelled, completed;
};

/* An external WDDM reset must reach embedded completion waiters too. This is
 * a bridge notification, not an implementation of the public RemoveDevice API. */
HRESULT vkd3d_wddm_device_lost(ID3D12Device *iface, HRESULT reason)
{
    struct d3d12_device *device;
    /* Portable core headers only define DEVICE_REMOVED. Other values below
     * are DXGI_ERROR_DEVICE_RESET, DEVICE_HUNG and DRIVER_INTERNAL_ERROR. */
    if (!iface || (reason != DXGI_ERROR_DEVICE_REMOVED && reason != (HRESULT)0x887a0007 &&
            reason != (HRESULT)0x887a0006 && reason != (HRESULT)0x887a0020)) return E_INVALIDARG;
    device = impl_from_ID3D12Device((d3d12_device_iface *)iface);
    if (SUCCEEDED(d3d12_device_removed_reason(device)))
        d3d12_device_mark_as_removed(device, reason, "WDDM native device lost");
    return d3d12_device_removed_reason(device);
}

static void vkd3d_wddm_fence_event_release(struct vkd3d_wddm_fence_event *event)
{
    if (!InterlockedDecrement(&event->references))
    {
        vkd3d_native_sync_handle_destroy(event->handle);
        ID3D12Fence1_Release(&event->fence->ID3D12Fence_iface);
        vkd3d_free(event);
    }
}

HRESULT vkd3d_wddm_fence_event_create(ID3D12Fence *iface, UINT64 value,
        struct vkd3d_wddm_fence_event **out)
{
    struct vkd3d_wddm_fence_event *event;
    extern CONST_VTBL struct ID3D12Fence1Vtbl d3d12_fence_vtbl;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!iface || iface->lpVtbl != (const struct ID3D12FenceVtbl *)&d3d12_fence_vtbl || value == UINT64_MAX)
        return E_INVALIDARG;
    if (!(event = vkd3d_calloc(1, sizeof(*event)))) return E_OUTOFMEMORY;
    event->references = 1;
    event->fence = impl_from_ID3D12Fence(iface);
    ID3D12Fence_AddRef(iface);
    hr = d3d12_device_removed_reason(event->fence->device);
    if (SUCCEEDED(hr)) hr = vkd3d_native_sync_handle_create(0, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT, &event->handle);
    if (SUCCEEDED(hr)) hr = d3d12_fence_set_native_sync_handle_on_completion(event->fence, value, event->handle);
    if (FAILED(hr)) { vkd3d_wddm_fence_event_release(event); return hr; }
    *out = event;
    return S_OK;
}

void vkd3d_wddm_fence_event_cancel(struct vkd3d_wddm_fence_event *event)
{
    struct d3d12_fence *fence;
    unsigned int i;
    if (!event) return;
    fence = event->fence;
    pthread_mutex_lock(&fence->mutex);
    if (!vkd3d_atomic_uint32_compare_exchange(&event->cancelled, 0, 1,
            vkd3d_memory_order_seq_cst, vkd3d_memory_order_acquire))
    {
        for (i = 0; i < fence->event_count; ++i)
            if (vkd3d_native_sync_handle_eq(fence->events[i].handle, event->handle))
            {
                /* Cancellation ends tracing but does not fabricate completion. */
                vkd3d_queue_timeline_trace_complete_event_signal(&fence->device->queue_timeline_trace,
                        NULL, fence->events[i].timeline_cookie);
                fence->events[i] = fence->events[--fence->event_count];
                break;
            }
        vkd3d_native_sync_handle_signal(event->handle);
    }
    pthread_mutex_unlock(&fence->mutex);
}

/* One waiting thread per registration. The caller retains ownership until
 * wait has entered; cancel may run concurrently. Destroy after wait returns.
 * Timeout leaves the registration armed so another bounded wait can continue. */
HRESULT vkd3d_wddm_fence_event_wait(struct vkd3d_wddm_fence_event *event, UINT timeout_ms)
{
    uint64_t start;
    UINT elapsed, slice;
    HRESULT hr;
    if (!event) return E_INVALIDARG;
    InterlockedIncrement(&event->references);
    start = vkd3d_get_current_time_ns();
    for (;;)
    {
        if (vkd3d_atomic_uint32_load_explicit(&event->cancelled, vkd3d_memory_order_acquire))
        { hr = (HRESULT)0x800703e3; break; } /* HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) */
        if (FAILED(hr = d3d12_device_removed_reason(event->fence->device))) break;
        if (vkd3d_atomic_uint32_load_explicit(&event->completed, vkd3d_memory_order_acquire)) { hr = S_OK; break; }
        elapsed = (UINT)min((vkd3d_get_current_time_ns() - start) / 1000000, (uint64_t)UINT_MAX);
        slice = elapsed >= timeout_ms ? 0 : min(timeout_ms - elapsed, 20u);
        if (vkd3d_native_sync_handle_acquire_timeout(event->handle, slice))
        {
            /* The notification is latched even if a later CPU signal rewinds
             * the fence before this thread resumes. */
            vkd3d_atomic_uint32_store_explicit(&event->completed, 1, vkd3d_memory_order_release);
            continue;
        }
        if (elapsed >= timeout_ms) { hr = (HRESULT)0x887a000a; break; } /* WAS_STILL_DRAWING */
    }
    vkd3d_wddm_fence_event_release(event);
    return hr;
}

void vkd3d_wddm_fence_event_destroy(struct vkd3d_wddm_fence_event *event)
{
    if (!event) return;
    vkd3d_wddm_fence_event_cancel(event);
    vkd3d_wddm_fence_event_release(event);
}
