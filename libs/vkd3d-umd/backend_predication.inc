/* SPDX-License-Identifier: LGPL-2.1-or-later */
int32_t vkdu_command_predication(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t operation)
{
    ID3D12Resource *native = buffer ? OBJ(ID3D12Resource, buffer) : NULL;
    HRESULT hr;
    if (!RECORDING(command) || (command->command_type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            command->command_type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            command->command_type != D3D12_COMMAND_LIST_TYPE_COPY) || operation > 1 ||
            (buffer && (!VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) ||
                        (offset & 7) || offset > buffer->bytes || 8 > buffer->bytes - offset))) return E_INVALIDARG;
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_PREDICATION_DROP_OWNER)
    if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 1, &native))) return hr;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_PREDICATION_DROP)
    native = NULL;
#endif
    hr = vkd3d_wddm_command_set_predication(OBJ(ID3D12GraphicsCommandList, command), native, offset, operation);
    if (SUCCEEDED(hr)) command->predicate_enabled = native != NULL;
    return hr;
}
