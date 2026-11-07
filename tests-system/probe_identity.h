// Local probe identity checks; no driver/private bridge entrypoints.
#pragma once
#include <windows.h>
#include <dxgi1_4.h>
#include <bcrypt.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <utility>
#include <string>
#include <vector>

inline HRESULT probe_parse_uint(const char *text, unsigned base, unsigned minimum, unsigned maximum, unsigned *out) {
    if (!text || !*text || *text == '-' || *text == '+' || *text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') return E_INVALIDARG;
    char *end = nullptr; errno = 0;
    const unsigned long value = std::strtoul(text, &end, base);
    if (errno || !end || *end || value < minimum || value > maximum) return E_INVALIDARG;
    *out = static_cast<unsigned>(value); return S_OK;
}

inline HRESULT probe_widen(const char *text, std::wstring &out) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!count) return HRESULT_FROM_WIN32(GetLastError());
    out.resize(count);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, &out[0], count)) return HRESULT_FROM_WIN32(GetLastError());
    out.resize(count - 1); return S_OK;
}

inline void probe_print_path(const char *label, const std::wstring &path) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> text(count ? count : 1, 0);
    if (count) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, text.data(), count, nullptr, nullptr);
    std::printf("MODULE %s path=%s\n", label, text.data());
}

inline HRESULT probe_module_path(HMODULE module, std::wstring &out) {
    if (!module) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count) return HRESULT_FROM_WIN32(GetLastError());
    if (count >= path.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    out.assign(path.data(), count); return S_OK;
}

inline HRESULT probe_system_module(const wchar_t *name, const char *label) {
    std::wstring path;
    HRESULT hr = probe_module_path(GetModuleHandleW(name), path);
    if (FAILED(hr)) return hr;
    probe_print_path(label, path);
    wchar_t system[MAX_PATH]; const UINT count = GetSystemDirectoryW(system, MAX_PATH);
    if (!count || count >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    const std::wstring expected = std::wstring(system) + L"\\" + name;
    return _wcsicmp(path.c_str(), expected.c_str()) ? E_ACCESSDENIED : S_OK;
}

inline HRESULT probe_hardware_adapter(IDXGIAdapter1 *adapter, LUID expected) {
    DXGI_ADAPTER_DESC1 desc{};
    HRESULT hr = adapter->GetDesc1(&desc);
    if (FAILED(hr)) return hr;
    std::printf("ADAPTER luid=%08lx:%08lx software=%u vendor=%04x device=%04x\n",
        static_cast<unsigned long>(desc.AdapterLuid.HighPart), static_cast<unsigned long>(desc.AdapterLuid.LowPart),
        !!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE), desc.VendorId, desc.DeviceId);
    if (desc.AdapterLuid.HighPart != expected.HighPart || desc.AdapterLuid.LowPart != expected.LowPart ||
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return E_ACCESSDENIED;
    return S_OK;
}

inline HRESULT probe_driver_modules() {
    if (GetModuleHandleW(L"D3D12Core.dll")) {
        const HRESULT hr = probe_system_module(L"D3D12Core.dll", "D3D12_CORE");
        if (FAILED(hr)) return hr;
    }
    for (const auto &entry : {std::make_pair(L"viogpud3d12.dll", "UMD"),
                              std::make_pair(L"viogpud3d12_vk_arm64.dll", "ICD")}) {
        std::wstring path;
        const HRESULT hr = probe_module_path(GetModuleHandleW(entry.first), path);
        if (FAILED(hr)) return hr;
        probe_print_path(entry.second, path);
    }
    return S_OK;
}

// Hold this handle without write/delete sharing until the compiler is no longer
// used. It binds the expected hash to the DLL loaded at the explicit path.
inline HRESULT probe_open_hashed_compiler(const wchar_t *path, const char *expected, HANDLE *retained) {
    if (!path || std::wcslen(path) < 3 || path[1] != L':' || path[2] != L'\\' ||
        std::strlen(expected) != 64) return E_INVALIDARG;
    for (unsigned i = 0; i < 64; ++i)
        if (!((expected[i] >= '0' && expected[i] <= '9') || (expected[i] >= 'a' && expected[i] <= 'f'))) return E_INVALIDARG;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    HRESULT result = E_FAIL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        DWORD bytes = 0, object_bytes = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &bytes, 0) >= 0) {
            std::vector<unsigned char> object(object_bytes);
            if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) >= 0) {
                unsigned char block[65536], digest[32]; bool okay = true;
                for (;;) {
                    DWORD read = 0;
                    if (!ReadFile(file, block, sizeof(block), &read, nullptr)) { okay = false; break; }
                    if (!read) break;
                    if (BCryptHashData(hash, block, read, 0) < 0) { okay = false; break; }
                }
                if (okay && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
                    char hex[65]{};
                    for (unsigned i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
                    std::printf("COMPILER sha256=%s\n", hex);
                    result = std::strcmp(hex, expected) ? E_ACCESSDENIED : S_OK;
                }
                BCryptDestroyHash(hash); hash = nullptr;
            }
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (FAILED(result)) CloseHandle(file); else *retained = file;
    return result;
}
