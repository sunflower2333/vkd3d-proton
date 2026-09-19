/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_WDDM_PRIVATE_H
#define VKD3D_WDDM_PRIVATE_H
#include "vkd3d.h"
#include "mesa_wddm_runtime.h"
/* Separate private entrypoints leave the public create_info ABI unchanged. */
HRESULT vkd3d_create_device_wddm(const struct vkd3d_device_create_info *info,
        const struct mwd_device_create_info *runtime, REFIID iid, void **device);
HRESULT vkd3d_create_heap_wddm(ID3D12Device *device, const D3D12_HEAP_DESC *desc,
        void *owner, void *token, ID3D12Heap **heap);
HRESULT vkd3d_wddm_queue_bind(ID3D12CommandQueue *queue, void *owner, void *token);
HRESULT vkd3d_wddm_queue_drain_enqueue(ID3D12CommandQueue *queue);
/* Private embedded-fence event ownership, not an OS monitored-fence import. */
struct vkd3d_wddm_fence_event;
HRESULT vkd3d_wddm_device_lost(ID3D12Device *device, HRESULT reason);
HRESULT vkd3d_wddm_fence_event_create(ID3D12Fence *fence, UINT64 value,
        struct vkd3d_wddm_fence_event **out);
HRESULT vkd3d_wddm_fence_event_wait(struct vkd3d_wddm_fence_event *event, UINT timeout_ms);
void vkd3d_wddm_fence_event_cancel(struct vkd3d_wddm_fence_event *event);
void vkd3d_wddm_fence_event_destroy(struct vkd3d_wddm_fence_event *event);
#endif
