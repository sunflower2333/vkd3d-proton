/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual WDK callbacks -> real backend -> CPU Vulkan -> same queue readback.
 * Internal bridge setup/resources; no backend peers, KMT or SYSTEM claim. */
#include "ddi.h"
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "ddi.cpp"
#include "../../tests/shaders/render_target/headers/vs_flat_color.h"
#include "../../tests/shaders/resource/headers/ps_store_buffer.h"
#include "../../tests/shaders/command/headers/execute_indirect_multi_dispatch_root_constants.h"
#include "../../tests/shaders/descriptors/headers/update_root_descriptors.h"
#define CHECK(e) do{HRESULT h_=(e);if(FAILED(h_)){std::fprintf(stderr,"FAIL integrated line%d %s hr=%08x\n",__LINE__,#e,unsigned(h_));std::exit(1);}}while(0)
#define REJECT(e) do{if(SUCCEEDED(e)){std::fprintf(stderr,"FAIL integrated unexpected success line%d\n",__LINE__);std::exit(1);}}while(0)
struct DdiFixture;
struct DdiItem { Object slot{}; DdiFixture *f; vkdu_object *real(){return slot.backend;} template<class H>H h(){return H{&slot};} };
struct DdiFixture {
 Context *ctx=new Context;D3D12DDI_DEVICE_FUNCS_CORE_0003 d{};D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 c{};D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 q{};HRESULT reported=S_OK;unsigned calls=0;
 D3D12DDI_HDEVICE h(){return {ctx};}DdiItem *item(){return new DdiItem{{},this};}
 HRESULT done(){++calls;auto hr=reported;reported=S_OK;ctx->last_error=S_OK;return hr;}
 static void APIENTRY report(void *p,HRESULT hr){static_cast<DdiFixture*>(p)->reported=hr;}
 HRESULT wrap(vkdu_object *v,vkdu_kind kind,DdiItem **out){auto *p=item();auto hr=VioGpuD3D12BridgeBindObject(h(),&p->slot,v,kind);if(FAILED(hr)){delete p;vkdu_object_destroy(v);}else *out=p;return hr;}
};
static const uint32_t *graphics_tokens(const void *data){auto *v=static_cast<const uint32_t*>(data);for(UINT i=0;i<v[7];++i){auto *c=v+v[8+i]/4;if(c[0]==0x52444853||c[0]==0x58454853)return c+2;}std::exit(1);}
static HRESULT ddii_root_create(DdiFixture *f,const vkdu_root_parameter *p,uint32_t n,uint32_t flags,DdiItem **out){
 std::vector<D3D12DDI_ROOT_PARAMETER> params(n);std::vector<std::vector<D3D12DDI_DESCRIPTOR_RANGE>> ranges(n);
 for(UINT i=0;i<n;++i){auto &d=params[i];d.ParameterType=static_cast<D3D12DDI_ROOT_PARAMETER_TYPE>(p[i].type);d.ShaderVisibility=static_cast<D3D12DDI_SHADER_VISIBILITY>(p[i].visibility);
 if(p[i].type==0){for(UINT j=0;j<p[i].range_count;++j){auto &r=p[i].ranges[j];ranges[i].push_back({static_cast<D3D12DDI_DESCRIPTOR_RANGE_TYPE>(r.type),r.count,r.shader_register,r.register_space,r.offset});}d.DescriptorTable={UINT(ranges[i].size()),ranges[i].data()};}
 else if(p[i].type==1)d.Constants={p[i].shader_register,p[i].register_space,p[i].constant_count};else d.Descriptor={p[i].shader_register,p[i].register_space};}
 D3D12DDI_ROOT_SIGNATURE desc{};desc.NumParameters=n;desc.pRootParameters=params.data();desc.Flags=static_cast<D3D12DDI_ROOT_SIGNATURE_FLAGS>(flags);D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args{&desc,1};auto *v=f->item();++f->calls;auto hr=f->d.pfnCreateRootSignature(f->h(),&args,v->h<D3D12DDI_HROOTSIGNATURE>());if(FAILED(hr))delete v;else *out=v;return hr;
}
static HRESULT ddii_heap_create(DdiFixture *f,uint32_t type,uint32_t n,int visible,DdiItem **out){D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 a{};a.Type=static_cast<D3D12DDI_DESCRIPTOR_HEAP_TYPE>(type);a.NumDescriptors=n;a.NodeMask=1;a.Flags=static_cast<D3D12DDI_DESCRIPTOR_HEAP_FLAGS>(1|(visible?2:0));auto *v=f->item();++f->calls;auto hr=f->d.pfnCreateDescriptorHeap(f->h(),&a,v->h<D3D12DDI_HDESCRIPTORHEAP>());if(FAILED(hr))delete v;else *out=v;return hr;}
static HRESULT ddii_allocator_create(DdiFixture *f,uint32_t type,DdiItem **out){auto *v=f->item();D3D12DDIARG_CREATECOMMANDALLOCATOR a{};a.hDrvCommandAllocator=v->h<D3D12DDI_HCOMMANDALLOCATOR>();a.Type=type?D3D12DDI_COMMAND_LIST_TYPE_BUNDLE:D3D12DDI_COMMAND_LIST_TYPE_DIRECT;a.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;++f->calls;auto hr=f->d.pfnCreateCommandAllocator(f->h(),&a);if(FAILED(hr))delete v;else *out=v;return hr;}
static HRESULT ddii_command_create(DdiFixture *f,DdiItem *allocator,uint32_t type,DdiItem **out){auto *v=f->item();D3D12DDIARG_CREATE_COMMAND_LIST_0001 a{};a.hDrvCommandList=v->h<D3D12DDI_HCOMMANDLIST>();a.hDrvCommandAllocator=allocator->h<D3D12DDI_HCOMMANDALLOCATOR>();a.Type=type?D3D12DDI_COMMAND_LIST_TYPE_BUNDLE:D3D12DDI_COMMAND_LIST_TYPE_DIRECT;a.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;a.NodeMask=1;++f->calls;auto hr=f->d.pfnCreateCommandList(f->h(),&a);if(FAILED(hr))delete v;else *out=v;return hr;}
static HRESULT ddii_queue_create(DdiFixture *f,uint32_t,DdiItem **out){auto *v=f->item();D3D12DDIARG_CREATECOMMANDQUEUE_0001 a{};a.hDrvCommandQueue=v->h<D3D12DDI_HCOMMANDQUEUE>();a.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;a.NodeMask=1;++f->calls;auto hr=f->d.pfnCreateCommandQueue(f->h(),&a);if(FAILED(hr))delete v;else *out=v;return hr;}
/* These real objects are only setup/observation: native allocation is untested. */
static HRESULT ddii_buffer_create(DdiFixture *f,uint64_t n,uint32_t heap,uint32_t flags,uint32_t state,DdiItem **out){vkdu_object *v=nullptr;auto hr=vkdu_buffer_create(f->ctx->backend,n,heap,flags,state,&v);return FAILED(hr)?hr:f->wrap(v,VKDU_BUFFER,out);}
static HRESULT ddii_render_target_create(DdiFixture *f,uint32_t w,uint32_t h,DdiItem **out){vkdu_object *v=nullptr;auto hr=vkdu_render_target_create(f->ctx->backend,w,h,&v);return FAILED(hr)?hr:f->wrap(v,VKDU_TEXTURE2D,out);}
static HRESULT ddii_pipeline_create(DdiFixture *f,DdiItem *r,const void *code,size_t bytes,DdiItem **out){vkdu_object *v=nullptr;auto hr=vkdu_pipeline_create(f->ctx->backend,r->real(),code,bytes,&v);return FAILED(hr)?hr:f->wrap(v,VKDU_PIPELINE,out);}
static HRESULT ddii_graphics_pipeline_create(DdiFixture *f,DdiItem *r,const void *vs,size_t vn,const void *ps,size_t pn,const vkdu_graphics_desc *d,DdiItem **out){vkdu_object *v=nullptr;auto hr=vkdu_graphics_pipeline_create(f->ctx->backend,r->real(),vs,vn,ps,pn,d,&v);return FAILED(hr)?hr:f->wrap(v,VKDU_PIPELINE,out);}
static HRESULT ddii_fence_create(DdiFixture *f,uint64_t n,DdiItem **out){vkdu_object *v=nullptr;auto hr=vkdu_fence_create(f->ctx->backend,n,&v);return FAILED(hr)?hr:f->wrap(v,VKDU_FENCE,out);}
static HRESULT ddii_buffer_map(DdiItem *p,uint64_t b,uint64_t e,void **out){return vkdu_buffer_map(p->real(),b,e,out);}
static HRESULT ddii_buffer_unmap(DdiItem *p,uint64_t b,uint64_t e){return vkdu_buffer_unmap(p->real(),b,e);}
static HRESULT ddii_queue_signal(DdiItem *q,DdiItem *f,uint64_t v){return vkdu_queue_signal(q->real(),f->real(),v);}
static HRESULT ddii_fence_wait(DdiItem *f,uint64_t v,uint32_t ms){return vkdu_fence_wait(f->real(),v,ms);}
static HRESULT ddii_device_status(DdiFixture *f){return vkdu_device_status(f->ctx->backend);}
static void ddii_object_destroy(DdiItem *p){VioGpuD3D12BridgeUnbindObject(&p->slot);delete p;}
#define ddii_shader_dxbc vkdu_shader_dxbc
#define ddii_shader_dxbc_free vkdu_shader_dxbc_free
static HRESULT ddii_texture2d_rtv(DdiItem *heap,uint32_t i,DdiItem *t){return vkdu_texture2d_rtv(heap->real(),i,t->real());}
static HRESULT ddii_buffer_cbv(DdiItem *heap,uint32_t i,DdiItem *b,uint64_t off,uint32_t bytes){auto *f=heap->f;f->calls+=2;auto h=f->d.pfnGetCPUDescriptorHandleForHeapStart(f->h(),heap->h<D3D12DDI_HDESCRIPTORHEAP>());h.ptr+=i*f->d.pfnGetDescriptorSizeInBytes(f->h(),D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);D3D12DDI_CONSTANT_BUFFER_VIEW_DESC desc{b->slot.address+off,bytes};f->d.pfnCreateConstantBufferView(f->h(),&desc,h);return f->done();}
static HRESULT ddii_allocator_reset(DdiItem *p){p->f->d.pfnResetCommandAllocator(p->h<D3D12DDI_HCOMMANDALLOCATOR>());return p->f->done();}
static HRESULT ddii_command_reset(DdiItem *p,DdiItem *a){D3D12DDIARG_RESETCOMMANDLIST args{};args.hDrvCommandAllocator=a->h<D3D12DDI_HCOMMANDALLOCATOR>();p->f->c.pfnResetCommandList(p->h<D3D12DDI_HCOMMANDLIST>(),&args);return p->f->done();}
#define CMD0(name,field) static HRESULT name(DdiItem *p){p->f->c.field(p->h<D3D12DDI_HCOMMANDLIST>());return p->f->done();}
CMD0(ddii_command_close,pfnCloseCommandList)
CMD0(ddii_command_clear_root_arguments,pfnClearRootArguments)
#define CMDH(name,field,H) static HRESULT name(DdiItem *p,DdiItem *v){p->f->c.field(p->h<D3D12DDI_HCOMMANDLIST>(),v->h<H>());return p->f->done();}
CMDH(ddii_command_execute_bundle,pfnExecuteBundle,D3D12DDI_HCOMMANDLIST)
CMDH(ddii_command_graphics_root,pfnSetGraphicsRootSignature,D3D12DDI_HROOTSIGNATURE)
CMDH(ddii_command_root,pfnSetComputeRootSignature,D3D12DDI_HROOTSIGNATURE)
CMDH(ddii_command_pipeline,pfnSetPipelineState,D3D12DDI_HPIPELINESTATE)
#define CONSTANTS(name,field) static HRESULT name(DdiItem *p,uint32_t i,uint32_t off,uint32_t n,const uint32_t *v){p->f->c.field(p->h<D3D12DDI_HCOMMANDLIST>(),i,n,v,off);return p->f->done();}
CONSTANTS(ddii_command_graphics_constants,pfnSetGraphicsRoot32BitConstants)
CONSTANTS(ddii_command_constants,pfnSetComputeRoot32BitConstants)
#define UAV(name,field) static HRESULT name(DdiItem *p,uint32_t i,DdiItem *v,uint64_t off){p->f->c.field(p->h<D3D12DDI_HCOMMANDLIST>(),i,v->slot.address+off);return p->f->done();}
UAV(ddii_command_graphics_uav,pfnSetGraphicsRootUnorderedAccessView)
UAV(ddii_command_uav,pfnSetComputeRootUnorderedAccessView)
static HRESULT ddii_command_draw(DdiItem *p,UINT a,UINT b,UINT c,UINT d){p->f->c.pfnDrawInstanced(p->h<D3D12DDI_HCOMMANDLIST>(),a,b,c,d);return p->f->done();}
static HRESULT ddii_command_dispatch(DdiItem *p,UINT a,UINT b,UINT c){p->f->c.pfnDispatch(p->h<D3D12DDI_HCOMMANDLIST>(),a,b,c);return p->f->done();}
static HRESULT ddii_command_topology(DdiItem *p,uint32_t v){p->f->c.pfnIaSetTopology(p->h<D3D12DDI_HCOMMANDLIST>(),static_cast<D3D12DDI_PRIMITIVE_TOPOLOGY>(v));return p->f->done();}
static HRESULT ddii_command_viewports(DdiItem *p,uint32_t n,const vkdu_viewport *v){D3D12DDI_VIEWPORT out[16]{};for(UINT i=0;i<n;++i)out[i]={v[i].x,v[i].y,v[i].width,v[i].height,v[i].min_depth,v[i].max_depth};p->f->c.pfnRsSetViewports(p->h<D3D12DDI_HCOMMANDLIST>(),n,out);return p->f->done();}
static HRESULT ddii_command_scissors(DdiItem *p,uint32_t n,const vkdu_rect *v){D3D12DDI_RECT out[16]{};for(UINT i=0;i<n;++i)out[i]={v[i].left,v[i].top,v[i].right,v[i].bottom};p->f->c.pfnRsSetScissorRects(p->h<D3D12DDI_HCOMMANDLIST>(),n,out);return p->f->done();}
static HRESULT ddii_command_render_target(DdiItem *p,DdiItem *heap,uint32_t i){auto *f=p->f;f->calls+=2;auto h=f->d.pfnGetCPUDescriptorHandleForHeapStart(f->h(),heap->h<D3D12DDI_HDESCRIPTORHEAP>());h.ptr+=i*f->d.pfnGetDescriptorSizeInBytes(f->h(),D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);f->c.pfnOMSetRenderTargets(p->h<D3D12DDI_HCOMMANDLIST>(),1,&h,FALSE,nullptr);return f->done();}
static HRESULT ddii_command_heaps(DdiItem *p,uint32_t n,DdiItem *const *v){D3D12DDI_HDESCRIPTORHEAP h[2]{};for(UINT i=0;i<n;++i)h[i]=v[i]->h<D3D12DDI_HDESCRIPTORHEAP>();p->f->c.pfnSetDescriptorHeaps(p->h<D3D12DDI_HCOMMANDLIST>(),n,h);return p->f->done();}
static HRESULT ddii_table(DdiItem *p,uint32_t i,DdiItem *heap,uint32_t first,bool graphics){auto *f=p->f;f->calls+=2;auto h=f->d.pfnGetGPUDescriptorHandleForHeapStart(f->h(),heap->h<D3D12DDI_HDESCRIPTORHEAP>());h.ptr+=first*f->d.pfnGetDescriptorSizeInBytes(f->h(),D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);if(graphics)f->c.pfnSetGraphicsRootDescriptorTable(p->h<D3D12DDI_HCOMMANDLIST>(),i,h);else f->c.pfnSetComputeRootDescriptorTable(p->h<D3D12DDI_HCOMMANDLIST>(),i,h);return f->done();}
static HRESULT ddii_command_graphics_table(DdiItem *p,uint32_t i,DdiItem *heap,uint32_t first){return ddii_table(p,i,heap,first,true);}
static HRESULT ddii_command_table(DdiItem *p,uint32_t i,DdiItem *heap,uint32_t first){return ddii_table(p,i,heap,first,false);}
static D3D12DDIARG_BUFFER_PLACEMENT placement(DdiItem *p,uint64_t off){D3D12DDIARG_BUFFER_PLACEMENT b{};b.BaseAddress.UMD.hResource=p->h<D3D12DDI_HRESOURCE>();b.BaseAddress.UMD.Offset=off;return b;}
static HRESULT ddii_command_copy(DdiItem *p,DdiItem *dst,uint64_t off,DdiItem *src,uint64_t soff,uint64_t n){p->f->c.pfnCopyBufferRegion(p->h<D3D12DDI_HCOMMANDLIST>(),placement(dst,off),placement(src,soff),n);return p->f->done();}
struct DdiBarrier {uint32_t type;DdiItem *resource;uint32_t before,after;};
static HRESULT ddii_command_barriers(DdiItem *p,uint32_t n,const DdiBarrier *v){D3D12DDIARG_RESOURCE_BARRIER_0003 out[16]{};for(UINT i=0;i<n;++i){out[i].Type=D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;out[i].UAV.hResource=v[i].resource->h<D3D12DDI_HRESOURCE>();}p->f->c.pfnResourceBarrier(p->h<D3D12DDI_HCOMMANDLIST>(),n,out);return p->f->done();}
static HRESULT ddii_command_transition(DdiItem *p,DdiItem *resource,uint32_t before,uint32_t after){D3D12DDIARG_RESOURCE_BARRIER_0003 b{};b.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.hResource=resource->h<D3D12DDI_HRESOURCE>();b.Transition.Subresource=D3D12DDI_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=static_cast<D3D12DDI_RESOURCE_STATES>(before);b.Transition.StateAfter=static_cast<D3D12DDI_RESOURCE_STATES>(after);p->f->c.pfnResourceBarrier(p->h<D3D12DDI_HCOMMANDLIST>(),1,&b);return p->f->done();}
static HRESULT ddii_queue_execute(DdiItem *p,uint32_t n,DdiItem *const *v){D3D12DDI_HCOMMANDLIST h[64]{};for(UINT i=0;i<n;++i)h[i]=v[i]->h<D3D12DDI_HCOMMANDLIST>();p->f->q.pfnExecuteCommandLists(p->h<D3D12DDI_HCOMMANDQUEUE>(),n,h);return p->f->done();}
#include "ddi_bundle_root_data_body.inc"
int main(){
 auto module=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module){std::fprintf(stderr,"FAIL Vulkan loader %lu\n",GetLastError());return 1;}
 auto loader=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr"));DdiFixture f;CHECK(vkdu_test_device_create(loader,&f.ctx->backend));f.ctx->report=DdiFixture::report;f.ctx->report_context=&f;CHECK(VioGpuD3D12BridgeGetTables(&f.d,&f.c,&f.q));
 if(native_contract_complete()){std::fputs("FAIL public gate open\n",stderr);return 1;}
 check_ddi_bundle_root_data(&f);
 if(f.ctx->references!=1||f.ctx->resources||f.ctx->descriptor_heaps){std::fputs("FAIL integrated cleanup\n",stderr);return 1;}
 CHECK(vkdu_device_status(f.ctx->backend));VioGpuD3D12BridgeDestroy(f.h());FreeLibrary(module);std::printf("PASS actual typed DDI to CPU Vulkan root data: same callbacks/backend/queue; %u DDI calls; public0; internal bridge setup, no KMT/SYSTEM/hardware\n",f.calls);return 0;
}
