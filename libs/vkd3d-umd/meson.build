umd_backend_lib = static_library('vkd3d_umd_backend', 'backend.c', 'debug_policy.c',
  dependencies : vkd3d_dep,
  include_directories : vkd3d_private_includes)
umd_backend_dep = declare_dependency(link_with : umd_backend_lib,
  dependencies : vkd3d_dep, include_directories : include_directories('.'))

if vkd3d_platform == 'windows'
  umd_ddi_lib = shared_library('viogpud3d12', 'ddi.cpp',
    name_prefix : '', vs_module_defs : 'viogpud3d12.def',
    dependencies : [umd_backend_dep, lib_gdi32, lib_dxgi],
    include_directories : vkd3d_private_includes,
    cpp_args : ['-DNOMINMAX'])
  if get_option('enable_umd_bridge_tests')
    # Deliberately no backend dependency: this is an ordinary system D3D12 app.
    umd_system_probe = executable('vkd3d-system-d3d12-probe', 'system_d3d12_probe.cpp', cpp_args : ['-DNOMINMAX'])
    umd_runtime_test = executable('vkd3d-umd-runtime-test', 'runtime_test.cpp',
      dependencies : [umd_backend_dep, lib_gdi32, lib_dxgi],
      include_directories : vkd3d_private_includes, cpp_args : ['-DNOMINMAX'])
    # Actual typed callbacks and real CPU Vulkan share the same object/queue chain.
    # Internal bridge setup only; this does not advertise the public native table.
    umd_ddi_root_data_test = executable('vkd3d-umd-ddi-root-data-test',
      'backend.c', 'debug_policy.c', 'ddi_bundle_root_data_test.cpp',
      c_args : ['-DVKDU_ENABLE_TEST_DEVICE'],
      cpp_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DNOMINMAX'],
      dependencies : [vkd3d_dep, lib_gdi32, lib_dxgi],
      include_directories : vkd3d_private_includes)
    umd_shared_gpu_probe = executable('vkd3d-umd-shared-gpu-probe', 'runtime_gpu_probe.cpp',
      dependencies : [umd_backend_dep, lib_gdi32, lib_dxgi],
      include_directories : vkd3d_private_includes, cpp_args : ['-DNOMINMAX'])
    # Uses the production backend; the CPU test-device entrypoint is absent.
    umd_gpu_probe = executable('vkd3d-umd-gpu-probe', 'backend_test.c',
      c_args : ['-DVKDU_GPU_PROBE'], dependencies : umd_backend_dep,
      include_directories : vkd3d_private_includes)
    umd_ddi_abi_test = executable('vkd3d-umd-ddi-abi-test', 'ddi_abi_test.cpp',
      include_directories : vkd3d_public_includes, cpp_args : ['-DNOMINMAX'])
    umd_ddi_descriptor_test = executable('vkd3d-umd-ddi-descriptor-test', 'ddi_descriptor_test.cpp',
      dependencies : [umd_backend_dep, lib_gdi32, lib_dxgi],
      include_directories : vkd3d_private_includes, cpp_args : ['-DNOMINMAX'])
  endif
endif

