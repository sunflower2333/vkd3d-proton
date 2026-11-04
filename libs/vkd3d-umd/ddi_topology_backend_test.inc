/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The build extracts this one real backend function and its validation macros.
 * Controlled object layout/callee isolate the typed DDI/error/lifetime path;
 * real Vulkan and actual-callee counts are covered by the backend fixture. */
#ifdef VKDU_TEST_TOPOLOGY_SOURCE_BODY
struct TopologyContractObject {
    void *object;
    vkdu_kind kind;
    uint32_t command_type,topology;
    int closed;
};
static bool topology_contract_active;
static TopologyContractObject topology_contract_state;
static unsigned topology_contract_engine_calls;
static uint32_t topology_contract_engine_value;
static void topology_contract_engine(uint32_t value)
{ ++topology_contract_engine_calls;topology_contract_engine_value=value; }
#pragma push_macro("vkdu_object")
#pragma push_macro("vkdu_command_topology")
#pragma push_macro("OBJ")
#pragma push_macro("VALID")
#pragma push_macro("RECORDING")
#pragma push_macro("ID3D12GraphicsCommandList_IASetPrimitiveTopology")
#define vkdu_object TopologyContractObject
#define vkdu_command_topology topology_contract_apply
#define ID3D12GraphicsCommandList_IASetPrimitiveTopology(ignored, value) topology_contract_engine(value)
#include "topology_backend_body.generated.inc"
#pragma pop_macro("ID3D12GraphicsCommandList_IASetPrimitiveTopology")
#pragma pop_macro("RECORDING")
#pragma pop_macro("VALID")
#pragma pop_macro("OBJ")
#pragma pop_macro("vkdu_command_topology")
#pragma pop_macro("vkdu_object")
#endif
