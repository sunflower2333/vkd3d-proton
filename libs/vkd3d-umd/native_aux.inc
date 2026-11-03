/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Auxiliary R0 DDI entries that the D3D12 runtime requires to be non-NULL.
 *
 * Each implements a legal minimal behaviour for this driver's advertised
 * capabilities (tiled resources NOT_SUPPORTED, no PIX shader comments, no
 * serialized PSO cache):
 *  - GetMipPacking: no packed mips for untiled resources.
 *  - Serialization: SerializeObject declines (E_NOTIMPL); deserialization
 *    reports a driver mismatch, which the runtime surfaces to applications as a
 *    cache miss (D3D12_ERROR_DRIVER_VERSION_MISMATCH) so they recompile.
 *  - Offer/Reclaim: offering is a residency hint; contents are always retained,
 *    so every reclaim reports "not discarded".
 *  - RetrieveShaderComment: no comment (empty string).
 *  - DiscardResource: a hint that leaves contents undefined; keeping them is legal.
 *  - CopyTiles: unreachable while TiledResourcesTier is NOT_SUPPORTED.
 *  - Blt: presentation blit, not implemented yet; reports E_NOTIMPL.
 *  - SetMarker: debug marker, no GPU effect.
 */

VOID APIENTRY native_get_mip_packing(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, UINT *packed, UINT *tiles) {
    if (packed) *packed = 0;
    if (tiles) *tiles = 0;
}

HRESULT APIENTRY native_serialize_object(D3D12DDI_HDEVICE, D3D12DDI_HANDLETYPE, HANDLE, SIZE_T *size,
        const UINT **blob) {
    if (size) *size = 0;
    if (blob) *blob = nullptr;
    return E_NOTIMPL;
}

VOID APIENTRY native_destroy_object_serialization(D3D12DDI_HDEVICE, SIZE_T, const UINT *) {}

SIZE_T APIENTRY native_deserialized_object_size(D3D12DDI_HDEVICE, D3D12DDI_HANDLETYPE, SIZE_T, const UINT *) {
    // Nonzero so the runtime proceeds to CreateDeserializedObject, which then
    // reports the mismatch through its HRESULT.
    return sizeof(void *);
}

HRESULT APIENTRY native_create_deserialized_object(D3D12DDI_HDEVICE, D3D12DDI_HANDLETYPE, SIZE_T, const UINT *,
        VOID *, VOID *) {
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
}

HRESULT APIENTRY native_offer_resources(D3D12DDI_HDEVICE, const D3D12DDIARG_OFFERRESOURCES *) {
    return S_OK;
}

HRESULT APIENTRY native_reclaim_resources(D3D12DDI_HDEVICE, D3D12DDIARG_RECLAIMRESOURCES_0001 *args) {
    if (!args) return E_INVALIDARG;
    if (args->pDiscarded)
        for (UINT i = 0; i < args->NumObjects; ++i) args->pDiscarded[i] = FALSE;
    args->WaitMask = 0; // Already resident: no paging fence to wait on.
    return S_OK;
}

HRESULT APIENTRY native_retrieve_shader_comment(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE, WCHAR *buffer,
        SIZE_T *count) {
    if (!count) return E_INVALIDARG;
    if (buffer && *count) buffer[0] = 0;
    *count = 1;
    return S_OK;
}

VOID APIENTRY native_discard_resource(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE,
        const D3D12DDIARG_DISCARD_RESOURCE_0003 *) {}

VOID APIENTRY native_copy_tiles(D3D12DDI_HCOMMANDLIST command, D3D12DDI_HRESOURCE,
        const D3D12DDI_TILED_RESOURCE_COORDINATE *, const D3D12DDI_TILE_REGION_SIZE *, D3D12DDI_HRESOURCE, UINT64,
        D3D12DDI_TILE_COPY_FLAGS) {
    if (auto *value = object(command.pDrvPrivate)) error(value, E_NOTIMPL);
}

VOID APIENTRY native_blt(D3D12DDI_HCOMMANDLIST command, const D3D12DDIARG_BLT *) {
    if (auto *value = object(command.pDrvPrivate)) error(value, E_NOTIMPL);
}

VOID APIENTRY native_set_marker(D3D12DDI_HCOMMANDLIST, UINT64) {}

void native_aux_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands) {
    device->pfnGetMipPacking = native_get_mip_packing;
    device->pfnSerializeObject = native_serialize_object;
    device->pfnDestroyObjectSerialization = native_destroy_object_serialization;
    device->pfnCalcPrivateDeserializedObjectSize = native_deserialized_object_size;
    device->pfnCreateDeserializedObject = native_create_deserialized_object;
    device->pfnOfferResources = native_offer_resources;
    device->pfnReclaimResources = native_reclaim_resources;
    device->pfnRetrieveShaderComment = native_retrieve_shader_comment;
    commands->pfnDiscardResource = native_discard_resource;
    commands->pfnCopyTiles = native_copy_tiles;
    commands->pfnBlt = native_blt;
    commands->pfnSetMarker = native_set_marker;
}
