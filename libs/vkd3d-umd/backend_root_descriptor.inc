/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by backend.c after the private object and address helpers. */

/* Validate the live buffer and GPU address before changing any root state.
 * Offset bounds do not establish the length of a shader's later memory reads. */
static int32_t vkdu_root_descriptor_address(
        vkdu_object *command, uint32_t index, uint32_t type,
        vkdu_object *buffer, uint64_t offset, uint64_t alignment,
        uint64_t *address)
{
    uint64_t base;
    if (!RECORDING(command) || command->command_type == D3D12_COMMAND_LIST_TYPE_COPY ||
            index >= command->slot_count || command->slots[index].type != type ||
            !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) ||
            offset >= buffer->bytes || (offset & (alignment - 1))) return E_INVALIDARG;
    if ((type == D3D12_ROOT_PARAMETER_TYPE_UAV &&
            !(buffer->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
            (type == D3D12_ROOT_PARAMETER_TYPE_SRV &&
            (buffer->resource_flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) return E_INVALIDARG;
    base = vkdu_buffer_address(buffer);
    if (!base || offset > UINT64_MAX - base || ((base + offset) & (alignment - 1)))
        return E_INVALIDARG;
    *address = base + offset;
    return S_OK;
}

/* Bind only a live, UAV-capable buffer; never wrap its GPU address. */
int32_t vkdu_command_uav(
        vkdu_object *command, uint32_t index,
        vkdu_object *buffer, uint64_t offset)
{
    uint64_t address;
    int32_t hr = vkdu_root_descriptor_address(command, index, D3D12_ROOT_PARAMETER_TYPE_UAV,
            buffer, offset, 4, &address);
    if (hr != S_OK) return hr;
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(
            OBJ(ID3D12GraphicsCommandList, command), index, address);
    return S_OK;
}

/* Root CBVs require a nonzero, non-overflowing 256-byte-aligned address. */
int32_t vkdu_command_cbv(
        vkdu_object *command, uint32_t index,
        vkdu_object *buffer, uint64_t offset)
{
    uint64_t address;
    int32_t hr = vkdu_root_descriptor_address(command, index, D3D12_ROOT_PARAMETER_TYPE_CBV,
            buffer, offset, 256, &address);
    if (hr != S_OK) return hr;
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(
            OBJ(ID3D12GraphicsCommandList, command), index, address);
    return S_OK;
}

/* Preserve SRV denial and DWORD alignment while sharing the address checks. */
int32_t vkdu_command_srv(
        vkdu_object *command, uint32_t index,
        vkdu_object *buffer, uint64_t offset)
{
    uint64_t address;
    int32_t hr = vkdu_root_descriptor_address(command, index, D3D12_ROOT_PARAMETER_TYPE_SRV,
            buffer, offset, 4, &address);
    if (hr != S_OK) return hr;
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(
            OBJ(ID3D12GraphicsCommandList, command), index, address);
    return S_OK;
}
