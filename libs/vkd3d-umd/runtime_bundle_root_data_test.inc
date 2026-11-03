/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_bundle_root_data() {
    for (unsigned mode=0;mode<3;++mode) {
        D3DDDI_ADAPTERCALLBACKS ac{};ac.pfnQueryAdapterInfoCb=test_query;
        D3D12DDI_ADAPTERFUNCS adapter{};D3D12DDIARG_OPENADAPTER open{};
        open.hRTAdapter.handle=expected_adapter;open.pAdapterCallbacks=&ac;open.pAdapterFuncs=&adapter;
        REQUIRE(OpenAdapter12(&open)==S_OK);
        D3DDDI_DEVICECALLBACKS kt{};D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
        um.pfnSetErrorCb=test_error;um.pfnSetCommandListErrorCb=test_command_error;
        alignas(NativeDevice) std::array<unsigned char,sizeof(NativeDevice)> dm{};
        D3D12DDIARG_CREATEDEVICE_0003 create{};create.hDrvDevice.pDrvPrivate=dm.data();create.hRTDevice.handle=expected_device;
        create.Interface=D3D12DDI_INTERFACE_VERSION_R0;create.Version=D3D12DDI_BUILD_VERSION<<16;
        create.pKTCallbacks=&kt;create.p12UMCallbacks=&um;REQUIRE(adapter.pfnCreateDevice(open.hAdapter,&create)==S_OK);
        D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&table,&commands,&queues)==S_OK && commands.pfnClearRootArguments && commands.pfnExecuteBundle);
        UINT32 count=7;REQUIRE(adapter.pfnGetSupportedVersions(open.hAdapter,&count,nullptr)==S_OK && count==0);
        auto guarded=commands;std::memset(&guarded,0xa5,sizeof(guarded));const auto before=guarded;
        REQUIRE(adapter.pfnFillDDITable(open.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&guarded,sizeof(guarded),0,{})==DXGI_ERROR_UNSUPPORTED);
        REQUIRE(std::memcmp(&guarded,&before,sizeof(guarded))==0);
        alignas(Object) std::array<unsigned char,sizeof(Object)> am{},bm{};
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};allocator.hDrvCommandAllocator.pDrvPrivate=am.data();
        allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_BUNDLE;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&allocator)==S_OK);
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 bundle{};bundle.hDrvCommandList.pDrvPrivate=bm.data();
        bundle.hRTCommandList.handle=reinterpret_cast<HANDLE>(uintptr_t(0xbcdc00+mode));
        bundle.Type=D3D12DDI_COMMAND_LIST_TYPE_BUNDLE;bundle.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        bundle.hDrvCommandAllocator=allocator.hDrvCommandAllocator;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&bundle)==S_OK);
        const auto calls=root_clear_calls;command_errors.clear();
        commands.pfnClearRootArguments(bundle.hDrvCommandList);
        REQUIRE(root_clear_calls==calls+1 && root_clear_command_type==1 && command_errors.empty());
        if(!mode){
            commands.pfnCloseCommandList(bundle.hDrvCommandList);
            commands.pfnClearRootArguments(bundle.hDrvCommandList);
            REQUIRE(command_errors.size()==1 && command_errors.back()==std::make_pair(bundle.hRTCommandList.handle,HRESULT(E_INVALIDARG)));
            table.pfnDestroyCommandList(create.hDrvDevice,bundle.hDrvCommandList);
        }else{
            bool retired=false;root_clear_result=E_OUTOFMEMORY;
            root_clear_callback=[&]{
                table.pfnDestroyCommandList(create.hDrvDevice,bundle.hDrvCommandList);bm.fill(0xa5);
                if(mode==2){table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);adapter.pfnDestroyDevice(create.hDrvDevice);dm.fill(0xa5);}
                retired=true;
            };
            commands.pfnClearRootArguments(bundle.hDrvCommandList);
            REQUIRE(retired && root_clear_calls==calls+2 && root_clear_command_type==1);
            if(mode==1)REQUIRE(command_errors.size()==1 && command_errors.back()==std::make_pair(bundle.hRTCommandList.handle,HRESULT(E_OUTOFMEMORY)));
            else REQUIRE(command_errors.empty());
            root_clear_callback={};root_clear_result=S_OK;
        }
        if(mode!=2){table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);adapter.pfnDestroyDevice(create.hDrvDevice);}
        REQUIRE(adapter.pfnCloseAdapter(open.hAdapter)==S_OK);
    }
    puts("PASS bundle root data typed DDI: actual BUNDLE ClearRootArguments route,closed refusal,retained callback/error identity and recursive device retirement; public gates closed (controlled peers)");
    return 0;
}
