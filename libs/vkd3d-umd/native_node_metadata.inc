/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* R0 metadata for the native backend's sole physical adapter and node.
 * HDEVICE is trusted runtime-owned storage, as for the other native DDIs.
 * This does not advertise implicit multi-adapter or a complete device. */
bool native_node_metadata_ready(NativeHeapOperation &op) {
    if (!op.live()) return false;
    const HRESULT prior = op.ctx->last_error.load();
    if (prior == DXGI_ERROR_DEVICE_REMOVED || prior == DXGI_ERROR_DEVICE_RESET ||
            prior == DXGI_ERROR_DEVICE_HUNG || prior == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        error(op.ctx, prior);
        return false;
    }
    return true;
}

UINT APIENTRY native_implicit_physical_adapter_mask(D3D12DDI_HDEVICE device) {
    NativeHeapOperation op(context(device));
    return native_node_metadata_ready(op) ? 1u : 0u;
}

VOID APIENTRY native_query_node_map(D3D12DDI_HDEVICE device,
        UINT count, UINT *map) {
    NativeHeapOperation op(context(device));
    if (!native_node_metadata_ready(op)) return;
    // Defensive shape policy: the native KMD path has exactly one adapter.
    // The WDK requires count output entries; it grants no null/count query
    // convention. Reject unsupported shapes without partially writing a map.
    if (count != 1 || !map) {
        error(op.ctx, E_INVALIDARG);
        return; // SetErrorCb can retire the device and free the output storage.
    }
    // Physical adapter0 is exposed as node ordinal0 (not the bit mask1).
    map[0] = 0;
}

void native_node_metadata_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device) {
    device->pfnGetImplicitPhysicalAdapterMask = native_implicit_physical_adapter_mask;
    device->pfnQueryNodeMap = native_query_node_map;
}
