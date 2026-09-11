/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WDK lifecycle fixture with controlled Vulkan/backend peers, NOT system
 * D3D12CreateDevice or VIOGPU GPU acceptance. Uses actual production entry. */
#include "ddi.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <vector>

static HRESULT backend_result = S_OK, query_result = S_OK;
static unsigned loads, unloads, creates, destroys, query_calls, error_calls;
static bool bad_loader = false, old_reply = false;
static uint64_t generation = 7;
static D3D12DDI_HRTDEVICE last_runtime{};
static const HANDLE expected_adapter = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x13570));
static const std::array<uint8_t, 8> expected_luid{1,2,3,4,5,6,7,8};
static HMODULE WINAPI test_load(LPCWSTR name, HANDLE, DWORD flags) {
    if (std::wcscmp(name, L"vulkan-1.dll") || flags != LOAD_LIBRARY_SEARCH_SYSTEM32) std::abort();
    ++loads;
    if (bad_loader) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x24680));
}
static FARPROC WINAPI test_symbol(HMODULE, LPCSTR name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr")) std::abort();
    return reinterpret_cast<FARPROC>(static_cast<uintptr_t>(0x35790));
}
static BOOL WINAPI test_unload(HMODULE) { ++unloads; return TRUE; }
static int32_t test_create(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8], vkdu_device **out) {
    if (!loader || std::memcmp(luid, expected_luid.data(), 8)) std::abort();
    ++creates; *out = nullptr;
    if (SUCCEEDED(backend_result)) *out = reinterpret_cast<vkdu_device *>(new unsigned(99));
    return backend_result;
}
static void test_destroy(vkdu_device *device) {
    if (device) { ++destroys; delete reinterpret_cast<unsigned *>(device); }
}
static HRESULT APIENTRY test_query(HANDLE runtime, const D3DDDICB_QUERYADAPTERINFO *args) {
    if (runtime != expected_adapter || args->PrivateDriverDataSize != 160) std::abort();
    auto *bytes = static_cast<uint8_t *>(args->pPrivateDriverData);
    for (unsigned i = 0; i < 160; ++i) if (bytes[i]) std::abort();
    ++query_calls;
    if (FAILED(query_result)) return query_result;
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes + offset, &value, 4); };
    put(0, 0x504d5644); put(8, 128);
    std::memcpy(bytes + 24, &generation, 8);
    if (!old_reply) {
        put(128, 0x44494c56); put(132, 1); put(136, 32); put(140, 1); put(152, 1);
        std::memcpy(bytes + 144, expected_luid.data(), 8);
    }
    return S_OK;
}
static void APIENTRY test_error(D3D10DDI_HRTDEVICE runtime, HRESULT result) {
    if (result != E_INVALIDARG) std::abort();
    ++error_calls; last_runtime = runtime;
}

#define LoadLibraryExW test_load
#define GetProcAddress test_symbol
#define FreeLibrary test_unload
#define vkdu_device_create_runtime test_create
#define vkdu_device_destroy test_destroy
#include "ddi.cpp"

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    REQUIRE(OpenAdapter12(nullptr) == E_INVALIDARG);
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    callbacks.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS funcs{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter;
    open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &funcs;
    old_reply = true;
    REQUIRE(OpenAdapter12(&open) == DXGI_ERROR_UNSUPPORTED && !open.hAdapter.pDrvPrivate);
    old_reply = false; query_result = E_ACCESSDENIED;
    REQUIRE(OpenAdapter12(&open) == E_ACCESSDENIED && !open.hAdapter.pDrvPrivate);
    query_result = S_OK;
    REQUIRE(OpenAdapter12(&open) == S_OK && open.hAdapter.pDrvPrivate && !loads);
    callbacks.pfnQueryAdapterInfoCb = nullptr; // Must retain the original callback.
    UINT count = 123;
    std::array<UINT64, 2> versions{0x1234, 0x5678};
    REQUIRE(funcs.pfnGetSupportedVersions(open.hAdapter, &count, versions.data()) == S_OK && count == 0);
    REQUIRE(versions[0] == 0x1234 && versions[1] == 0x5678);
    REQUIRE(funcs.pfnGetOptionalDDITables(open.hAdapter, &count, nullptr) == S_OK && count == 0);
    D3D12DDI_3DPIPELINELEVEL level = D3D12DDI_3DPIPELINELEVEL_12_2;
    D3D12DDIARG_GETCAPS caps{D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, nullptr, &level, sizeof(level)};
    REQUIRE(funcs.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(level == D3D12DDI_3DPIPELINELEVEL_12_2);
    std::array<unsigned char, sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0003)> table;
    table.fill(0xa5);
    REQUIRE(funcs.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        table.data(), table.size(), 0, {}) == DXGI_ERROR_UNSUPPORTED);
    for (auto byte : table) REQUIRE(byte == 0xa5);
    REQUIRE(funcs.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        table.data(), table.size() - 1, 0, {}) == E_INVALIDARG);
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size{D3D12DDI_INTERFACE_VERSION_R0, D3D12DDI_BUILD_VERSION << 16, D3D12DDI_CREATE_DEVICE_FLAG_NONE};
    REQUIRE(funcs.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == sizeof(NativeDevice));
    ++size.Interface;
    REQUIRE(funcs.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == 0);
    --size.Interface;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice) + 16> storage;
    storage.fill(0xa5);
    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnSetErrorCb = test_error;
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hRTDevice.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x456a0));
    create.Interface = size.Interface; create.Version = size.Version;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    create.hDrvDevice.pDrvPrivate = storage.data();
    ++generation;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED && !loads);
    --generation; bad_loader = true;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    bad_loader = false; backend_result = E_OUTOFMEMORY;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == E_OUTOFMEMORY && unloads == 1 && destroys == 0);
    for (auto byte : storage) REQUIRE(byte == 0xa5);
    backend_result = S_OK;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    for (size_t i = sizeof(NativeDevice); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(ctx && ctx->backend && ctx->runtime_device.handle == create.hRTDevice.handle);
    um.pfnSetErrorCb = nullptr;
    error(ctx, E_INVALIDARG);
    REQUIRE(error_calls == 1 && last_runtime.handle == create.hRTDevice.handle);
    std::weak_ptr<NativeAdapter> retained = ctx->native_adapter;
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == S_OK && !retained.expired());
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == E_INVALIDARG);
    // Model an existing object's Context reference after runtime slot retirement.
    ++ctx->references;
    funcs.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(!context(create.hDrvDevice) && destroys == 0 && unloads == 1);
    error(ctx, E_INVALIDARG);
    REQUIRE(error_calls == 1 && !ctx->runtime_device.handle && !ctx->kernel_callbacks.pfnAllocateCb);
    release(ctx);
    REQUIRE(destroys == 1 && unloads == 2 && retained.expired());
    funcs.pfnDestroyDevice(create.hDrvDevice); // Runtime memory is never deleted.
    for (size_t i = sizeof(NativeDevice); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
    std::printf("PASS native OpenAdapter12 WDK identity/negotiation/private memory/callback/lifetime/error cleanup (%zu-bit); no system-runtime or GPU acceptance\n", sizeof(void *) * 8);
    return 0;
}
