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
#endif
