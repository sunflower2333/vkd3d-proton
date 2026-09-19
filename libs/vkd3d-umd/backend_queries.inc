/* SPDX-License-Identifier: LGPL-2.1-or-later */
static HRESULT retain_query_owners(vkdu_object *command, vkdu_object *heap, vkdu_object *destination)
{
#ifdef VKDU_TEST_QUERY_DROP_OWNER
    (void)command; (void)heap; (void)destination;
    return S_OK;
#else
    return vkd3d_wddm_command_retain_query(OBJ(ID3D12GraphicsCommandList, command),
            OBJ(ID3D12QueryHeap, heap), destination ? OBJ(ID3D12Resource, destination) : NULL);
#endif
}
static int query_matches(vkdu_object *command, vkdu_object *heap, uint32_t type)
{
    uint32_t expected;
    if (!RECORDING(command) || !VALID(heap, VKDU_QUERY_HEAP) ||
            !vkdu_same_device(command, heap) || type > D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3 ||
            command->command_type == D3D12_COMMAND_LIST_TYPE_COPY ||
            (command->command_type == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
             type != D3D12_QUERY_TYPE_TIMESTAMP && type != D3D12_QUERY_TYPE_PIPELINE_STATISTICS))
        return 0;
    expected = type <= D3D12_QUERY_TYPE_BINARY_OCCLUSION ? D3D12_QUERY_HEAP_TYPE_OCCLUSION :
        type == D3D12_QUERY_TYPE_TIMESTAMP ? D3D12_QUERY_HEAP_TYPE_TIMESTAMP :
        type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS :
        D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
    return heap->query_type == expected;
}

int32_t vkdu_command_query(vkdu_object *command, vkdu_object *heap, uint32_t type, uint32_t index, int begin)
{
    struct vkdu_query_scope *scopes;
    uint32_t i, capacity;
    HRESULT hr;
    if (!query_matches(command, heap, type) || index >= heap->query_count ||
            (begin && type == D3D12_QUERY_TYPE_TIMESTAMP)) return E_INVALIDARG;
    for (i = 0; i < command->query_scope_count; i++)
        if (command->query_scopes[i].heap == heap->object && command->query_scopes[i].index == index) break;
    if (begin)
    {
        if (i != command->query_scope_count) return E_INVALIDARG;
        if (command->query_scope_count == command->query_scope_capacity)
        {
            if (command->query_scope_capacity >= 65536) return E_OUTOFMEMORY;
            capacity = command->query_scope_capacity ? command->query_scope_capacity * 2 : 8;
            if (!(scopes = realloc(command->query_scopes, capacity * sizeof(*scopes)))) return E_OUTOFMEMORY;
            command->query_scopes = scopes; command->query_scope_capacity = capacity;
        }
    }
    else if (type != D3D12_QUERY_TYPE_TIMESTAMP &&
            (i == command->query_scope_count || command->query_scopes[i].type != type)) return E_INVALIDARG;
    hr = retain_query_owners(command, heap, NULL);
    if (FAILED(hr)) return hr;
    if (begin)
    {
        command->query_scopes[command->query_scope_count++] = (struct vkdu_query_scope){heap->object, type, index};
        ID3D12GraphicsCommandList_BeginQuery(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12QueryHeap, heap), type, index);
    }
    else
    {
        if (type != D3D12_QUERY_TYPE_TIMESTAMP)
            command->query_scopes[i] = command->query_scopes[--command->query_scope_count];
        ID3D12GraphicsCommandList_EndQuery(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12QueryHeap, heap), type, index);
    }
    return S_OK;
}

int32_t vkdu_command_query_resolve(vkdu_object *command, vkdu_object *heap, uint32_t type,
        uint32_t first, uint32_t count, vkdu_object *destination, uint64_t offset)
{
    uint64_t stride, bytes;
    uint32_t i;
    HRESULT hr;
    if (!query_matches(command, heap, type) || !VALID(destination, VKDU_BUFFER) ||
            !vkdu_same_device(command, destination) || first > heap->query_count ||
            count > heap->query_count - first || offset % 8) return E_INVALIDARG;
    stride = type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) :
        type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS) : sizeof(uint64_t);
    bytes = stride * count;
    if (offset > destination->bytes || bytes > destination->bytes - offset) return E_INVALIDARG;
    for (i = 0; i < command->query_scope_count; i++)
        if (command->query_scopes[i].heap == heap->object && command->query_scopes[i].index >= first &&
                command->query_scopes[i].index - first < count) return E_INVALIDARG;
    if (!count) return S_OK;
    hr = retain_query_owners(command, heap, destination);
    if (FAILED(hr)) return hr;
#ifndef VKDU_TEST_QUERY_DROP_RESOLVE
    ID3D12GraphicsCommandList_ResolveQueryData(OBJ(ID3D12GraphicsCommandList, command),
            OBJ(ID3D12QueryHeap, heap), type, first, count, OBJ(ID3D12Resource, destination), offset);
#endif
    return S_OK;
}
