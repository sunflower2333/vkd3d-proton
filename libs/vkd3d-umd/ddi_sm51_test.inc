/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The unchanged generated headers also declare unused public-API bytecode
 * wrappers. The DDI-only test TU needs their layout, not the public API header. */
struct Sm51TestBytecode { const void *pShaderBytecode; SIZE_T BytecodeLength; };
#define D3D12_SHADER_BYTECODE Sm51TestBytecode
#include "sm51_test_shaders.h"
#undef D3D12_SHADER_BYTECODE
static int check_native_sm51(uint32_t index)
{
    const auto &f = sm51_fixtures[index];
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    next_result = reported = S_OK;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    Object root{}, shader{};
    REQUIRE(bind(&ctx, &root, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
    uint32_t words; const auto *original = sm51_chunk(f.container,f.size,0x58454853,&words);
    REQUIRE(original && words < 256);
    UINT tokens[256]{}; std::memcpy(tokens,original,words*4);
    D3D12DDIARG_SIGNATURE_ENTRY inputs[3]{}, outputs[1]{};
    auto translate = [](const vkdu_signature *from, UINT count, D3D12DDIARG_SIGNATURE_ENTRY *to) {
        for (UINT i=0;i<count;++i) {
            to[i].SystemValue=static_cast<D3D10_SB_NAME>(from[i].system_value);
            to[i].Register=from[i].reg; to[i].Mask=from[i].mask;
            to[i].RegisterComponentType=static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(from[i].component_type);
        }
    };
    translate(f.inputs,f.input_count,inputs); translate(f.outputs,f.output_count,outputs);
    D3D12DDIARG_STAGE_IO_SIGNATURES io{};
    io.NumInputSignatureEntries=f.input_count;io.NumOutputSignatureEntries=f.output_count;
    io.pInputSignatureDeprecated=inputs;io.pOutputSignatureDeprecated=outputs;
    auto create=f.stage ? device.pfnCreateVertexShader : device.pfnCreatePixelShader;
    create({&ctx},tokens,{&root},{&shader},&io,static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    if (reported != S_OK) {
        std::fprintf(stderr,"FAIL SM51 native %s returned %08x; unpublished=%u\n",
            f.name,static_cast<unsigned>(reported),!shader.magic && !ctx.graphics_objects);
        REQUIRE(!shader.magic && !ctx.graphics_objects);
        VioGpuD3D12BridgeUnbindObject(&root); REQUIRE(ctx.references==1); return 1;
    }
    REQUIRE(shader.magic == object_magic && shader.native_graphics_tag == (f.stage ? 1u : 2u));
    REQUIRE(sm51_preserved(&f,shader.shader,shader.shader_words*4));
    /* A second create into an occupied slot must retain the original allocation. */
    auto *owned=shader.shader;const auto owned_words=shader.shader_words;const auto references=ctx.references.load();
    reported=S_OK;create({&ctx},tokens,{&root},{&shader},&io,static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    REQUIRE(reported==E_INVALIDARG && shader.shader==owned && shader.shader_words==owned_words && ctx.references==references);
    REQUIRE(sm51_preserved(&f,shader.shader,shader.shader_words*4));
    Object rejected{}, wrong_root{};
    REQUIRE(bind(&ctx,&wrong_root,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    for(UINT which=0;which<8;++which) {
        std::memcpy(tokens,original,words*4);
        auto request=io;auto root_handle=D3D12DDI_HROOTSIGNATURE{&root};
        auto flags=static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0);
        if(which==0)tokens[0]=(tokens[0]&~255u)|0x52;
        if(which==1)tokens[0]^=0x10000;
        if(which==2)tokens[1]=1;
        if(which==3)tokens[2]&=0x00ffffff;
        if(which==4)flags=static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(1);
        if(which==5)root_handle.pDrvPrivate=&wrong_root;
        if(which==6)root_handle.pDrvPrivate=nullptr;
        if(which==7){request.NumInputSignatureEntries=1;request.pInputSignatureDeprecated=nullptr;}
        const auto before=ctx.references.load();reported=S_OK;
        create({&ctx},tokens,root_handle,{&rejected},&request,flags);
        REQUIRE(reported==E_INVALIDARG && !rejected.magic && ctx.references==before);
        REQUIRE(shader.shader==owned && sm51_preserved(&f,shader.shader,shader.shader_words*4));
    }
    VioGpuD3D12BridgeUnbindObject(&wrong_root);
    std::memset(tokens,0xcc,sizeof(tokens));std::memset(inputs,0xcc,sizeof(inputs));std::memset(outputs,0xcc,sizeof(outputs));
    REQUIRE(sm51_preserved(&f,shader.shader,shader.shader_words*4));
    VioGpuD3D12BridgeUnbindObject(&shader);VioGpuD3D12BridgeUnbindObject(&root);
    REQUIRE(ctx.references==1 && !ctx.graphics_objects);
    std::printf("PASS SM51 native %s actual R0 builder, owned SHEX/signatures, 9 rejection controls and cleanup\n",f.name);
    return 0;
}
