/* Execute exact extracted production bodies with the real private headers.
 * Only Vulkan descriptor encoding/update calls are intercepted. Distinct nonzero
 * encodings make selecting a null payload of the wrong type observable. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include "vkd3d_descriptor_debug.h"
#include <stdio.h>
#include <stdlib.h>

/* Keep the extracted helper independent of the linked production state.o. */
#define vkd3d_bindless_resolve_null_descriptor_type test_resolve_null_descriptor_type
#include "actual-functions.inc"
#undef vkd3d_bindless_resolve_null_descriptor_type

static unsigned checks, updates, copies;
static VkDescriptorType written[8][8];
static const char *case_name;
#define CHECK(c, message) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %s: %s\n", case_name, message); exit(1); } } while (0)

static void VKAPI_PTR capture_descriptor(VkDevice device, const VkDescriptorGetInfoEXT *info,
        size_t size, void *destination)
{
    CHECK(info->type >= VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            info->type <= VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "unexpected encoding type");
    CHECK(!info->data.pSampledImage, "null encoding has a nonzero resource");
    memset(destination, 0x80 + info->type, size);
}

static void VKAPI_PTR capture_update(VkDevice device, uint32_t count,
        const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *vk_copies)
{
    unsigned i;
    ++updates;
    for (i = 0; i < count; ++i)
    {
        unsigned set = (uintptr_t)writes[i].dstSet;
        CHECK(set && set < 8 && writes[i].dstArrayElement < 8, "write destination identity");
        CHECK(writes[i].dstBinding == set + 2 && writes[i].descriptorCount == 1, "write binding/count");
        CHECK(!writes[i].pBufferInfo->buffer && !writes[i].pImageInfo->imageView &&
                !*writes[i].pTexelBufferView, "write is not null");
        written[set][writes[i].dstArrayElement] = writes[i].descriptorType;
    }
    for (i = 0; i < copy_count; ++i)
    {
        unsigned src = (uintptr_t)vk_copies[i].srcSet;
        unsigned dst = (uintptr_t)vk_copies[i].dstSet;
        CHECK(src && src < 8 && dst && dst < 8 && vk_copies[i].descriptorCount == 1, "copy identity");
        CHECK(vk_copies[i].srcBinding == src + 2 && vk_copies[i].dstBinding == dst + 2, "copy binding");
        written[dst][vk_copies[i].dstArrayElement] = written[src][vk_copies[i].srcArrayElement];
        ++copies;
    }
}

static struct d3d12_device *make_device(void)
{
    struct d3d12_device *d = calloc(1, sizeof(*d));
    VkPhysicalDeviceDescriptorBufferPropertiesEXT *p = &d->device_info.descriptor_buffer_properties;
    CHECK(d, "allocate device");
    d->vk_procs.vkGetDescriptorEXT = capture_descriptor;
    d->vk_procs.vkUpdateDescriptorSets = capture_update;
    p->sampledImageDescriptorSize = p->storageImageDescriptorSize = 32;
    p->robustUniformBufferDescriptorSize = p->robustStorageBufferDescriptorSize = 16;
    p->robustUniformTexelBufferDescriptorSize = p->robustStorageTexelBufferDescriptorSize = 16;
    p->samplerDescriptorSize = 16;
    p->descriptorBufferOffsetAlignment = 16;
    d->device_info.properties2.properties.limits.nonCoherentAtomSize = 64;
    d->bindless_state.flags = VKD3D_BINDLESS_MUTABLE_TYPE | VKD3D_BINDLESS_RAW_SSBO |
            VKD3D_BINDLESS_MUTABLE_TYPE_SPLIT_RAW_TYPED;
    d->device_info.mutable_descriptor_features.mutableDescriptorType = true;
    return d;
}

static void add_set(struct d3d12_device *d, unsigned index, unsigned set,
        VkDescriptorType declared, VkDescriptorType initial, unsigned flags, unsigned size)
{
    struct vkd3d_bindless_set_info *s = &d->bindless_state.legacy.set_info[index];
    s->set_index = set;
    s->binding_index = set + 3;
    s->vk_descriptor_type = declared;
    s->vk_init_null_descriptor_type = initial;
    s->flags = flags;
    s->heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    s->host_mapping_descriptor_size = size;
    d->bindless_state.legacy.set_count = index + 1;
}

