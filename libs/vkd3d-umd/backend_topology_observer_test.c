/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only translation unit: each observer forwards unchanged arguments once
 * to the actual COM callee. Production DLLs compile backend.c directly. */
#define COBJMACROS
#include "vkd3d.h"
#include "backend_topology_observer_test.h"
static struct vkdu_topology_observation topology_observation;
static void observe_topology(ID3D12GraphicsCommandList *list, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    ++topology_observation.setter_before;
    list->lpVtbl->IASetPrimitiveTopology(list,topology);
    ++topology_observation.setter_after;
}
static void observe_draw(ID3D12GraphicsCommandList *list, UINT vertices,UINT instances,UINT first,UINT first_instance)
{
    ++topology_observation.draw_before;
    list->lpVtbl->DrawInstanced(list,vertices,instances,first,first_instance);
    ++topology_observation.draw_after;
}
static void observe_indexed(ID3D12GraphicsCommandList *list,UINT indices,UINT instances,UINT first,INT base,UINT first_instance)
{
    ++topology_observation.indexed_before;
    list->lpVtbl->DrawIndexedInstanced(list,indices,instances,first,base,first_instance);
    ++topology_observation.indexed_after;
}
#undef ID3D12GraphicsCommandList_IASetPrimitiveTopology
#undef ID3D12GraphicsCommandList_DrawInstanced
#undef ID3D12GraphicsCommandList_DrawIndexedInstanced
#define ID3D12GraphicsCommandList_IASetPrimitiveTopology observe_topology
#define ID3D12GraphicsCommandList_DrawInstanced observe_draw
#define ID3D12GraphicsCommandList_DrawIndexedInstanced observe_indexed
#include "backend.c"
struct vkdu_topology_observation vkdu_test_topology_observation(void)
{ return topology_observation; }
