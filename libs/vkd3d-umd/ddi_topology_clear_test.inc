/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_TEST_TOPOLOGY_SOURCE_BODY
static unsigned topology_error_calls;
static HANDLE topology_error_runtime;
static HRESULT topology_error_result;
static void APIENTRY topology_command_error(D3D12DDI_HRTCOMMANDLIST runtime,HRESULT result)
{ ++topology_error_calls;topology_error_runtime=runtime.handle;topology_error_result=result; }
static int check_native_topology_clear()
{
    Context ctx;ctx.backend=reinterpret_cast<vkdu_device *>(&next_base);ctx.report=capture;
    ctx.runtime_device.handle=&ctx;ctx.runtime_callbacks.pfnSetCommandListErrorCb=topology_command_error;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device,&commands,&queue)==S_OK);
    PFND3D12DDI_IA_SETTOPOLOGY_0003 set=commands.pfnIaSetTopology;
    REQUIRE(set!=nullptr);
    Object command{};unsigned failures=0;
    graphics_context=&ctx;graphics_hook=nullptr;for(auto &slot:graphics_slots)slot=nullptr;
    topology_contract_active=true;topology_contract_engine_calls=topology_error_calls=0;
    topology_contract_state={reinterpret_cast<void *>(1),VKDU_COMMAND_LIST,0,0,0};
    REQUIRE(bind(&ctx,&command,make_peer(ctx.backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
    command.runtime_command.handle=reinterpret_cast<HANDLE>(0x543210);
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    REQUIRE(topology_contract_engine_calls==2&&topology_contract_state.topology==4&&topology_error_calls==0);
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
    printf("OBS typed topology zero errors=%u logical=%u engine_calls=%u\n",topology_error_calls,topology_contract_state.topology,topology_contract_engine_calls);
    if(topology_error_calls||topology_contract_state.topology||topology_contract_engine_calls!=2){++failures;puts("FAIL typed topology legal-zero SetError/logical-state");}
    topology_error_calls=0;
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    REQUIRE(topology_contract_engine_calls==3&&topology_contract_state.topology==4&&!topology_error_calls);
    set({&command},static_cast<D3D12DDI_PRIMITIVE_TOPOLOGY>(99));
    REQUIRE(topology_contract_engine_calls==3&&topology_contract_state.topology==4&&topology_error_calls==1&&
        topology_error_result==E_INVALIDARG&&topology_error_runtime==reinterpret_cast<HANDLE>(0x543210));
    for(unsigned type:{2u,3u}) {
        topology_contract_state.command_type=type;topology_error_calls=0;
        set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
        REQUIRE(topology_error_calls==1&&topology_error_result==E_INVALIDARG&&topology_contract_state.topology==4&&topology_contract_engine_calls==3);
    }
    topology_contract_state.command_type=0;topology_contract_state.closed=1;topology_error_calls=0;
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
    REQUIRE(topology_error_calls==1&&topology_error_result==E_INVALIDARG&&topology_contract_state.topology==4&&topology_contract_engine_calls==3);
    topology_contract_state.closed=0;
    reject_retain_peer=command.backend;topology_error_calls=0;
    set({&command},D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
    REQUIRE(topology_error_calls==1&&topology_error_result==E_INVALIDARG&&topology_contract_state.topology==4&&topology_contract_engine_calls==3);
    reject_retain_peer=nullptr;VioGpuD3D12BridgeUnbindObject(&command);
    for(unsigned invalid:{0u,1u}) {
        command={};REQUIRE(bind(&ctx,&command,make_peer(ctx.backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
        command.runtime_command.handle=reinterpret_cast<HANDLE>(uintptr_t(0x654320+invalid));
        topology_contract_state.topology=4;topology_error_calls=0;
        const auto before_destroy=destroys,before_engine=topology_contract_engine_calls;
        graphics_slots[0]=&command;graphics_hook=graphics_poison;graphics_locks_free=false;
        set({&command},static_cast<D3D12DDI_PRIMITIVE_TOPOLOGY>(invalid?99:0));
        graphics_hook=nullptr;graphics_slots[0]=nullptr;
        REQUIRE(graphics_locks_free&&destroys==before_destroy+1&&ctx.references==1&&topology_contract_engine_calls==before_engine);
        if(invalid)REQUIRE(topology_error_calls==1&&topology_error_result==E_INVALIDARG&&
            topology_error_runtime==reinterpret_cast<HANDLE>(uintptr_t(0x654320+invalid))&&topology_contract_state.topology==4);
        else if(topology_error_calls||topology_contract_state.topology){++failures;puts("FAIL typed topology reentrant legal-zero SetError/logical-state");}
        printf("OBS typed topology reentrant invalid=%u errors=%u destroyed=1 retained_until_return=1 locks_free=%u\n",invalid,topology_error_calls,graphics_locks_free);
    }
    REQUIRE(ctx.references==1);topology_contract_active=false;next_result=reported=S_OK;
    printf("%s actual typed topology DDI: legal0 SetError suppression; source-bound backend; same4 twice forwards; invalid99/type/closed/retain; recursive unbind+slot poison+copied runtime identity failures=%u\n",failures?"RED":"PASS",failures);
    return failures?1:0;
}
#endif
