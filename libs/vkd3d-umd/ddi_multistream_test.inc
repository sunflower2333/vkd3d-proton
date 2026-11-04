/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Runs inside the controlled native stage fixture with the actual production
 * creation/PSO table. The backend oracle checks the copied data after release. */
    {
        Object multi{};
        UINT words[512], word_count=stage_multistream_raster_tokens(words);
        DWORD previous;
        auto *tokens_page=static_cast<BYTE *>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        auto *decl_page=static_cast<BYTE *>(VirtualAlloc(nullptr,12288,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        REQUIRE(tokens_page && decl_page && VirtualProtect(tokens_page+4096,4096,PAGE_NOACCESS,&previous) &&
            VirtualProtect(decl_page+8192,4096,PAGE_NOACCESS,&previous));
        auto *guard_tokens=reinterpret_cast<UINT *>(tokens_page+4096)-word_count;
        auto *decl=reinterpret_cast<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *>(decl_page+8192)-512;
        std::memcpy(guard_tokens,words,word_count*4);
        for (UINT stream=0;stream<4;++stream) {
            for (UINT i=0;i<127;++i)decl[stream*128+i]={stream,stream,UINT32_MAX,1};
            decl[stream*128+127]={stream,stream,0,8};
        }
        UINT strides4[]={512,512,512,512};
        auto multi_io=io;multi_io.NumInputSignatureEntries=0;multi_io.pInputSignatureDeprecated=nullptr;
        D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT multi_create{guard_tokens,{&root},decl,512,strides4,4,3};
        reported=S_OK;device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&multi},&multi_io,{});
        REQUIRE(reported==S_OK && multi.native_graphics_tag==8);
        std::memset(guard_tokens,0xcc,word_count*4);std::memset(decl,0xcc,512*sizeof(*decl));
        REQUIRE(VirtualFree(tokens_page,0,MEM_RELEASE) && VirtualFree(decl_page,0,MEM_RELEASE));
        std::memset(strides4,0xcc,sizeof(strides4));
        pipeline.hGeometryShader={&multi};
        graphics_context=&ctx;graphics_slots[0]=&multi;graphics_hook=graphics_poison;
        REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && graphics_locks_free && geometry_bytes && graphics_so.entry_count==512 &&
            graphics_so.rasterized_stream==3 && graphics_so.strides[3]==512);
        for (UINT stream=0;stream<4;++stream) {
            REQUIRE(graphics_so.entries[stream*128+126].reg==UINT32_MAX &&
                graphics_so.entries[stream*128+127].stream==stream && graphics_so.entries[stream*128+127].slot==stream &&
                graphics_so.entries[stream*128+127].reg==0 && graphics_so.entries[stream*128+127].mask==8);
        }
        graphics_hook=nullptr;graphics_slots[0]=nullptr;
        device.pfnDestroyPipelineState(h,{&pso});multi={};
        D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY small[]={{0,0,0,8},{1,1,0,8}};
        UINT short_strides[]={4,4};
        multi_create={words,{&root},small,2,short_strides,2,UINT32_MAX};
        small[1].OutputSlot=0;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&multi_io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);small[1].OutputSlot=1;
        small[1].Stream=0;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&multi_io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);small[1].Stream=4;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&multi_io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);small[1].Stream=1;
        const UINT topology_word=7;REQUIRE(words[topology_word]==0x0100085c);
        words[topology_word]=0x0100185c; /* A multi-stream GS cannot output line strips. */
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&multi_io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);words[topology_word]=0x0100085c;
        D3D12DDIARG_SIGNATURE_ENTRY builtin_link[2]={link[0],link[1]};
        builtin_link[0].SystemValue=D3D10_SB_NAME_CLIP_DISTANCE;
        auto builtin_io=multi_io;builtin_io.pOutputSignatureDeprecated=builtin_link;
        for(UINT pos=2;pos<word_count;pos+=(words[pos]>>24)&127)
            if((words[pos]&0x7ff)==103)words[pos+3]=2;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&builtin_io,{});
        REQUIRE(reported==E_NOTIMPL && !bad.magic);
        stage_multistream_raster_tokens(words);
        multi_create.NumEntries=513;multi_create.pOutputStreamDecl=reinterpret_cast<const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *>(1);
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},&multi_io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        multi_create={nullptr,{&root},small,2,short_strides,2,UINT32_MAX};
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},nullptr,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        small[0]={1,0,UINT32_MAX,1};multi_create.NumEntries=1;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&bad},nullptr,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        multi_create={words,{&root},nullptr,0,nullptr,0,3};reported=S_OK;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&multi_create,{&multi},&multi_io,{});
        REQUIRE(reported==S_OK && multi.native_graphics_tag==8);
        pipeline.hGeometryShader={&multi};
        REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && graphics_has_so && !graphics_so.entry_count &&
            graphics_so.rasterized_stream==3 && geometry_bytes);
        device.pfnDestroyPipelineState(h,{&pso});device.pfnDestroyShader(h,{&multi});pipeline.hGeometryShader={&so};
    }
