/* SPDX-License-Identifier: LGPL-2.1-or-later */
    for (UINT system=2;system<=3;++system) for (UINT mask: {15u,12u}) {
        Object producer{},consumer{},missing{};
        const UINT systems[]={system,system},masks[]={15,15},values[2][4]={{1,2,3,4},{5,6,7,8}};
        UINT vertex_words[96],vn=stage_distance_tokens(1,systems,masks,values,vertex_words);
        UINT pixel_words[]={0x50,21,0x0100086a,0x04002064,0x001010f2,0,1,
            0x04001064,0x00101002|(mask<<4),2,system,0x03000065,0x00102002|(mask<<4),0,
            0x05000036,0x00102002|(mask<<4),0,0x00101e46,2,0x0100003e};
        pixel_words[1]=sizeof(pixel_words)/4;
        BYTE *pages[4]{};DWORD previous;
        for (auto &page:pages) {
            page=static_cast<BYTE *>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            REQUIRE(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&previous));
        }
        auto *vcode=reinterpret_cast<UINT *>(pages[0]+4096)-vn;
        auto *pcode=reinterpret_cast<UINT *>(pages[1]+4096)-pixel_words[1];
        auto *outputs=reinterpret_cast<D3D12DDIARG_SIGNATURE_ENTRY *>(pages[2]+4096)-3;
        auto *inputs=reinterpret_cast<D3D12DDIARG_SIGNATURE_ENTRY *>(pages[3]+4096)-3;
        std::memcpy(vcode,vertex_words,vn*4);std::memcpy(pcode,pixel_words,sizeof(pixel_words));
        outputs[0]=link[0];outputs[1]=outputs[2]=link[0];
        outputs[1].SystemValue=outputs[2].SystemValue=static_cast<D3D10_SB_NAME>(system);
        outputs[1].Register=1;outputs[2].Register=2;
        std::memcpy(inputs,outputs,3*sizeof(*inputs));inputs[2].Mask=mask;
        D3D12DDIARG_STAGE_IO_SIGNATURES vss{},pss{};
        D3D12DDIARG_SIGNATURE_ENTRY color=link[0];color.SystemValue=D3D10_SB_NAME_UNDEFINED;color.Mask=mask;
        vss.pOutputSignatureDeprecated=outputs;vss.NumOutputSignatureEntries=3;
        pss.pInputSignatureDeprecated=inputs;pss.NumInputSignatureEntries=3;
        pss.pOutputSignatureDeprecated=&color;pss.NumOutputSignatureEntries=1;
        reported=S_OK;device.pfnCreateVertexShader(h,vcode,{&root},{&producer},&vss,{});
        REQUIRE(reported==S_OK && producer.magic);
        device.pfnCreatePixelShader(h,pcode,{&root},{&consumer},&pss,{});
        REQUIRE(reported==S_OK && consumer.magic);
        pcode[10]=system==2 ? 3 : 2;inputs[2].SystemValue=static_cast<D3D10_SB_NAME>(pcode[10]);
        device.pfnCreatePixelShader(h,pcode,{&root},{&missing},&pss,{});
        REQUIRE(reported==S_OK && missing.magic);
        pcode[10]=system;inputs[2].SystemValue=static_cast<D3D10_SB_NAME>(system);
        device.pfnCreatePixelShader(h,pcode,{&foreign},{&bad},&pss,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        pss.NumInputSignatureEntries=33;pss.pInputSignatureDeprecated=reinterpret_cast<D3D12DDIARG_SIGNATURE_ENTRY *>(1);
        device.pfnCreatePixelShader(h,pcode,{&root},{&bad},&pss,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        pss.NumInputSignatureEntries=3;pss.pInputSignatureDeprecated=inputs;
        /* A MOV cut inside its source operand ends exactly at NOACCESS. */
        const UINT cut=pixel_words[1]-2;auto *truncated=reinterpret_cast<UINT *>(pages[1]+4096)-cut;
        std::memmove(truncated,pixel_words,cut*4);truncated[1]=cut;
        device.pfnCreatePixelShader(h,truncated,{&root},{&bad},&pss,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        for (auto *page:pages) {std::memset(page,0xcc,4096);REQUIRE(VirtualFree(page,0,MEM_RELEASE));}
        auto request=pipeline;request.hVertexShader={&producer};request.hGeometryShader={};request.hPixelShader={&consumer};
        request.NumRenderTargets=1;request.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        request.hPixelShader={&missing};graphics_link_distances=true;
        REQUIRE(device.pfnCreatePipelineState(h,&request)==E_INVALIDARG && !pso.magic && graphics_linked_code.empty() &&
            peer(backend(&root,VKDU_ROOT))->references==1);
        device.pfnDestroyShader(h,{&missing});request.hPixelShader={&consumer};
        graphics_context=&ctx;graphics_slots[0]=&producer;graphics_slots[1]=&consumer;
        graphics_hook=graphics_poison;graphics_link_distances=true;
        REQUIRE(device.pfnCreatePipelineState(h,&request)==S_OK && graphics_locks_free && graphics_link_result==S_OK);
        graphics_hook=nullptr;graphics_link_distances=false;graphics_slots[0]=graphics_slots[1]=nullptr;
        const auto &linked=graphics_linked_code;const UINT *signature=nullptr;
        for (UINT i=0;i<linked[7];++i) if (linked[linked[8+i]/4]==0x31475349) signature=linked.data()+linked[8+i]/4+2;
        REQUIRE(signature && signature[0]==3);
        for (UINT i=0,seen=0;i<3;++i) {
            const UINT *entry=signature+2+8*i;
            if (entry[3]==system) {
                REQUIRE(entry[2]==entry[5]-1 && (entry[6]&15)==15 && (entry[6]>>8)==(entry[5]==2 ? mask : 0));++seen;
            }
            if(i==2)REQUIRE(seen==2);
        }
        /* Framing failures must reject without reading beyond this exact page,
         * and reset output values. This invokes the production helper. */
        auto *page=static_cast<BYTE *>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        REQUIRE(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&previous));
        auto *broken=reinterpret_cast<UINT *>(page+4096)-16;
        const UINT malformed[]={0x43425844,0,0,0,0,1,64,1,36,0x31475349,20,1,8,0,0,0};
        std::memcpy(broken,malformed,sizeof(malformed));void *result=reinterpret_cast<void *>(1);size_t bytes=1;
        REQUIRE(vkdu_shader_link_distances(linked.data(),linked.size()*4,broken,64,0,&result,&bytes)==E_INVALIDARG && !result && !bytes);
        broken[8]=60;result=reinterpret_cast<void *>(1);bytes=1;
        REQUIRE(vkdu_shader_link_distances(linked.data(),linked.size()*4,broken,64,0,&result,&bytes)==E_INVALIDARG && !result && !bytes);
        REQUIRE(VirtualFree(page,0,MEM_RELEASE));
        device.pfnDestroyPipelineState(h,{&pso});
    }
    puts("PASS native cross-stage Clip/Cull subsets: guarded inputs, foreign root/count/truncated-token/missing-producer rejection, caller poison/free, shared complete packing after recursive shader destruction, malformed ISG1 bounds and null outputs");
