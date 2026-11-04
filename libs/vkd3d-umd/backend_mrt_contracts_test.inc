/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
struct mrt_om_snapshot {
    struct d3d12_rtv_desc rtvs[8], dsv;
    uint32_t count, width, height, layers;
};
static struct mrt_om_snapshot mrt_snapshot(vkdu_object *command)
{
    struct d3d12_command_list *list = vkdu_test_command_iface(command);
    struct mrt_om_snapshot result = {0};
    memcpy(result.rtvs,list->rtvs,sizeof(result.rtvs)); result.dsv=list->dsv;
    result.count=list->rendering_info.info.colorAttachmentCount;
    result.width=list->fb_width;result.height=list->fb_height;result.layers=list->fb_layer_count;
    return result;
}
static void mrt_unchanged(vkdu_object *command, const struct mrt_om_snapshot *before)
{
    struct mrt_om_snapshot after=mrt_snapshot(command);
    if(memcmp(before,&after,sizeof(after))) {fputs("FAIL rejected MRT operation changed OM state\n",stderr);exit(1);}
}
struct mrt_retire_race { vkdu_object *resource; pthread_mutex_t lock; pthread_cond_t ready; int go; };
static void mrt_retire_paused(void *arg)
{
    struct mrt_retire_race *race=arg;
    pthread_mutex_lock(&race->lock);race->go=2;pthread_cond_signal(&race->ready);
    while(race->go!=3) pthread_cond_wait(&race->ready,&race->lock);
    pthread_mutex_unlock(&race->lock);
}
static void *mrt_retire_worker(void *arg)
{
    struct mrt_retire_race *race=arg;
    pthread_mutex_lock(&race->lock);
    while(!race->go) pthread_cond_wait(&race->ready,&race->lock);
    pthread_mutex_unlock(&race->lock);
    vkdu_object_destroy(race->resource);return NULL;
}
static void check_mrt_contracts(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *heap,*copy,*foreign,*dsv,*wrong,*a,*b,*wide,*tall,*array,*depth,*deep,*allocator,*command,*ca,*cc;
    const struct vkdu_texture2d_shape shape={8,8,28,2,2,1,1,0},depth_shape={8,8,40,2,2,2,1,0};
    struct vkdu_render_target_binding bind[2];struct mrt_om_snapshot saved;
    uint32_t i;
    CHECK(vkdu_heap_create(device,2,16,0,&heap));CHECK(vkdu_heap_create(device,2,16,0,&copy));
    CHECK(vkdu_heap_create(other,2,16,0,&foreign));CHECK(vkdu_heap_create(device,3,4,0,&dsv));
    CHECK(vkdu_heap_create(device,0,1,0,&wrong));
    CHECK(vkdu_render_target_create(device,4,4,&a));CHECK(vkdu_render_target_create(device,4,4,&b));
    CHECK(vkdu_render_target_create(device,8,4,&wide));CHECK(vkdu_render_target_create(device,4,8,&tall));
    CHECK(vkdu_texture2d_shape_create(device,&shape,&array));
    CHECK(vkdu_depth_create(device,4,4,40,&depth));CHECK(vkdu_texture2d_shape_create(device,&depth_shape,&deep));
    CHECK(vkdu_texture2d_rtv(heap,0,a));CHECK(vkdu_texture2d_rtv(heap,1,b));
    CHECK(vkdu_texture2d_rtv(heap,2,wide));CHECK(vkdu_texture2d_rtv(heap,3,tall));
    CHECK(vkdu_texture2d_rtv_range(heap,4,array,28,1,0,1));
    CHECK(vkdu_texture2d_rtv_range(heap,5,array,28,1,1,1));
    CHECK(vkdu_texture2d_rtv_range(heap,6,array,28,1,0,2));
    CHECK(vkdu_texture2d_dsv(dsv,0,depth,0));CHECK(vkdu_texture2d_dsv_range(dsv,1,deep,0,1,0,2));
    CHECK(vkdu_allocator_create(device,0,&allocator));CHECK(vkdu_command_create(device,allocator,0,&command));
    CHECK(vkdu_allocator_create(device,3,&ca));CHECK(vkdu_command_create(device,ca,3,&cc));
    bind[0]=(struct vkdu_render_target_binding){heap,0};bind[1]=(struct vkdu_render_target_binding){heap,1};
    CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));saved=mrt_snapshot(command);
