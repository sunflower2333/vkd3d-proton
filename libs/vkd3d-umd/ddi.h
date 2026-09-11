/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include "backend.h"

/* Native OpenAdapter12 plus internal bridge entrypoints. No complete DDI
 * version/feature level is advertised until the full native contract exists.
 * The runtime-private memory supplied for object handles must be zeroed first.
 * Missing table slots stay NULL and must never be advertised to the runtime. */
typedef void (APIENTRY *VKDU_REPORT_ERROR)(void *context, HRESULT result);
extern "C" {
HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *args);
HRESULT APIENTRY VioGpuD3D12BridgeCreate(PFN_vkGetInstanceProcAddr loader, const vkdu_adapter *adapter,
        VKDU_REPORT_ERROR report, void *report_context, D3D12DDI_HDEVICE *device);
void APIENTRY VioGpuD3D12BridgeDestroy(D3D12DDI_HDEVICE device);
HRESULT APIENTRY VioGpuD3D12BridgeStatus(D3D12DDI_HDEVICE device);
SIZE_T APIENTRY VioGpuD3D12BridgeObjectSize(void);
HRESULT APIENTRY VioGpuD3D12BridgeGetTables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device,
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands, D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue);
/* Ownership transfers on success only. Existing backing must be bound by the
 * native allocation contract before this can become a system-runtime resource.
 * This helper is NOT CreateHeapAndResource or a shared-allocation implementation. */
HRESULT APIENTRY VioGpuD3D12BridgeBindObject(D3D12DDI_HDEVICE device, void *private_memory,
        vkdu_object *object, vkdu_kind kind);
void APIENTRY VioGpuD3D12BridgeUnbindObject(void *private_memory);
}
