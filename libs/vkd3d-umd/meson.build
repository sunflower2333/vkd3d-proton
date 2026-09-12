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
    umd_runtime_test = executable('vkd3d-umd-runtime-test', 'runtime_test.cpp',
      dependencies : [umd_backend_dep, lib_gdi32, lib_dxgi],
      include_directories : vkd3d_private_includes, cpp_args : ['-DNOMINMAX'])
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
  # Separate object build. The CPU-only test entrypoint is not in production.
  umd_backend_test = executable('vkd3d-umd-backend-test', 'backend.c', 'debug_policy.c', 'backend_test.c',
    c_args : ['-DVKDU_ENABLE_TEST_DEVICE'],
    dependencies : vkd3d_dep,
    include_directories : vkd3d_private_includes)
  test('vkd3d-umd-backend', umd_backend_test, timeout : 120)
endif
