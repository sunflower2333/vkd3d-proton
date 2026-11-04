/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Descriptors hold weak lifetime tokens, never resource references. The lock
 * protects the live pointer against a final wrapper release. No engine or COM
 * call, including Release, is permitted while this mutex is held. Descriptor
 * updates themselves retain the API's external synchronization requirement. */
static pthread_mutex_t vkdu_attachment_mutex = PTHREAD_MUTEX_INITIALIZER;
static void vkdu_attachment_drop_locked(struct vkdu_attachment_lifetime *lifetime)
{
    if (lifetime && !--lifetime->references) free(lifetime);
}
static void vkdu_attachment_retire(vkdu_object *object)
{
    uint32_t i;
    if (!object->attachment_lifetime && !object->attachment_records) return;
    pthread_mutex_lock(&vkdu_attachment_mutex);
    if (object->attachment_lifetime) {
        object->attachment_lifetime->live = NULL;
        vkdu_attachment_drop_locked(object->attachment_lifetime);
    }
    if (object->attachment_records)
        for (i = 0; i < object->descriptor_count; ++i)
            vkdu_attachment_drop_locked(object->attachment_records[i].lifetime);
    pthread_mutex_unlock(&vkdu_attachment_mutex);
    free(object->attachment_records);
}
static void vkdu_attachment_store(vkdu_object *heap, uint32_t index, vkdu_object *texture,
        const D3D12_RESOURCE_DESC *resource, uint32_t format, uint32_t mip,
        uint32_t first_layer, uint32_t layers, HRESULT status)
{
    struct vkdu_attachment_record record = {0};
    if (SUCCEEDED(status)) {
        record.lifetime = texture->attachment_lifetime;
        record.width = (uint32_t)(resource->Width >> mip);
        record.height = resource->Height >> mip;
        if (!record.width) record.width = 1;
        if (!record.height) record.height = 1;
        record.layers = layers; record.format = format;
        record.mip = mip; record.first_layer = first_layer;
        record.sample_count = resource->SampleDesc.Count; record.sample_quality = resource->SampleDesc.Quality;
    }
    pthread_mutex_lock(&vkdu_attachment_mutex);
    if (record.lifetime) ++record.lifetime->references;
    vkdu_attachment_drop_locked(heap->attachment_records[index].lifetime);
    heap->attachment_records[index] = record;
    pthread_mutex_unlock(&vkdu_attachment_mutex);
}
static void vkdu_attachment_copy(vkdu_object *dst, uint32_t di, vkdu_object *src,
        uint32_t si, uint32_t count, int valid)
{
    uint32_t i;
    if (!dst->attachment_records) return;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MRT_DROP_METADATA_COPY)
    return;
#endif
    pthread_mutex_lock(&vkdu_attachment_mutex);
    for (i = 0; i < count; ++i) {
        struct vkdu_attachment_record record = {0};
        if (valid) record = src->attachment_records[si + i];
        if (record.lifetime) ++record.lifetime->references;
        vkdu_attachment_drop_locked(dst->attachment_records[di + i].lifetime);
        dst->attachment_records[di + i] = record;
    }
    pthread_mutex_unlock(&vkdu_attachment_mutex);
}
static vkdu_object *vkdu_attachment_acquire(vkdu_object *heap, uint32_t index,
        struct vkdu_attachment_record *record)
{
    vkdu_object *resource = NULL;
    pthread_mutex_lock(&vkdu_attachment_mutex);
    *record = heap->attachment_records[index];
    if (record->lifetime && record->lifetime->live &&
            vkdu_object_retain(record->lifetime->live)) resource = record->lifetime->live;
    pthread_mutex_unlock(&vkdu_attachment_mutex);
    return resource;
}
static int vkdu_attachment_dimensions_equal(const struct vkdu_attachment_record *a,
        const struct vkdu_attachment_record *b)
{
    return a->width == b->width && a->height == b->height && a->layers == b->layers &&
        a->sample_count == b->sample_count && a->sample_quality == b->sample_quality;
}
