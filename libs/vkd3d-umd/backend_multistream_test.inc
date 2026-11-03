/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "stage_stream_test_tokens.h"
static void check_multistream_output(vkdu_device *device)
{
    vkdu_object *root, *pipeline, *queue, *allocator, *command, *fence, *vertices, *upload, *output, *readback;
    const struct vkdu_signature vin[] = {{0,0,15,3},{0,1,15,3}}, link[] = {{1,0,15,3},{0,1,15,3}};
    struct vkdu_signature gout = {0,2,15,3};
    const uint32_t slots[4] = {2,0,3,1}, capacity[4] = {3,4,3,5};
    struct vkdu_input_element inputs[] = {{0,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,0},
        {0,16,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,1}};
    struct vkdu_graphics_desc state = {0,UINT32_MAX,3,1,0,1,15,2,inputs};
    const uint32_t *vt = graphics_tokens(unaligned_vertex_stride_vs_code_dxbc);
    uint32_t gt[512], gn = stage_multistream_tokens(gt), round, i, s;
    void *vs, *gs[3], *mapped; size_t vs_size, gs_size[3];
    CHECK(vkdu_shader_dxbc(1,vt,vt[1],vin,2,link,2,&vs,&vs_size));
    CHECK(vkdu_shader_dxbc(2,gt,gn,NULL,0,&gout,1,&gs[0],&gs_size[0]));
    for(uint32_t type=1;type<=2;++type) {
        uint32_t emitted=0;
        for(uint32_t pos=2;pos<gn;pos+=(gt[pos]>>24)&127)
            if((gt[pos]&0x7ff)==54) {
                for(uint32_t c=0;c<4;++c)gt[pos+4+c]=0x81234000u+emitted*4+c;
                ++emitted;
            }
        gout.component_type=type;
        CHECK(vkdu_shader_dxbc(2,gt,gn,NULL,0,&gout,1,&gs[type],&gs_size[type]));
    }
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&vertices));
    CHECK(vkdu_buffer_create(device,16384,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,16384,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,16384,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_map(vertices,0,0,&mapped)); memset(mapped,0,4096); CHECK(vkdu_buffer_unmap(vertices,0,4096));
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; state.gs = gs[0]; state.gs_size = gs_size[0];
    for (round = 0; round < 7; ++round) {
        const uint32_t type=round>=5 ? round-4 : 0, partial=round==3 || round>=5;
        state.gs=gs[type];state.gs_size=gs_size[type];
        struct vkdu_stream_output_desc so = {0,4,UINT32_MAX,{{0}},{0}};
        struct vkdu_stream_output_buffer views[4];
        struct vkdu_vertex_buffer vb = {vertices,64,32,32};
        uint32_t expected[4096], draws = round < 3 ? round + 1 : 2;
        const uint32_t stride = round == 4 ? 512 : 16, sentinel = 0x7e610000 + round;
        for (i = 0; i < 4096; ++i) expected[i] = sentinel;
        for (s = 0; s < 4; ++s) {
            const uint32_t slot = slots[s], count = round == 4 ? 2 : capacity[s];
            const uint32_t offset = 256 + slot * 2048, counter = 15360 + slot * 4;
            so.strides[slot] = stride;
            if (round == 4) {
                for (i = 0; i < 127; ++i) so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,UINT32_MAX,1};
                so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,2,8};
            } else if (partial) {
                so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,2,8};
                so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,UINT32_MAX,1};
                so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,2,1};
            } else so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,slot,2,15};
            views[slot] = (struct vkdu_stream_output_buffer){output,output,offset,count * stride,counter};
            expected[counter / 4] = 0;
        }
        CHECK(vkdu_stream_output_validate(&so,gs[type],gs_size[type])); state.stream_output = &so;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,NULL,0,&state,&pipeline));
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); memcpy(mapped,expected,sizeof(expected));
        CHECK(vkdu_buffer_unmap(upload,0,sizeof(expected)));
        CHECK(vkdu_command_copy(command,output,0,upload,0,sizeof(expected)));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_vertex_buffers(command,0,1,&vb)); CHECK(vkdu_command_topology(command,1));
        for (i = 0; i < draws; ++i) {
            CHECK(vkdu_command_stream_output(command,0,4,views)); CHECK(vkdu_command_draw(command,1,1,0,0));
        }
        CHECK(vkdu_command_stream_output(command,0,4,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,sizeof(expected))); CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,round+1));
        CHECK(vkdu_fence_wait(fence,round+1,10000));
        for (s = 0; s < 4; ++s) {
            uint32_t v, c, written = draws * (s + 1), count = round == 4 ? 2 : capacity[s], slot = slots[s];
            if (written > count) written = count;
            expected[3840 + slot] = written * stride;
            for (v = 0; v < written; ++v) for (c = 0; c < 4; ++c) {
                uint32_t word = 64 + slot * 512 + v * stride / 4 + c;
                float value = (float)(16 * s + 4 * (v % (s + 1)) + c + 1);
                if (round == 4) { if (c != 3) continue; word += 124; }
                else if (partial) { if (c != 0 && c != 3) continue; word += c ? -3 : 2; }
                if(type)expected[word]=0x81234000u+(s*(s+1)/2+v%(s+1))*4+c;
                else memcpy(&expected[word],&value,4);
            }
        }
        CHECK(vkdu_buffer_map(readback,0,sizeof(expected),&mapped));
        for (i = 0; i < 4096; ++i) if (((uint32_t *)mapped)[i] != expected[i]) {
            fprintf(stderr,"FAIL multistream round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback,0,0)); vkdu_object_destroy(pipeline);
        printf("PASS actual four-stream SO round%u:4096 exact words, independent emit rates/counters,permuted slots,append/overflow,partial masks,512-entry limit\n",round);
    }
    vkdu_shader_dxbc_free(vs);for(i=0;i<3;++i)vkdu_shader_dxbc_free(gs[i]);
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(root); vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
}
