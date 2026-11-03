// KMT capability probe: WDDM caps relevant to D3D12 (GPU VA, monitored fences,
// virtual contexts). Usage: kmtcaps.exe <luid-low-hex>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void dump(const char *name, NTSTATUS st, const void *data, size_t size) {
    std::printf("%s status=%08lx", name, (unsigned long)st);
    if (st == 0) {
        const auto *b = static_cast<const unsigned char *>(data);
        std::printf(" bytes=");
        for (size_t i = 0; i < size; ++i) std::printf("%02x", b[i]);
    }
    std::printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { std::puts("usage: kmtcaps <luid-low-hex>"); return 2; }
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid.LowPart = strtoul(argv[1], nullptr, 16);
    NTSTATUS st = D3DKMTOpenAdapterFromLuid(&open);
    std::printf("OpenAdapterFromLuid status=%08lx\n", (unsigned long)st);
    if (st) return 1;

    auto query = [&](const char *name, KMTQUERYADAPTERINFOTYPE type, void *data, UINT size) {
        D3DKMT_QUERYADAPTERINFO q{};
        q.hAdapter = open.hAdapter; q.Type = type; q.pPrivateDriverData = data; q.PrivateDriverDataSize = size;
        NTSTATUS s = D3DKMTQueryAdapterInfo(&q);
        dump(name, s, data, size);
        return s;
    };
    D3DKMT_DRIVERCAPS_EXT ext{};
    query("DRIVERCAPS_EXT", KMTQAITYPE_DRIVERCAPS_EXT, &ext, sizeof(ext));
    D3DKMT_WDDM_2_0_CAPS c20{};
    if (!query("WDDM_2_0_CAPS", KMTQAITYPE_WDDM_2_0_CAPS, &c20, sizeof(c20)))
        std::printf("  PagingProcessGpuVirtualAddress? Support64BitAtomics=%u GpuMmuSupported=%u IoMmuSupported=%u\n",
            c20.Support64BitAtomics, c20.GpuMmuSupported, c20.IoMmuSupported);
    D3DKMT_WDDM_2_7_CAPS c27{};
    if (!query("WDDM_2_7_CAPS", KMTQAITYPE_WDDM_2_7_CAPS, &c27, sizeof(c27)))
        std::printf("  HwSchSupported=%u HwSchEnabled=%u\n", c27.HwSchSupported, c27.HwSchEnabled);
    D3DKMT_ADAPTERTYPE at{};
    if (!query("ADAPTERTYPE", KMTQAITYPE_ADAPTERTYPE, &at, sizeof(at)))
        std::printf("  RenderSupported=%u DisplaySupported=%u SoftwareDevice=%u Paravirtualized=%u\n",
            at.RenderSupported, at.DisplaySupported, at.SoftwareDevice, at.Paravirtualized);

    D3DKMT_CREATEDEVICE dev{};
    dev.hAdapter = open.hAdapter;
    st = D3DKMTCreateDevice(&dev);
    std::printf("CreateDevice status=%08lx\n", (unsigned long)st);
    if (!st) {
        for (UINT hint : {(UINT)D3DKMT_CLIENTHINT_DX12, (UINT)D3DKMT_CLIENTHINT_DX11, (UINT)D3DKMT_CLIENTHINT_UNKNOWN}) {
            for (UINT affinity : {0u, 1u}) {
                D3DKMT_CREATECONTEXTVIRTUAL cv{};
                cv.hDevice = dev.hDevice; cv.NodeOrdinal = 0; cv.EngineAffinity = affinity;
                cv.ClientHint = (D3DKMT_CLIENTHINT)hint;
                st = D3DKMTCreateContextVirtual(&cv);
                std::printf("CreateContextVirtual hint=%u affinity=%u status=%08lx\n", hint, affinity, (unsigned long)st);
                if (!st) { D3DKMT_DESTROYCONTEXT d{}; d.hContext = cv.hContext; D3DKMTDestroyContext(&d); }
            }
        }
        for (UINT affinity : {0u, 1u}) {
            D3DKMT_CREATECONTEXTVIRTUAL cv{};
            cv.hDevice = dev.hDevice; cv.NodeOrdinal = 0; cv.EngineAffinity = affinity;
            cv.Flags.SynchronizationOnly = 1;
            st = D3DKMTCreateContextVirtual(&cv);
            std::printf("CreateContextVirtual SynchronizationOnly affinity=%u status=%08lx\n", affinity, (unsigned long)st);
            if (!st) { D3DKMT_DESTROYCONTEXT d{}; d.hContext = cv.hContext; D3DKMTDestroyContext(&d); }
        }
        {
            D3DKMT_CREATECONTEXT c{};
            c.hDevice = dev.hDevice; c.NodeOrdinal = 0; c.EngineAffinity = 1;
            c.Flags.SynchronizationOnly = 1;
            st = D3DKMTCreateContext(&c);
            std::printf("CreateContext(legacy) SynchronizationOnly status=%08lx\n", (unsigned long)st);
            if (!st) { D3DKMT_DESTROYCONTEXT d{}; d.hContext = c.hContext; D3DKMTDestroyContext(&d); }
        }
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 so{};
        so.hDevice = dev.hDevice;
        so.Info.Type = D3DDDI_MONITORED_FENCE;
        so.Info.MonitoredFence.InitialFenceValue = 0;
        st = D3DKMTCreateSynchronizationObject2(&so);
        std::printf("CreateSynchronizationObject2(MONITORED_FENCE) status=%08lx cpu=%p gpuva=%llx\n", (unsigned long)st,
            so.Info.MonitoredFence.FenceValueCPUVirtualAddress,
            (unsigned long long)so.Info.MonitoredFence.FenceValueGPUVirtualAddress);
        if (!st) { D3DKMT_DESTROYSYNCHRONIZATIONOBJECT d{}; d.hSyncObject = so.hSyncObject; D3DKMTDestroySynchronizationObject(&d); }
        D3DKMT_DESTROYDEVICE dd{}; dd.hDevice = dev.hDevice; D3DKMTDestroyDevice(&dd);
    }
    D3DKMT_CLOSEADAPTER close{}; close.hAdapter = open.hAdapter; D3DKMTCloseAdapter(&close);
    return 0;
}