if get_option('enable_umd_bridge_tests')
  # The root-state fixture reads actual private command-list layout. Build tags
  # must be generated before compiling that fixture, including in clean trees.
  umd_test_state_dep = declare_dependency(dependencies : vkd3d_dep,
    sources : [vkd3d_build, vkd3d_version], include_directories : include_directories('../..'))
  umd_fence_test = executable('vkd3d-umd-fence-test', 'backend.c', 'debug_policy.c', 'fence_test.cpp',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE'], cpp_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DNOMINMAX'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  test('vkd3d-umd-fence-events', umd_fence_test, timeout : 60)
  umd_fence_negative = executable('vkd3d-umd-fence-negative', 'backend.c', 'debug_policy.c', 'fence_test.cpp',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_FENCE_DROP_WAIT'],
    cpp_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DNOMINMAX'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  # Separate object build. The CPU-only test entrypoint is not in production.
  umd_backend_test = executable('vkd3d-umd-backend-test', 'backend_topology_observer_test.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_TOPOLOGY_OBSERVER'],
    dependencies : umd_test_state_dep,
    include_directories : vkd3d_private_includes)
  test('vkd3d-umd-backend', umd_backend_test, timeout : 120)
  umd_subresource_copy_negative = executable('vkd3d-umd-subresource-copy-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SUBRESOURCE_COPY_WRONG_LAYER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_subresource_view_negative = executable('vkd3d-umd-subresource-view-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SUBRESOURCE_VIEW_BASE_ZERO'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_attachment_predicate_negative = executable('vkd3d-umd-attachment-predicate-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_ATTACHMENT_INVERT'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_attachment_query_negative = executable('vkd3d-umd-attachment-query-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_ATTACHMENT_QUERY_PREDICATED'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  foreach wrong : ['mip', 'layer']
    executable('vkd3d-umd-attachment-view-' + wrong + '-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
      c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_ATTACHMENT_VIEW_' + wrong.to_upper()],
      dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  endforeach
  umd_predication_negative = executable('vkd3d-umd-predication-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_PREDICATION_DROP'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_predication_owner_negative = executable('vkd3d-umd-predication-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_PREDICATION_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_uav_clear_negative = executable('vkd3d-umd-uav-clear-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_UAV_CLEAR_DROP'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_uav_clear_owner_negative = executable('vkd3d-umd-uav-clear-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_UAV_CLEAR_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_root_clear_negative = executable('vkd3d-umd-root-clear-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_ROOT_CLEAR_DROP'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_typeless_negative = executable('vkd3d-umd-typeless-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_TYPELESS_DROP_SRGB'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_alias_owner_negative = executable('vkd3d-umd-alias-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_ALIAS_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_cached_heap_negative = executable('vkd3d-umd-cached-heap-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_CACHED_HEAP_AS_READBACK'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  # Deliberately corrupt actual sampler translation in this CPU-only binary.
  # The real sampling readback must reject it; production has neither define.
  umd_sampler_negative = executable('vkd3d-umd-sampler-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SAMPLER_DROP_ADDRESS'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_texture_negative = executable('vkd3d-umd-texture-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_TEXTURE_DROP_OFFSET'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_graphics_negative = executable('vkd3d-umd-graphics-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_GRAPHICS_DROP_SCISSOR'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_graphics_root_negative = executable('vkd3d-umd-graphics-root-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_GRAPHICS_ROOT_DROP_OFFSET'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_depth_negative = executable('vkd3d-umd-depth-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_DEPTH_DROP_COMPARE'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_blend_negative = executable('vkd3d-umd-blend-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_BLEND_DROP_FACTOR'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_bgra_negative = executable('vkd3d-umd-bgra-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_BGRA_DROP_BLUE'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_srgb_negative = executable('vkd3d-umd-srgb-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SRGB_DROP_DECODE'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_d16_negative = executable('vkd3d-umd-d16-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_D16_DROP_OFFSET'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_resource_copy_negative = executable('vkd3d-umd-resource-copy-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_RESOURCE_COPY_DROP'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_query_resolve_negative = executable('vkd3d-umd-query-resolve-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_QUERY_DROP_RESOLVE'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_query_owner_negative = executable('vkd3d-umd-query-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_QUERY_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_vertex_offset_negative = executable('vkd3d-umd-vertex-offset-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_VERTEX_DROP_OFFSET'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_vertex_owner_negative = executable('vkd3d-umd-vertex-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_VERTEX_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_tess_factors_negative = executable('vkd3d-umd-tess-factors-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_TESS_SWAP_LINE_FACTORS'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_tess_constants_negative = executable('vkd3d-umd-tess-constants-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_TESS_DROP_HULL_CONSTANTS'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_so_component_negative = executable('vkd3d-umd-so-component-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SO_DROP_COMPONENT'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_so_raster_negative = executable('vkd3d-umd-so-raster-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SO_DROP_RASTER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_so_clip_negative = executable('vkd3d-umd-so-clip-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SO_DROP_CLIP_INDEX'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_distance_link_negative = executable('vkd3d-umd-distance-link-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_DISTANCE_DROP_PACKING'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_so_offset_negative = executable('vkd3d-umd-so-offset-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SO_DROP_OFFSET'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
  umd_so_owner_negative = executable('vkd3d-umd-so-owner-negative', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE', '-DVKDU_TEST_SO_DROP_OWNER'],
    dependencies : umd_test_state_dep, include_directories : vkd3d_private_includes)
endif
