/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_bundles() {
    for (unsigned mode=0;mode<3;++mode) {
        D3DDDI_ADAPTERCALLBACKS ac{};ac.pfnQueryAdapterInfoCb=test_query;
        D3D12DDI_ADAPTERFUNCS adapter{};D3D12DDIARG_OPENADAPTER open{};
        open.hRTAdapter.handle=expected_adapter;open.pAdapterCallbacks=&ac;open.pAdapterFuncs=&adapter;
        REQUIRE(OpenAdapter12(&open)==S_OK);
        D3DDDI_DEVICECALLBACKS kt{};D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
        um.pfnSetErrorCb=test_error;um.pfnSetCommandListErrorCb=test_command_error;
        alignas(NativeDevice) std::array<unsigned char,sizeof(NativeDevice)> device_memory{};
        D3D12DDIARG_CREATEDEVICE_0003 create{};create.hDrvDevice.pDrvPrivate=device_memory.data();create.hRTDevice.handle=expected_device;
        create.Interface=D3D12DDI_INTERFACE_VERSION_R0;create.Version=D3D12DDI_BUILD_VERSION<<16;create.pKTCallbacks=&kt;create.p12UMCallbacks=&um;
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter,&create)==S_OK);
        auto *ctx=context(create.hDrvDevice);
        D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&table,&commands,&queue)==S_OK && commands.pfnExecuteBundle);
        UINT32 count=99;REQUIRE(adapter.pfnGetSupportedVersions(open.hAdapter,&count,nullptr)==S_OK && count==0);
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 guarded;std::memset(&guarded,0xa5,sizeof(guarded));auto before=guarded;
        REQUIRE(adapter.pfnFillDDITable(open.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&guarded,sizeof(guarded),0,{})==DXGI_ERROR_UNSUPPORTED);
        REQUIRE(std::memcmp(&before,&guarded,sizeof(guarded))==0);
        alignas(Object) std::array<unsigned char,sizeof(Object)> pa{},ba{},pm{},bm{},bad{};
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};allocator.hDrvCommandAllocator.pDrvPrivate=pa.data();
        allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&allocator)==S_OK);
        auto bundle_allocator=allocator;bundle_allocator.hDrvCommandAllocator.pDrvPrivate=ba.data();bundle_allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_BUNDLE;
        auto invalid=bundle_allocator;invalid.hDrvCommandAllocator.pDrvPrivate=bad.data();invalid.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&invalid)==E_INVALIDARG);
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&bundle_allocator)==S_OK);
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 parent{};parent.hDrvCommandList.pDrvPrivate=pm.data();parent.hRTCommandList.handle=reinterpret_cast<HANDLE>(uintptr_t(0x77880));
        parent.hDrvCommandAllocator=allocator.hDrvCommandAllocator;parent.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;parent.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&parent)==S_OK);
        auto bundle=parent;bundle.hDrvCommandList.pDrvPrivate=bm.data();bundle.hRTCommandList.handle=reinterpret_cast<HANDLE>(uintptr_t(0x77890));bundle.Type=D3D12DDI_COMMAND_LIST_TYPE_BUNDLE;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&bundle)==E_INVALIDARG);
        bundle.hDrvCommandAllocator=bundle_allocator.hDrvCommandAllocator;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&bundle)==S_OK);
        command_errors.clear();commands.pfnExecuteBundle(parent.hDrvCommandList,bundle.hDrvCommandList);
        REQUIRE(command_errors.back()==std::make_pair(parent.hRTCommandList.handle,HRESULT(E_INVALIDARG)));
        commands.pfnCloseCommandList(bundle.hDrvCommandList);command_errors.clear();
        commands.pfnExecuteBundle(parent.hDrvCommandList,{reinterpret_cast<void *>(uintptr_t(1))});
        REQUIRE(command_errors.back().second==E_INVALIDARG);command_errors.clear();
        commands.pfnExecuteBundle(parent.hDrvCommandList,bundle.hDrvCommandList);REQUIRE(command_errors.empty());
        const unsigned calls=bundle_executes;const auto old_generation=generation;
        bundle_callback=[&] {
            if(mode==1){++generation;return;}
            table.pfnDestroyCommandList(create.hDrvDevice,bundle.hDrvCommandList);bm.fill(0xa5);
            table.pfnDestroyCommandList(create.hDrvDevice,parent.hDrvCommandList);pm.fill(0xa5);
            if(mode==2){table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);table.pfnDestroyCommandAllocator(create.hDrvDevice,bundle_allocator.hDrvCommandAllocator);adapter.pfnDestroyDevice(create.hDrvDevice);device_memory.fill(0xa5);}
        };
        bundle_result=mode==0?E_OUTOFMEMORY:S_OK;
        commands.pfnExecuteBundle(parent.hDrvCommandList,bundle.hDrvCommandList);
        REQUIRE(bundle_executes==calls+1);
        if(mode==2)REQUIRE(command_errors.empty());
        else REQUIRE(command_errors.back()==std::make_pair(parent.hRTCommandList.handle,HRESULT(mode==0?E_OUTOFMEMORY:DXGI_ERROR_DEVICE_REMOVED)));
        bundle_callback={};bundle_result=S_OK;generation=old_generation;
        if(mode==1){table.pfnDestroyCommandList(create.hDrvDevice,parent.hDrvCommandList);table.pfnDestroyCommandList(create.hDrvDevice,bundle.hDrvCommandList);}
        if(mode!=2){table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);table.pfnDestroyCommandAllocator(create.hDrvDevice,bundle_allocator.hDrvCommandAllocator);adapter.pfnDestroyDevice(create.hDrvDevice);}
        REQUIRE(adapter.pfnCloseAdapter(open.hAdapter)==S_OK);
    }
    std::puts("PASS native R0 bundle typed DDI: exact BUNDLE type, bad flags/allocator/open/opaque slot, owned retained callbacks, generation and recursive teardown; public gates closed");
    return 0;
}
