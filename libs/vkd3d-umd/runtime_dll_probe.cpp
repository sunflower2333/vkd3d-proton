/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent executable: public WDK types and three typed DLL exports only.
 * No embedded UMD/backend/engine and no private C++ object layout access. */
#include <windows.h>
#if defined(__MINGW32__) && !defined(__out)
#define DLL_PROBE_DEFINE_OUT
#define __out
#endif
#include <d3d12umddi.h>
#ifdef DLL_PROBE_DEFINE_OUT
#undef __out
#undef DLL_PROBE_DEFINE_OUT
#endif
#include <d3dkmthk.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct ProbeFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool value, const char *message) {
    if (!value) { std::fprintf(stdout, "FAIL %s\n", message); throw ProbeFailure(message); }
}
void check(HRESULT value, const char *message) {
    if (FAILED(value)) {
        std::fprintf(stdout, "FAIL %s hr=%08x\n", message, static_cast<unsigned>(value));
        throw ProbeFailure(message);
    }
}

#include "runtime_dll_platform.inc"
#include "runtime_dll_provider.inc"
#include "runtime_dll_workload.inc"
#include "runtime_dll_graphics.inc"
#include "runtime_dll_controls.inc"
#include "runtime_dll_residency_controls.inc"
#include "runtime_dll_bundle_root_data.inc"

bool parse_luid_word(const wchar_t *text, UINT &value) {
    if (!text || !*text || std::wcslen(text) > 8) return false;
    UINT result = 0;
    for (const auto *p = text; *p; ++p) {
        UINT digit = *p >= L'0' && *p <= L'9' ? *p - L'0' : *p >= L'a' && *p <= L'f' ? *p - L'a' + 10 :
            *p >= L'A' && *p <= L'F' ? *p - L'A' + 10 : 16;
        if (digit == 16) return false;
        result = (result << 4) | digit;
    }
    value = result; return true;
}
}

int wmain(int argc, wchar_t **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
#if defined(VKDU_PROBE_TEST_RESIDENCY_EARLY_USE) || defined(VKDU_PROBE_TEST_RESIDENCY_DROP_REF) || \
    defined(VKDU_PROBE_TEST_RESIDENCY_RELEASE_BEFORE_RENDER)
        if (argc != 2 || std::wcscmp(argv[1], L"--residency-self-test")) {
            std::fputs("CONTROL-ONLY residency mutation binary; all native modes disabled\n", stdout); return 2;
        }
#endif
#if defined(VKDU_PROBE_TEST_DROP_SYNCHRONOUS) || defined(VKDU_PROBE_TEST_WRONG_EVENT_CONTEXT) || \
    defined(VKDU_PROBE_TEST_CPU_COMPLETION) || defined(VKDU_PROBE_TEST_EARLY_RELEASE)
        if (argc != 2 || (std::wcscmp(argv[1], L"--self-test") &&
                std::wcscmp(argv[1], L"--residency-self-test") &&
                std::wcscmp(argv[1], L"--bundle-root-data-self-test") &&
                std::wcscmp(argv[1], L"--graphics-self-test") &&
                std::wcscmp(argv[1], L"--graphics-identity-self-test"))) {
            std::fputs("CONTROL-ONLY binary: all native modes disabled\n", stdout); return 2;
        }
#endif
        if (argc == 2 && !std::wcscmp(argv[1], L"--residency-self-test")) {
            residency_provider_controls(); return 0;
        }
        if (argc == 2 && !std::wcscmp(argv[1], L"--bundle-root-data-self-test")) {
            dll_bundle_root_data_controls(); return 0;
        }
        if (argc == 8 && !std::wcscmp(argv[1], L"--bundle-root-data")) {
            UINT high = 0, low = 0;
            require(parse_luid_word(argv[6], high) && parse_luid_word(argv[7], low) && (high | low), "explicit nonzero adapter LUID hex high/low");
            run_dll_bundle_root_data(argv[2],argv[3],argv[4],argv[5],{low,static_cast<LONG>(high)});
            return 0;
        }
        if (argc == 2 && !std::wcscmp(argv[1], L"--graphics-identity-self-test")) {
            dll_graphics_identity_controls(); return 0;
        }
        if (argc == 2 && !std::wcscmp(argv[1], L"--graphics-self-test")) {
            dll_graphics_controls(); return 0;
        }
        if (argc == 2 && !std::wcscmp(argv[1], L"--self-test")) {
            platform_controls(); provider_controls(); return 0;
        }
        if ((argc == 4 || argc == 5) && !std::wcscmp(argv[1], L"--inspect")) {
            if (argc == 5) require(!std::wcscmp(argv[4], L"foreign-pointer") || !std::wcscmp(argv[4], L"preloaded"),
                "known local-only inspection control");
            inspect_dll(argv[2], argv[3], argc == 5 ? argv[4] : nullptr); return 0;
        }
        if (argc == 8 && !std::wcscmp(argv[1], L"--residency-copy")) {
            UINT high = 0, low = 0;
            require(parse_luid_word(argv[6], high) && parse_luid_word(argv[7], low) && (high | low), "explicit nonzero adapter LUID hex high/low");
            run_dll_workload(false, argv[2], argv[3], argv[4], argv[5], {low, static_cast<LONG>(high)}, true);
            return 0;
        }
        if (argc == 8 && (!std::wcscmp(argv[1], L"--copy") || !std::wcscmp(argv[1], L"--compute") || !std::wcscmp(argv[1], L"--graphics"))) {
            UINT high = 0, low = 0;
            require(parse_luid_word(argv[6], high) && parse_luid_word(argv[7], low) && (high | low), "explicit nonzero adapter LUID hex high/low");
            if (!std::wcscmp(argv[1], L"--graphics"))
                run_dll_graphics_workload(argv[2], argv[3], argv[4], argv[5], {low, static_cast<LONG>(high)});
            else run_dll_workload(!std::wcscmp(argv[1], L"--compute"), argv[2], argv[3], argv[4], argv[5], {low, static_cast<LONG>(high)});
            return 0;
        }
        std::fputs("Usage: native-dll-probe --residency-self-test | --bundle-root-data-self-test | --self-test | --graphics-self-test | --graphics-identity-self-test | --inspect ABSOLUTE_DLL SHA256 [foreign-pointer|preloaded]\n"
            "       --residency-copy|--bundle-root-data|--copy|--compute|--graphics ABSOLUTE_DLL CORE_SHA256 ABSOLUTE_ICD_JSON ICD_DLL_SHA256 LUID_HIGH_HEX LUID_LOW_HEX\n", stdout);
        return 2;
    } catch (const std::exception &error) {
        std::fprintf(stdout, "FAIL independent probe: %s\n", error.what()); return 1;
    } catch (...) {
        std::fputs("FAIL independent probe: unknown exception\n", stdout); return 1;
    }
}
