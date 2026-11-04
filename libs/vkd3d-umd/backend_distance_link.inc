/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* R0 links system values by register/component, while DXBC's Clip/Cull lowering
 * packs each semantic independently. Give each consumer the producer's complete
 * distance signature before lowering, including unused rows/components. This
 * both assigns the correct semantic indices and keeps scalar-array positions
 * identical. The strict embedded interface validator remains unchanged. */
static int vkdu_is_distance(uint32_t semantic)
{
    return semantic == VKD3D_SV_CLIP_DISTANCE || semantic == VKD3D_SV_CULL_DISTANCE;
}
/* The upstream signature reader's initial size check assumes six-word rows.
 * Check the actual ISG1/OSG1/OSG5 row width before asking it to parse a container. */
static int vkdu_distance_container_valid(const void *code, size_t size)
{
    const uint32_t *words=code;uint32_t i,header,seen=0;
    if (!code || (uintptr_t)code%4 || size<32 || size%4 || size>UINT32_MAX ||
            words[0]!=0x43425844 || words[6]!=size || words[7]>(size-32)/4) return 0;
    header=32+4*words[7];
    for (i=0;i<words[7];++i) {
        uint32_t at=words[8+i],bytes,tag,width=0,kind=0;
        if (at%4 || at<header || at>size-8 || (bytes=words[at/4+1])>size-at-8) return 0;
        tag=words[at/4];
        if (tag==0x4e475349 || tag==0x31475349) {kind=1;width=tag==0x31475349 ? 8 : 6;}
        if (tag==0x4e47534f || tag==0x3147534f || tag==0x3547534f) {
            kind=2;width=tag==0x3147534f ? 8 : tag==0x3547534f ? 7 : 6;
        }
        if (kind) {
            if ((seen&kind) || bytes<8 || words[at/4+2]>(bytes-8)/(4*width)) return 0;
            seen|=kind;
        }
    }
    return 1;
}
int32_t vkdu_shader_link_distances(const void *producer, size_t producer_size,
        const void *consumer, size_t consumer_size, uint32_t stream, void **code, size_t *size)
{
    struct vkd3d_shader_code source = {producer,producer_size}, target = {consumer,consumer_size};
    struct vkd3d_shader_signature inputs = {0}, outputs = {0};
    struct vkd3d_shader_signature_element entries[32];
    uint32_t i,j,k,count=0,kinds=0,matched[32]={0},distance_regs=0,distance_count=0,component_bits[2]={0};
    const uint32_t *old=consumer;
    uint32_t *copy=NULL,old_input=UINT32_MAX,chunk_count,chunk_bytes=8,bytes,offset,header_bytes;
    HRESULT hr=E_INVALIDARG;
    int rc;
    if (!code || !size) return E_POINTER;
    *code=NULL;*size=0;
    if (stream>=4 || !vkdu_distance_container_valid(producer,producer_size) ||
            !vkdu_distance_container_valid(consumer,consumer_size)) return E_INVALIDARG;
    if ((rc=vkd3d_shader_parse_input_signature(&target,&inputs))<0) {
        if (rc==VKD3D_ERROR_OUT_OF_MEMORY) hr=E_OUTOFMEMORY;
        goto done;
    }
    if (inputs.element_count>32) {hr=E_NOTIMPL;goto done;}
    for (i=0;i<inputs.element_count;++i) {
        const struct vkd3d_shader_signature_element *in=&inputs.elements[i];
        if (vkdu_is_distance(in->sysval_semantic)) kinds|=1u<<(in->sysval_semantic==VKD3D_SV_CULL_DISTANCE);
        else entries[count++]=*in;
    }
    if (!kinds) {hr=S_OK;goto done;}
    if ((rc=vkd3d_shader_parse_output_signature(&source,&outputs))<0) {
        if (rc==VKD3D_ERROR_OUT_OF_MEMORY) hr=E_OUTOFMEMORY;
        goto done;
    }
    if (outputs.element_count>128) {hr=E_NOTIMPL;goto done;}
    for (i=0;i<outputs.element_count;++i) {
        const struct vkd3d_shader_signature_element *out=&outputs.elements[i];
        uint32_t mask=out->mask&15,kind=out->sysval_semantic==VKD3D_SV_CULL_DISTANCE,used=0,regs=0,bits;
        if (out->stream_index!=stream || !vkdu_is_distance(out->sysval_semantic)) continue;
        if (!mask || out->register_index>=32 || out->component_type!=VKD3D_TYPE_FLOAT) goto done;
        if (out->semantic_index>=2) {hr=E_NOTIMPL;goto done;}
        bits=mask<<(4*out->semantic_index);
        if (component_bits[kind]&bits) goto done;
        component_bits[kind]|=bits;distance_regs|=1u<<out->register_index;
        for (bits=distance_regs;bits;bits>>=1)regs+=bits&1;
        for (bits=mask;bits;bits>>=1)distance_count+=bits&1;
        if (regs>2 || distance_count>8) goto done;
        if (!(kinds&(1u<<kind))) continue;
        for (j=0;j<inputs.element_count;++j) {
            const struct vkd3d_shader_signature_element *in=&inputs.elements[j];
            if (in->register_index!=out->register_index || in->sysval_semantic!=out->sysval_semantic) continue;
            if (in->stream_index || in->component_type!=out->component_type || in->min_precision!=out->min_precision ||
                    !(in->mask&15) || (in->mask&~out->mask&15) || matched[j]) goto done;
            matched[j]=1;used|=(in->mask>>8)&15;
        }
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_DISTANCE_DROP_PACKING)
        /* Deliberately omit unused rows while still assigning matching
         * semantic indices. This recreates real scalar-array misrouting. */
        if (!used) continue;
#endif
        if (count==32) {hr=E_NOTIMPL;goto done;}
        entries[count]=*out;entries[count].stream_index=0;
        entries[count++].mask=mask|(used<<8);
    }
    for (i=0;i<inputs.element_count;++i)
        if (vkdu_is_distance(inputs.elements[i].sysval_semantic) && !matched[i]) goto done;
    chunk_count=old[7];header_bytes=32+4*chunk_count;
    for (i=0;i<chunk_count;++i) {
        uint32_t at=old[8+i];
        if (at%4 || at<header_bytes || at>consumer_size-8 || old[at/4+1]>consumer_size-at-8) goto done;
        if (old[at/4]==0x4e475349 || old[at/4]==0x31475349) {
            if (old_input!=UINT32_MAX) goto done;
            old_input=i;
        }
    }
    if (old_input==UINT32_MAX) goto done;
    for (i=0;i<count;++i) {
        size_t n=strlen(entries[i].semantic_name)+1;
        if (n>UINT32_MAX-chunk_bytes-32) goto done;
        chunk_bytes+=32+(uint32_t)n;
    }
    if (chunk_bytes>UINT32_MAX-3) goto done;
    chunk_bytes=(chunk_bytes+3)&~3u;
    if (consumer_size>UINT32_MAX-8-chunk_bytes) goto done;
    bytes=header_bytes;
    for (i=0;i<chunk_count;++i) {
        uint32_t n=i==old_input ? chunk_bytes : old[old[8+i]/4+1];
        if (n>UINT32_MAX-3) goto done;
        n=(n+3)&~3u;
        if (bytes>UINT32_MAX-8 || n>UINT32_MAX-bytes-8) goto done;
        bytes+=8+n;
    }
    if (!(copy=calloc(bytes,1))) {hr=E_OUTOFMEMORY;goto done;}
    memcpy(copy,old,header_bytes);copy[6]=bytes;offset=header_bytes;
    for (i=0;i<chunk_count;++i) {
        copy[8+i]=offset;
        if (i==old_input) {
            uint32_t *sig=copy+offset/4+2;char *names=(char *)(sig+2+8*count);
            copy[offset/4]=0x31475349;copy[offset/4+1]=chunk_bytes;sig[0]=count;sig[1]=8;
            for (j=0;j<count;++j) {
                const struct vkd3d_shader_signature_element *e=&entries[j];uint32_t *dst=sig+2+8*j;
                dst[0]=e->stream_index;dst[1]=(uint32_t)(names-(char *)sig);dst[2]=e->semantic_index;
                dst[3]=e->sysval_semantic;dst[4]=e->component_type;dst[5]=e->register_index;
                dst[6]=e->mask;dst[7]=e->min_precision;
                k=(uint32_t)strlen(e->semantic_name)+1;memcpy(names,e->semantic_name,k);names+=k;
            }
            offset+=8+chunk_bytes;
        } else {
            uint32_t n=8+old[old[8+i]/4+1];memcpy((char *)copy+offset,(const char *)old+old[8+i],n);offset+=(n+3)&~3u;
        }
    }
    vkd3d_compute_dxbc_checksum(copy,bytes,copy+1);
    *code=copy;*size=bytes;copy=NULL;hr=S_OK;
 done:
    free(copy);vkd3d_shader_free_shader_signature(&inputs);vkd3d_shader_free_shader_signature(&outputs);return hr;
}