static void split_sets(struct d3d12_device *d, bool raw_fixed)
{
    /* Index zero belongs to the separate sampler heap in the actual layout. */
    d->bindless_state.legacy.set_info[0].heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    add_set(d, 1, 0, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VKD3D_BINDLESS_SET_MUTABLE_TYPED | VKD3D_BINDLESS_SET_MUTABLE, 32);
    add_set(d, 2, 1, raw_fixed ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            raw_fixed ? VKD3D_BINDLESS_SET_RAW_SSBO :
            VKD3D_BINDLESS_SET_MUTABLE_RAW | VKD3D_BINDLESS_SET_MUTABLE | VKD3D_BINDLESS_SET_RAW_SSBO, 16);
}

static struct d3d12_descriptor_heap *make_heap(struct d3d12_device *d, bool buffer)
{
    struct d3d12_descriptor_heap *h;
    size_t size = sizeof(*h) + 8 * (sizeof(struct vkd3d_descriptor_metadata_types) +
            sizeof(struct vkd3d_descriptor_metadata_view));
    unsigned i;
    CHECK(!posix_memalign((void **)&h, 256, size), "allocate real encoded heap");
    memset(h, 0, size);
    h->device = d;
    h->desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    h->desc.NumDescriptors = 8;
    h->buffer_ranges.host_ptr = calloc(8, sizeof(struct vkd3d_bound_buffer_range));
    h->raw_va_aux_buffer.host_ptr = calloc(8, sizeof(VkDeviceAddress));
    for (i = 1; i < d->bindless_state.legacy.set_count; ++i)
    {
        struct vkd3d_bindless_set_info *s = &d->bindless_state.legacy.set_info[i];
        h->sets[s->set_index].vk_descriptor_set = (VkDescriptorSet)(uintptr_t)(s->set_index + 1);
        /* One full guard cell on each side; actual destination stride is retained. */
        h->sets[s->set_index].mapped_set = calloc(10, s->host_mapping_descriptor_size);
        memset(h->sets[s->set_index].mapped_set, 0x5a, 10 * s->host_mapping_descriptor_size);
        h->sets[s->set_index].mapped_set += s->host_mapping_descriptor_size;
        if (buffer)
            d3d12_descriptor_heap_add_null_descriptor_template_buffers(h, s, i);
        else
            d3d12_descriptor_heap_add_null_descriptor_template_descriptors(h, s, i);
    }
    return h;
}

static vkd3d_cpu_descriptor_va_t heap_va(struct d3d12_descriptor_heap *h, unsigned slot)
{
    return (uintptr_t)h + 3 + slot * VKD3D_RESOURCE_DESC_INCREMENT;
}

static void free_heap(struct d3d12_descriptor_heap *h)
{
    unsigned i;
    for (i = 1; i < h->device->bindless_state.legacy.set_count; ++i)
    {
        struct vkd3d_bindless_set_info *s = &h->device->bindless_state.legacy.set_info[i];
        free(h->sets[s->set_index].mapped_set - s->host_mapping_descriptor_size);
    }
    free(h->buffer_ranges.host_ptr);
    free(h->raw_va_aux_buffer.host_ptr);
    free(h);
}

static void check_payload(const uint8_t *p, size_t size, VkDescriptorType type, const char *diagnostic)
{
    size_t i;
    for (i = 0; i < size; ++i)
        CHECK(p[i] == (i < (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 32u : 16u) ? 0x80 + type : 0), diagnostic);
}

