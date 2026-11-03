/* SPDX-License-Identifier: LGPL-2.1-or-later */
    for (UINT stage=1;stage<=2;++stage) for (UINT system=2;system<=3;++system) {
        Object clipped{},clipped_vs{};
        const UINT systems[]={system,system},masks[]={14,12},values[2][4]={{1,2,3,4},{5,6,7,8}};
        UINT code_words[96],n=stage_distance_tokens(stage,systems,masks,values,code_words),stride=24;
        BYTE *pages[3]{};DWORD previous;
        for (auto &page:pages) {
            page=static_cast<BYTE *>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            REQUIRE(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&previous));
        }
        auto *code=reinterpret_cast<UINT *>(pages[0]+4096)-n;
        auto *sig=reinterpret_cast<D3D12DDIARG_SIGNATURE_ENTRY *>(pages[1]+4096)-3;
        auto *decl=reinterpret_cast<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *>(pages[2]+4096)-3;
        std::memcpy(code,code_words,n*4);
        sig[0]=link[0];sig[1]=link[0];sig[1].SystemValue=static_cast<D3D10_SB_NAME>(system);
        sig[1].Register=2;sig[1].Mask=12;sig[2]=sig[1];sig[2].Register=1;sig[2].Mask=14;
        decl[0]={0,0,2,12};decl[1]={0,0,UINT32_MAX,1};decl[2]={0,0,1,14};
        D3D12DDIARG_STAGE_IO_SIGNATURES sigs{};sigs.pOutputSignatureDeprecated=sig;sigs.NumOutputSignatureEntries=3;
        D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT request{code,{&root},decl,3,&stride,1,UINT32_MAX};
        reported=S_OK;device.pfnCreateGeometryShaderWithStreamOutput(h,&request,{&clipped},&sigs,{});
        REQUIRE(reported==S_OK && clipped.native_graphics_tag==8);
        const auto *header=reinterpret_cast<const NativeGeometryOutput *>(clipped.shader);
        const auto *container=clipped.shader+sizeof(*header)/4;
        std::vector<uint32_t> expected(container,container+header->code_words);
        const UINT *signature=container+container[9]/4+2;const UINT width=stage==2 ? 7 : 6,shift=stage==2 ? 1 : 0;
        REQUIRE(signature[0]==3);
        for (UINT i=0,seen=0;i<3;++i) {
            const UINT *e=signature+2+i*width+shift;
            if (e[2]==system) { REQUIRE(e[1]==e[4]-1 && e[5]==(e[4]==1 ? 14u : 12u));++seen; }
            if (i==2)REQUIRE(seen==2);
        }
        if (stage==1) {
            reported=S_OK;device.pfnCreateVertexShader(h,code,{&root},{&clipped_vs},&sigs,{});
            REQUIRE(reported==S_OK && clipped_vs.magic);
        }
        /* Invalid distance type, then overlapping capture and three distance
         * registers. The production callbacks must leave no live shader. */
        sig[1].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_UINT32;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&request,{&bad},&sigs,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);sig[1].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
        decl[2]=decl[0];device.pfnCreateGeometryShaderWithStreamOutput(h,&request,{&bad},&sigs,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);decl[2]={0,0,1,14};
        sig[0].SystemValue=static_cast<D3D10_SB_NAME>(system);
        for (UINT at=2;at<n;at+=(code[at]>>24)&127)
            if ((code[at]&2047)==103 && code[at+2]==0)code[at+3]=system;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&request,{&bad},&sigs,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        for (UINT i=0;i<3;++i) { std::memset(pages[i],0xcc,4096);REQUIRE(VirtualFree(pages[i],0,MEM_RELEASE)); }
        stride=0;
        pipeline.hGeometryShader={&clipped};pipeline.hVertexShader=stage==1 ? D3D12DDI_HSHADER{&clipped_vs} : D3D12DDI_HSHADER{&vs};
        graphics_context=&ctx;graphics_slots[0]=&clipped;graphics_slots[1]=stage==1 ? &clipped_vs : nullptr;
        graphics_hook=graphics_poison;graphics_copy_programs=true;
        REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && graphics_locks_free && graphics_so.entry_count==3 &&
            graphics_so.strides[0]==24 && graphics_so.entries[0].reg==2 && graphics_so.entries[2].mask==14);
        REQUIRE((stage==1 ? graphics_vertex_code : graphics_geometry_code)==expected);
        graphics_copy_programs=false;graphics_hook=nullptr;graphics_slots[0]=graphics_slots[1]=nullptr;
        device.pfnDestroyPipelineState(h,{&pso});pipeline.hGeometryShader={&so};pipeline.hVertexShader={&vs};
    }
    puts("PASS native split Clip/Cull: guarded token/signature/declaration ends, stable indices, type/register/overlap rejection, caller poison/free and exact copied PSO code after recursive shader destruction");