#define MRT_BAD(expr) do {REJECT(expr);mrt_unchanged(command,&saved);} while(0)
    MRT_BAD(vkdu_command_render_targets(command,9,bind,NULL,0));
    MRT_BAD(vkdu_command_render_targets(command,UINT32_MAX,bind,NULL,0));
    MRT_BAD(vkdu_command_render_targets(command,1,NULL,NULL,0));
    for(i=2;i<=6;++i) {
        if(i==4 || i==5) continue;
        bind[1].index=i;MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    }
    bind[1].index=15;MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    bind[1].index=UINT32_MAX;MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    bind[1].index=0;MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    bind[1]=(struct vkdu_render_target_binding){foreign,0};MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    bind[1]=(struct vkdu_render_target_binding){wrong,0};MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0));
    bind[1]=(struct vkdu_render_target_binding){heap,1};
    MRT_BAD(vkdu_command_render_targets(command,2,bind,dsv,1));
    MRT_BAD(vkdu_command_render_targets(command,2,bind,heap,0));
    MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,1));
    REJECT(vkdu_command_render_targets(cc,1,bind,NULL,0));
    bind[0].index=4;bind[1].index=5;CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));
    saved=mrt_snapshot(command);
    if(saved.width!=4 || saved.height!=4 || saved.layers!=1) abort();
    CHECK(vkdu_descriptor_copy(copy,0,heap,4,2));bind[0].heap=bind[1].heap=copy;bind[0].index=0;bind[1].index=1;
    CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));saved=mrt_snapshot(command);
    /* Late invalid inputs must preserve both the descriptor bytes and metadata. */
    MRT_BAD(vkdu_texture2d_rtv_range(copy,1,array,28,1,1,2));
    CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));mrt_unchanged(command,&saved);
    {
        struct vkdu_descriptor_span to[2]={{copy,0,1},{copy,1,1}},from[2]={{heap,2,1},{heap,16,1}};
        MRT_BAD(vkdu_descriptor_copy_ranges(device,2,2,to,2,from));
        CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));mrt_unchanged(command,&saved);
        from[0].first=0;from[1].first=1;CHECK(vkdu_descriptor_copy_ranges(device,2,2,to,2,from));
        CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));
        /* A valid overwrite changes dimensions and invalidates a two-RT match. */
        CHECK(vkdu_texture2d_rtv(copy,1,wide));saved=mrt_snapshot(command);
        MRT_BAD(vkdu_command_render_targets(command,2,bind,dsv,0));
        CHECK(vkdu_texture2d_rtv(copy,1,b));CHECK(vkdu_command_render_targets(command,2,bind,dsv,0));
        from[1].first=0;CHECK(vkdu_descriptor_copy_ranges(device,2,2,to,2,from));saved=mrt_snapshot(command);
        MRT_BAD(vkdu_command_render_targets(command,2,bind,NULL,0)); /* repeated source aliases */
    }
    CHECK(vkdu_command_render_targets(command,0,NULL,NULL,0));
    for(i=0;i<2;++i) {
        vkdu_object *retired;uint32_t destroyed=0;const float clear[4]={0};
        if(i) CHECK(vkdu_depth_create(device,4,4,40,&retired)); else CHECK(vkdu_render_target_create(device,4,4,&retired));
        CHECK(vkdu_test_destroy_counter(retired,&destroyed));
        if(i) {CHECK(vkdu_texture2d_dsv(dsv,2,retired,0));CHECK(vkdu_descriptor_copy(dsv,3,dsv,2,1));}
        else {CHECK(vkdu_texture2d_rtv(heap,8,retired));CHECK(vkdu_descriptor_copy(copy,8,heap,8,1));}
        vkdu_object_destroy(retired);
        if(vkdu_test_counter_load(&destroyed)!=1) {fputs("FAIL attachment descriptor owns resource\n",stderr);exit(1);}
        saved=mrt_snapshot(command);
        if(i) {
            MRT_BAD(vkdu_command_render_targets(command,0,NULL,dsv,2));
            MRT_BAD(vkdu_command_render_targets(command,0,NULL,dsv,3));
            MRT_BAD(vkdu_command_clear_dsv(command,dsv,3,1,0.5f,0,0,NULL));
        } else {
            bind[0]=(struct vkdu_render_target_binding){heap,8};MRT_BAD(vkdu_command_render_targets(command,1,bind,NULL,0));
            bind[0]=(struct vkdu_render_target_binding){copy,8};MRT_BAD(vkdu_command_render_targets(command,1,bind,NULL,0));
            MRT_BAD(vkdu_command_clear_rtv(command,copy,8,clear,0,NULL));
            CHECK(vkdu_texture2d_rtv(copy,8,a));CHECK(vkdu_command_render_targets(command,1,bind,NULL,0));
            CHECK(vkdu_command_render_targets(command,0,NULL,NULL,0));
        }
    }
    /* Force final refs==0 while token.live still points at the wrapper. The
     * OM acquire must reject resurrection before the finalizer takes its lock. */
    {
        struct mrt_retire_race race={0};pthread_t thread;uint32_t destroyed=0;
        CHECK(vkdu_render_target_create(device,4,4,&race.resource));CHECK(vkdu_test_destroy_counter(race.resource,&destroyed));
        CHECK(vkdu_texture2d_rtv(heap,9,race.resource));bind[0]=(struct vkdu_render_target_binding){heap,9};
        if(pthread_mutex_init(&race.lock,NULL)||pthread_cond_init(&race.ready,NULL))abort();
        CHECK(vkdu_test_retire_hook(race.resource,mrt_retire_paused,&race));race.go=1;
        if(pthread_create(&thread,NULL,mrt_retire_worker,&race))abort();
        pthread_mutex_lock(&race.lock);while(race.go!=2)pthread_cond_wait(&race.ready,&race.lock);pthread_mutex_unlock(&race.lock);
        REJECT(vkdu_command_render_targets(command,1,bind,NULL,0));
        if(vkdu_test_counter_load(&destroyed))abort();
        pthread_mutex_lock(&race.lock);race.go=3;pthread_cond_signal(&race.ready);pthread_mutex_unlock(&race.lock);
        if(pthread_join(thread,NULL))abort();
        if(vkdu_test_counter_load(&destroyed)!=1)abort();
        pthread_cond_destroy(&race.ready);pthread_mutex_destroy(&race.lock);
    }
    for(i=0;i<64;++i) {
        struct mrt_retire_race race={0};pthread_t thread;uint32_t destroyed=0;int32_t result;
        CHECK(vkdu_render_target_create(device,4,4,&race.resource));CHECK(vkdu_test_destroy_counter(race.resource,&destroyed));
        CHECK(vkdu_texture2d_rtv(heap,9,race.resource));bind[0]=(struct vkdu_render_target_binding){heap,9};
        if(pthread_mutex_init(&race.lock,NULL)||pthread_cond_init(&race.ready,NULL)||pthread_create(&thread,NULL,mrt_retire_worker,&race))abort();
        pthread_mutex_lock(&race.lock);race.go=1;pthread_cond_signal(&race.ready);pthread_mutex_unlock(&race.lock);
        result=vkdu_command_render_targets(command,1,bind,NULL,0);
        if(result!=S_OK && result!=E_INVALIDARG)abort();
        if(pthread_join(thread,NULL))abort();
        pthread_cond_destroy(&race.ready);pthread_mutex_destroy(&race.lock);
        if(vkdu_test_counter_load(&destroyed)!=1)abort();
        REJECT(vkdu_command_render_targets(command,1,bind,NULL,0));
        CHECK(vkdu_command_render_targets(command,0,NULL,NULL,0));
    }
    CHECK(vkdu_command_close(command));REJECT(vkdu_command_render_targets(command,0,NULL,NULL,0));
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(cc);vkdu_object_destroy(ca);
    vkdu_object_destroy(a);vkdu_object_destroy(b);vkdu_object_destroy(wide);vkdu_object_destroy(tall);vkdu_object_destroy(array);
    vkdu_object_destroy(depth);vkdu_object_destroy(deep);vkdu_object_destroy(heap);vkdu_object_destroy(copy);
    vkdu_object_destroy(foreign);vkdu_object_destroy(dsv);vkdu_object_destroy(wrong);
    puts("PASS actual MRT contracts:atomic late input rejects; effective mip width/height/layers; DSV; disjoint layers/alias rejects; simple/range copy and overwrite; weak retired RTV/DSV; deterministic zero-ref/pre-retire acquire rejection; 64 release-versus-bind stress rounds; COPY/closed list rejection");
#undef MRT_BAD
}
#endif