static void check_write(bool buffer, VkDescriptorType request, VkDescriptorType typed,
        VkDescriptorType raw, bool merged, bool raw_fixed)
{
    struct d3d12_device *d = make_device();
    struct d3d12_descriptor_heap *h;
    struct d3d12_desc_split desc;
    unsigned before, i;
    if (raw_fixed)
        d->bindless_state.flags |= VKD3D_BINDLESS_CBV_AS_SSBO;
    split_sets(d, raw_fixed);
    if (merged)
    {
        d->bindless_state.flags &= ~VKD3D_BINDLESS_MUTABLE_TYPE_SPLIT_RAW_TYPED;
        d->bindless_state.flags |= VKD3D_BINDLESS_MUTABLE_TYPE_RAW_SSBO;
        d->bindless_state.legacy.set_count = 2;
        d->bindless_state.legacy.set_info[1].flags |= VKD3D_BINDLESS_SET_MUTABLE_RAW;
    }
    vkd3d_bindless_state_init_null_descriptor_payloads(&d->bindless_state, d);
    h = make_heap(d, buffer);
    desc = d3d12_desc_decode_va(heap_va(h, 2));
    CHECK(desc.heap == h && desc.offset == 2, "real descriptor address decoding");
    desc.view->info.flags = VKD3D_DESCRIPTOR_FLAG_NON_NULL;
    memset(h->buffer_ranges.host_ptr + 2 * sizeof(struct vkd3d_bound_buffer_range), 0xab,
            sizeof(struct vkd3d_bound_buffer_range));
    ((VkDeviceAddress *)h->raw_va_aux_buffer.host_ptr)[2] = 0x123456;
    d3d12_descriptor_heap_write_null_descriptor_template(heap_va(h, 2), request);
    if (buffer)
    {
        check_payload(h->sets[0].mapped_set + 64, 32, typed, "typed payload mismatch");
        if (!merged)
            check_payload(h->sets[1].mapped_set + 32, 16, raw, "raw payload mismatch");
        CHECK(!updates, "descriptor-buffer unexpectedly used vkUpdateDescriptorSets");
        for (i = 1; i < d->bindless_state.legacy.set_count; ++i)
        {
            const struct vkd3d_bindless_set_info *s = &d->bindless_state.legacy.set_info[i];
            const uint8_t *base = h->sets[s->set_index].mapped_set - s->host_mapping_descriptor_size;
            unsigned byte;
            for (byte = 0; byte < 10 * s->host_mapping_descriptor_size; ++byte)
                if (byte < 3 * s->host_mapping_descriptor_size || byte >= 4 * s->host_mapping_descriptor_size)
                    CHECK(base[byte] == 0x5a, "descriptor write touched another slot/guard");
        }
    }
    else
    {
        CHECK(written[1][2] == typed, "typed write selected illegal/wrong active type");
        if (!merged)
            CHECK(written[2][2] == raw, "raw write selected illegal/wrong active type");
        CHECK(updates == 1, "one descriptor batch required");
    }
    CHECK(!((VkDeviceAddress *)h->raw_va_aux_buffer.host_ptr)[2], "null did not clear raw VA");
    for (i = 0; i < sizeof(struct vkd3d_bound_buffer_range); ++i)
        CHECK(!((uint8_t *)h->buffer_ranges.host_ptr)[2 * sizeof(struct vkd3d_bound_buffer_range) + i],
                "null did not clear buffer range");
    CHECK(desc.types->set_info_mask == (merged ? 2u : 6u), "null lost companion set copy mask");
    CHECK(desc.types->current_null_type == request, "null cache lost requested type");
    CHECK(desc.view->info.flags & VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET, "range metadata copy flag lost");
    before = updates;
    d3d12_descriptor_heap_write_null_descriptor_template(heap_va(h, 2), request);
    CHECK(updates == before, "same null type should skip duplicate writes");
    if (!buffer)
    {
        d3d12_desc_copy_single(heap_va(h, 4), heap_va(h, 2), d);
        CHECK(written[1][4] == typed && (merged || written[2][4] == raw), "copy lost null companion set");
        CHECK(copies == (merged ? 1u : 2u), "copy batch missing set");
    }
    free_heap(h);
    free(d);
}

static void check_initial_buffer(void)
{
    struct d3d12_device *d = make_device();
    struct d3d12_descriptor_heap h = {0};
    unsigned i;
    split_sets(d, false);
    vkd3d_bindless_state_init_null_descriptor_payloads(&d->bindless_state, d);
    h.device = d; h.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.desc.NumDescriptors = 8;
    CHECK(d3d12_descriptor_heap_create_descriptor_buffer(&h) == S_OK, "allocate host descriptor buffer");
    for (i = 0; i < 8; ++i)
    {
        check_payload(h.descriptor_buffer.host_allocation + h.descriptor_buffer.offsets[0] + 32*i,
                32, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, "initial typed payload mismatch");
        check_payload(h.descriptor_buffer.host_allocation + h.descriptor_buffer.offsets[1] + 16*i,
                16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "initial raw payload mismatch");
    }
    vkd3d_free_aligned(h.descriptor_buffer.host_allocation);
    free(d);
}

