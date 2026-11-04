/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "stage_clip_test_tokens.h"
void vkd3d_compute_dxbc_checksum(const void *dxbc, size_t size, uint32_t checksum[4]);
static void check_split_clip_stream_output(vkdu_device *device)
{
    vkdu_object *root, *pipeline, *queue, *allocator, *command, *fence, *upload, *output, *readback;
    struct vkdu_graphics_desc state = {0,UINT32_MAX,3,1,0,1,15,0,NULL};
    uint32_t round, failures = 0, rejected = 0, executed = 0;
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,4096,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    for (round = 0; round < 4; ++round) {
        const uint32_t system = round < 2 ? 2 : 3, across = round & 1, sentinel = 0x7e630000 + round;
        struct vkdu_signature outputs[] = {{1,0,15,3},{system,1,3,3},{system,2,3,3}};
        struct vkdu_stream_output_desc so = {across ? 2 : 1,1,UINT32_MAX,
            {{0,0,2,3},{0,0,1,2}},{across ? 12 : 8}};
        struct vkdu_stream_output_buffer view = {output,output,256,64,2048};
        uint32_t tokens[40], *words, *sig, i, mismatch = 0, expected[1024];
        void *code, *mapped; size_t size; int32_t hr;
        stage_split_clip_tokens(system,tokens);
        CHECK(vkdu_shader_dxbc(1,tokens,40,NULL,0,outputs,3,&code,&size));
        /* The native R0 register union has no semantic indexes. Explicitly
         * make this valid API DXBC: second distance is semantic index1. The
         * seam must inspect the full signature before DXBC lowering merges it. */
        words = code; sig = words + words[9]/4 + 2;
        if (sig[0] != 3) exit(1);
        sig[2 + 2*6 + 1] = 1;
        vkd3d_compute_dxbc_checksum(code,size,words+1);
        /* Imported semantic index2 is outside this implementation's 0/1
         * mapping. Verify explicit unsupported/NULL, never silent omission or
         * a claim that every non-0/1 semantic index is a D3D-invalid program. */
        sig[2 + 2*6 + 1] = 2;
        vkd3d_compute_dxbc_checksum(code,size,words+1);
        state.stream_output = &so; pipeline = (vkdu_object *)(uintptr_t)1;
        hr = vkdu_graphics_pipeline_create(device,root,code,size,NULL,0,&state,&pipeline);
        if (hr != (int32_t)0x80004001 || pipeline || vkdu_stream_output_validate(&so,code,size) != hr) {
            fprintf(stderr,"FAIL imported clip/cull index2 unsupported boundary round%u hr=%08x\n",round,(uint32_t)hr); exit(1);
        }
        sig[2 + 2*6 + 1] = 1;
        vkd3d_compute_dxbc_checksum(code,size,words+1);
        state.stream_output = &so; pipeline = (vkdu_object *)(uintptr_t)1;
        hr = vkdu_graphics_pipeline_create(device,root,code,size,NULL,0,&state,&pipeline);
        if (hr == (int32_t)0x80004001 && !pipeline) {
            if (vkdu_stream_output_validate(&so,code,size) != hr) exit(1);
            vkdu_shader_dxbc_free(code); ++rejected; continue;
        }
        CHECK(hr);
        if (executed) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
        }
        for (i = 0; i < 1024; ++i) expected[i] = sentinel;
        expected[512] = 0;
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); memcpy(mapped,expected,sizeof(expected)); CHECK(vkdu_buffer_unmap(upload,0,sizeof(expected)));
        CHECK(vkdu_command_copy(command,output,0,upload,0,sizeof(expected)));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline)); CHECK(vkdu_command_topology(command,1));
        CHECK(vkdu_command_stream_output(command,0,1,&view)); CHECK(vkdu_command_draw(command,1,1,0,0));
        CHECK(vkdu_command_stream_output(command,0,1,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,sizeof(expected))); CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        expected[64] = 0x40700000; expected[65] = 0x40900000; expected[512] = across ? 12 : 8;
        if (across) expected[66] = 0x40200000;
        CHECK(vkdu_buffer_map(readback,0,sizeof(expected),&mapped));
        for (i = 0; i < 1024; ++i) if (((uint32_t *)mapped)[i] != expected[i]) {
            if (mismatch++ < 4) fprintf(stderr,"FAIL split %s semantic%s word%u=%08x expected%08x\n",
                system == 2 ? "clip" : "cull",across ? "1+0" : "1",i,((uint32_t *)mapped)[i],expected[i]);
        }
        failures += !!mismatch; ++executed;
        CHECK(vkdu_buffer_unmap(readback,0,0)); vkdu_object_destroy(pipeline); vkdu_shader_dxbc_free(code);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(root); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    if (failures) { fprintf(stderr,"FAIL split clip/cull: %u actual readback mismatches in%u executed cases\n",failures,executed); exit(1); }
    if (rejected || executed != 4) { fprintf(stderr,"FAIL legal split clip/cull cases still rejected: %u rejected,%u executed\n",rejected,executed); exit(1); }
    printf("PASS split clip/cull semantic0.xy+1.xy: %u explicit unsupported rejections,%u correct GPU cases\n",rejected,executed);
}