static void check_embedded(void)
{
    struct d3d12_device *d = make_device();
    DECLSPEC_ALIGN(64) uint8_t payload[256];
    static const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    unsigned i;
    d->bindless_state.flags |= VKD3D_BINDLESS_MUTABLE_EMBEDDED;
    d->bindless_state.packed_raw_buffer_offset = 16;
    vkd3d_bindless_state_init_null_descriptor_payloads(&d->bindless_state, d);
    d->bindless_state.cbv_srv_uav_size = 32;
    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        memset(payload, 0x5a, sizeof(payload));
        d3d12_descriptor_heap_write_null_descriptor_template_embedded(d, (uintptr_t)payload, types[i]);
        if (types[i] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        {
            check_payload(payload, 16, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, "embedded CBV typed half");
            check_payload(payload+16, 16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, "embedded CBV raw half");
        }
        else if (types[i] == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || types[i] == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
        {
            check_payload(payload, 16, types[i], "embedded buffer typed half");
            check_payload(payload+16, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "embedded buffer raw half");
        }
        else
            check_payload(payload, 32, types[i], "embedded image payload");
        CHECK(payload[32] == 0x5a, "embedded write exceeded stride");
    }
    free(d);
}

static void check_nonmutable(bool buffer)
{
    static const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    struct d3d12_device *d = make_device();
    struct d3d12_descriptor_heap *h;
    struct d3d12_desc_split desc;
    unsigned i;
    d->bindless_state.flags = 0;
    d->device_info.mutable_descriptor_features.mutableDescriptorType = false;
    d->bindless_state.legacy.set_info[0].heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    for (i = 0; i < ARRAY_SIZE(types); ++i)
        add_set(d, i+1, i, types[i], types[i], 0, i < 2 ? 32 : 16);
    vkd3d_bindless_state_init_null_descriptor_payloads(&d->bindless_state, d);
    h = make_heap(d, buffer);
    desc = d3d12_desc_decode_va(heap_va(h, 2));
    desc.view->info.flags = VKD3D_DESCRIPTOR_FLAG_NON_NULL;
    d3d12_descriptor_heap_write_null_descriptor_template(heap_va(h, 2), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        if (buffer)
        {
            unsigned size = i < 2 ? 32 : 16;
            check_payload(h->sets[i].mapped_set + size * 2, size, types[i], "nonmutable payload changed type");
        }
        else
            CHECK(written[i+1][2] == types[i], "nonmutable descriptor changed type");
    }
    CHECK(desc.types->set_info_mask == 0x7e, "nonmutable companion sets lost");
    CHECK(desc.types->current_null_type == 0, "nonmutable null cache must ignore request type");
    i = updates;
    d3d12_descriptor_heap_write_null_descriptor_template(heap_va(h, 2), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(updates == i, "nonmutable changed null type should still skip duplicate writes");
    free_heap(h); free(d);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2, "expected one case");
    case_name = argv[1];
    if (!strcmp(case_name, "write-image")) check_write(false, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, false);
    else if (!strcmp(case_name, "write-cbv")) check_write(false, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false, false);
    else if (!strcmp(case_name, "write-uav")) check_write(false, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, false);
    else if (!strcmp(case_name, "write-texel")) check_write(false, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, false);
    else if (!strcmp(case_name, "buffer-image")) check_write(true, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, false);
    else if (!strcmp(case_name, "buffer-cbv")) check_write(true, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false, false);
    else if (!strcmp(case_name, "buffer-uav")) check_write(true, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, false);
    else if (!strcmp(case_name, "buffer-initial")) check_initial_buffer();
    else if (!strcmp(case_name, "merged-image")) check_write(false, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0, true, false);
    else if (!strcmp(case_name, "merged-cbv")) check_write(false, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, true, false);
    else if (!strcmp(case_name, "fixed-raw-image")) check_write(false, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, true);
    else if (!strcmp(case_name, "fixed-raw-cbv")) check_write(false, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, false, true);
    else if (!strcmp(case_name, "embedded")) check_embedded();
    else if (!strcmp(case_name, "nonmutable")) check_nonmutable(false);
    else if (!strcmp(case_name, "nonmutable-buffer")) check_nonmutable(true);
    else CHECK(false, "unknown case");
    printf("PASS %s: %u checks, actual production functions and private layouts\n", case_name, checks);
    return 0;
}
