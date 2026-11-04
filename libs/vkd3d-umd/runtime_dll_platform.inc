/* Diagnostic-only fixed recorder. No I/O, allocation or mutex in emit(). */
enum class TlKind : uint64_t {
    Run=1, CoreLoad, IcdConfigure, RuntimeOpen, OpenAdapter, CreateDevice, IcdVerify,
    CheckAllocation, CalcSizes, CreateHeap, Status, QueryKmt, QueryParse, QueryFinal,
    CreateContext, Escape, AllocationRequest, CreateAllocation, PagingQueue, MakeResident,
    PagingState, PagingEnqueue, PagingWait, RenderCallback, RenderKmt, Signal, Observe,
    DestroyContext, Execute, SignalFence, BatchWait, DestroyQueue, Map, Terminal, Retain, Error,
    KmtOpen, KmtDevice
};
struct TlLastError { DWORD value = GetLastError(); ~TlLastError() { SetLastError(value); } };
static uint64_t tl_ptr(const void *p) noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }
static uint64_t tl_u32(HRESULT n) noexcept { return static_cast<uint32_t>(n); }
static uint64_t tl_luid(LUID n) noexcept { return uint64_t(n.LowPart) | (uint64_t(static_cast<uint32_t>(n.HighPart)) << 32); }
struct Timeline {
    static constexpr uint64_t capacity = 8192, closed = uint64_t(1) << 63;
    static constexpr uint64_t magic = 0x31454e494c544456ull, end_magic = 0x31444e454c544456ull;
    struct Record { uint64_t words[24]{}; };
    struct Slot { Record record; std::atomic_uint ready{0}; };
    static_assert(sizeof(Record) == 192, "timeline record wire size");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "callback reservation is lock-free");
    std::array<Slot, capacity> slots{};
    std::array<uint64_t, 16 + capacity * 24 + 8> output{};
    std::atomic<uint64_t> reservation{closed}, calls{0}, overflow{0};
    std::atomic_uint clock_fault{0};
    uint64_t frequency=0, anchor_before=0, anchor_utc=0, anchor_after=0;
    uint64_t reserved=0, published=0, incomplete=0;
    bool started=false;
    wchar_t binary_path[32768]{}, status_path[32768]{};
    DWORD path_error=0;
    BOOL (WINAPI *qpc)(LARGE_INTEGER *) = QueryPerformanceCounter;
    BOOL (WINAPI *qpf)(LARGE_INTEGER *) = QueryPerformanceFrequency;
    BOOL (WINAPI *write_file)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) = WriteFile;
    void begin(bool capture_path=true) noexcept {
        TlLastError preserve;
        // Called once before workers, or after all control threads have joined.
        reservation.store(closed); calls.store(0); overflow.store(0); clock_fault.store(0);
        for (auto &slot : slots) slot.ready.store(0, std::memory_order_relaxed);
        frequency=anchor_before=anchor_utc=anchor_after=reserved=published=incomplete=0;
        path_error=0; binary_path[0]=status_path[0]=0;
        LARGE_INTEGER f{}, before{}, after{}; FILETIME utc{};
        if (!qpf(&f) || f.QuadPart <= 0) clock_fault.fetch_or(1); else frequency=f.QuadPart;
        if (!qpc(&before)) clock_fault.fetch_or(2); else anchor_before=before.QuadPart;
        GetSystemTimePreciseAsFileTime(&utc);
        anchor_utc=uint64_t(utc.dwLowDateTime)|(uint64_t(utc.dwHighDateTime)<<32);
        if (!anchor_utc) clock_fault.fetch_or(4);
        if (!qpc(&after)) clock_fault.fetch_or(8); else anchor_after=after.QuadPart;
        if (anchor_after < anchor_before) clock_fault.fetch_or(16);
        if (capture_path) {
            wchar_t cwd[32768]{};
            const DWORD n=GetCurrentDirectoryW(32768,cwd);
            if (!n || n >= 32680 || n < 3 || cwd[1] != L':' || cwd[2] != L'\\')
                path_error=GetLastError() ? GetLastError() : ERROR_INVALID_NAME;
            else {
                std::wcscpy(binary_path,cwd); std::wcscat(binary_path,L"\\native-dll-timeline.bin");
                std::wcscpy(status_path,cwd); std::wcscat(status_path,L"\\native-dll-timeline-status.json");
            }
        }
        started=true; reservation.store(0,std::memory_order_release);
    }
    uint64_t next_call() noexcept { TlLastError preserve; return calls.fetch_add(1,std::memory_order_relaxed)+1; }
    uint64_t reserve() noexcept {
        uint64_t value=reservation.load(std::memory_order_relaxed);
        for (;;) {
            if (value & closed) return capacity;
            if (value >= capacity) { overflow.fetch_add(1,std::memory_order_relaxed); return capacity; }
            if (reservation.compare_exchange_weak(value,value+1,std::memory_order_acq_rel,std::memory_order_relaxed)) return value;
        }
    }
    void publish(uint64_t slot, uint64_t call, TlKind kind, uint64_t edge,
            std::initializer_list<uint64_t> fields) noexcept {
        if (slot >= capacity) return;
        auto &out=slots[slot].record;
        out=Record{};
        LARGE_INTEGER stamp{};
        if (!qpc(&stamp)) clock_fault.fetch_or(32,std::memory_order_relaxed);
        out.words[0]=slot; out.words[1]=call; out.words[2]=stamp.QuadPart;
        out.words[3]=GetCurrentThreadId(); out.words[4]=static_cast<uint64_t>(kind); out.words[5]=edge;
        size_t index=6;
        for (uint64_t field : fields) { if (index < 24) out.words[index++]=field; }
        slots[slot].ready.store(1,std::memory_order_release);
    }
    void emit(uint64_t call, TlKind kind, uint64_t edge, std::initializer_list<uint64_t> fields={}) noexcept {
        TlLastError preserve;
        publish(reserve(),call,kind,edge,fields);
    }
    DWORD snapshot(unsigned exit, unsigned reason) noexcept {
        TlLastError preserve;
        const uint64_t word=reservation.fetch_or(closed,std::memory_order_acq_rel);
        reserved=word & ~closed; published=incomplete=0;
        // Read only release-published immutable slots, never an unfinished writer.
        for (uint64_t n=0;n<reserved;++n) {
            if (slots[n].ready.load(std::memory_order_acquire)) {
                std::memcpy(output.data()+16+published*24,slots[n].record.words,192); ++published;
            } else ++incomplete;
        }
        const uint64_t header[16]={magic,1,128,192,capacity,reserved,published,overflow.load(),incomplete,
            clock_fault.load(),frequency,anchor_before,anchor_utc,anchor_after,GetCurrentProcessId(),0};
        std::memcpy(output.data(),header,sizeof(header));
        const uint64_t trailer[8]={end_magic,published,exit,reason,1,0,0,0};
        std::memcpy(output.data()+16+published*24,trailer,sizeof(trailer));
        return static_cast<DWORD>(128+published*192+64);
    }
    struct WriteResult { DWORD expected=0, actual=0, error=0; BOOL ok=FALSE; };
    WriteResult write_once(HANDLE h,const void *data,DWORD size) noexcept {
        TlLastError preserve;
        WriteResult result{}; result.expected=size;
        result.ok=write_file(h,data,size,&result.actual,nullptr);
        result.error=result.ok ? 0 : GetLastError();
        return result;
    }
    void finish(unsigned exit,unsigned reason) noexcept {
        TlLastError preserve;
        if (!started) return;
        const DWORD bytes=snapshot(exit,reason);
        if (path_error) return; // Missing closed files explicitly fails observation.
        HANDLE binary=CreateFileW(binary_path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        const DWORD open_error=binary==INVALID_HANDLE_VALUE ? GetLastError() : 0;
        HANDLE status=CreateFileW(status_path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        WriteResult write{}; write.expected=bytes;
        BOOL close_ok=FALSE; DWORD close_error=0;
        if (binary!=INVALID_HANDLE_VALUE) {
            write=write_once(binary,output.data(),bytes);
            close_ok=CloseHandle(binary); close_error=close_ok ? 0 : GetLastError();
        }
        if (status!=INVALID_HANDLE_VALUE) {
            char text[2048]{};
            const int count=std::snprintf(text,sizeof(text),
                "{\"schema\":\"native-dll-timeline-v1\",\"expected_bytes\":%u,\"actual_bytes\":%u,"
                "\"binary_open_error\":%u,\"binary_write_ok\":%u,\"binary_write_error\":%u,"
                "\"binary_close_ok\":%u,\"binary_close_error\":%u,\"reserved\":%llu,\"published\":%llu,"
                "\"overflow\":%llu,\"incomplete\":%llu,\"clock_fault\":%llu,\"workload_exit\":%u,"
                "\"termination_reason\":%u,\"status_close\":\"external-closed-file-check-required\"}\n",
                static_cast<unsigned>(bytes),static_cast<unsigned>(write.actual),static_cast<unsigned>(open_error),write.ok?1u:0u,
                static_cast<unsigned>(write.error),close_ok?1u:0u,static_cast<unsigned>(close_error),
                static_cast<unsigned long long>(reserved),static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(output[7]),static_cast<unsigned long long>(incomplete),
                static_cast<unsigned long long>(output[9]),exit,reason);
            if (count>0 && static_cast<size_t>(count)<sizeof(text)) write_once(status,text,static_cast<DWORD>(count));
            CloseHandle(status); // Its own write/close cannot self-certify; outer verifies both closed files.
        }
    }
};
static Timeline timeline;
struct TlCall {
    uint64_t id; TlKind kind;
    TlCall(TlKind k,std::initializer_list<uint64_t> fields={}) noexcept : id(timeline.next_call()),kind(k) {
        timeline.emit(id,kind,1,fields);
    }
    void end(std::initializer_list<uint64_t> fields={}) const noexcept { timeline.emit(id,kind,2,fields); }
    void point(std::initializer_list<uint64_t> fields) const noexcept { timeline.emit(id,kind,0,fields); }
};
template<typename F> auto tl_call(TlKind kind, F &&operation, std::initializer_list<uint64_t> fields={}) -> decltype(operation()) {
    TlCall call(kind,fields);
    const auto result=operation();
    call.end({tl_u32(result)});
    return result;
}
template<typename F> void tl_void(TlKind kind,F &&operation,std::initializer_list<uint64_t> fields={}) {
    TlCall call(kind,fields); operation(); call.end();
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included only by the independent executable, never by the production DLL. */
struct OpaqueStorage {
    static constexpr size_t guard = 64, limit = 1024 * 1024;
    unsigned char *base = nullptr;
    size_t size = 0;
    OpaqueStorage() = default;
    OpaqueStorage(const OpaqueStorage &) = delete;
    OpaqueStorage &operator=(const OpaqueStorage &) = delete;
    ~OpaqueStorage() { if (base) _aligned_free(base); }
    void allocate(size_t bytes, const char *kind) {
        require(!base && bytes && bytes <= limit, "opaque CalcPrivate size is nonzero and bounded");
        base = static_cast<unsigned char *>(_aligned_malloc(bytes + 2 * guard, guard));
        require(base != nullptr, "opaque private allocation");
        size = bytes;
        std::memset(base, 0xa5, guard);
        std::memset(data(), 0, size);
        std::memset(base + guard + size, 0xa5, guard);
        std::printf("OPAQUE kind=%s calc_bytes=%llu alignment=%u\n", kind,
            static_cast<unsigned long long>(size), static_cast<unsigned>(guard));
    }
    void *data() const { return base ? base + guard : nullptr; }
    bool intact() const {
        if (!base) return true;
        for (size_t i = 0; i < guard; ++i)
            if (base[i] != 0xa5 || base[guard + size + i] != 0xa5) return false;
        return true;
    }
};

/* The provider supplies observations; neither stored status nor enqueue is
 * completion. A queue is destroyed once, then its storage remains owned until
 * the entire session's safe teardown. */
struct BatchGate {
    D3DKMT_HANDLE context = 0;
    bool submitted = false, event_observed = false, destroy_called = false, destroyed = false;
    bool fault = false;
    void begin(D3DKMT_HANDLE value) { require(!context && value, "batch begins once with real context"); context = value; }
    void execute() { require(context && !submitted && !destroy_called, "batch Execute ordering"); submitted = true; }
    void observed(D3DKMT_HANDLE value) {
        require(submitted && value == context && !destroy_called, "event belongs to submitted batch context");
        event_observed = true;
    }
    void destroy_start() { require(context && !destroy_called, "typed queue destroy once"); destroy_called = true; }
    void destroy_finish(D3DKMT_HANDLE value, bool events_empty, bool status_ok) {
        require(destroy_called && value == context, "matching DestroyContext callback");
        destroyed = events_empty && status_ok; fault |= !destroyed;
    }
    bool readback_allowed() const { return submitted && event_observed && destroyed && !fault; }
    bool release_allowed() const { return destroyed && !fault && (!submitted || event_observed); }
};

template <typename F> HRESULT callback_boundary(std::atomic_uint &faults, F &&function) noexcept {
    try { return function(); }
    catch (const std::bad_alloc &) { ++faults; return E_OUTOFMEMORY; }
    catch (...) { ++faults; return E_FAIL; }
}

std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> text(32768);
    const DWORD n = GetModuleFileNameW(module, text.data(), static_cast<DWORD>(text.size()));
    require(n && n < text.size(), "loaded module path fits buffer");
    return {text.data(), n};
}
std::wstring canonical_path(const wchar_t *path) {
    require(path && std::wcslen(path) > 3 && ((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
        (path[0] == L'\\' && path[1] == L'\\')), "explicit absolute Windows path required");
    std::vector<wchar_t> text(32768);
    const DWORD n = GetFullPathNameW(path, static_cast<DWORD>(text.size()), text.data(), nullptr);
    require(n && n < text.size(), "canonical absolute path fits buffer");
    return {text.data(), n};
}
std::wstring basename(const std::wstring &path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}
WORD process_machine() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64) || defined(__x86_64__)
    return IMAGE_FILE_MACHINE_AMD64;
#else
    return IMAGE_FILE_MACHINE_I386;
#endif
}

struct PinnedFile {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::string hash;
    WORD machine = 0;
    PinnedFile() = default;
    PinnedFile(const PinnedFile &) = delete;
    PinnedFile &operator=(const PinnedFile &) = delete;
    ~PinnedFile() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    void open(const wchar_t *name, const wchar_t *expected_hash, bool image = true) {
        require(file == INVALID_HANDLE_VALUE, "pinned file opens once");
        path = canonical_path(name);
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        require(file != INVALID_HANDLE_VALUE, "explicit pinned file exists and denies mutation");
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE state = nullptr;
        struct HashCleanup {
            BCRYPT_ALG_HANDLE &algorithm; BCRYPT_HASH_HANDLE &state;
            ~HashCleanup() { if (state) BCryptDestroyHash(state); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
        } cleanup{algorithm, state};
        require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
            "SHA256 provider");
        require(BCryptCreateHash(algorithm, &state, nullptr, 0, nullptr, 0, 0) == 0, "SHA256 state");
        std::array<unsigned char, 65536> buffer{};
        DWORD count = 0;
        bool first = true;
        for (;;) {
            require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != FALSE,
                "read pinned file");
            if (!count) break;
            if (first && image) {
                require(count >= sizeof(IMAGE_DOS_HEADER), "PE DOS header present");
                IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, buffer.data(), sizeof(dos));
                require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0 &&
                    static_cast<size_t>(dos.e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) <= count, "bounded PE header");
                DWORD signature = 0; IMAGE_FILE_HEADER header{};
                std::memcpy(&signature, buffer.data() + dos.e_lfanew, sizeof(signature));
                std::memcpy(&header, buffer.data() + dos.e_lfanew + 4, sizeof(header));
                require(signature == IMAGE_NT_SIGNATURE && (header.Characteristics & IMAGE_FILE_DLL), "actual PE DLL required");
                machine = header.Machine;
                require(machine == process_machine(), "DLL architecture matches independent process");
            }
            first = false;
            require(BCryptHashData(state, buffer.data(), count, 0) == 0, "hash pinned file bytes");
        }
        require(!first, "pinned file nonempty");
        std::array<unsigned char, 32> digest{};
        require(BCryptFinishHash(state, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0, "finish SHA256");
        const char hex[] = "0123456789abcdef";
        for (auto byte : digest) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
        if (expected_hash) {
            require(std::wcslen(expected_hash) == 64, "expected SHA256 has exactly64 hex digits");
            for (size_t i = 0; i < 64; ++i) {
                const wchar_t c = expected_hash[i];
                require(((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')) && c == hash[i],
                    "pinned file SHA256 matches explicit expectation");
            }
        }
        std::printf("PIN path=%ls sha256=%s machine=%04x\n", path.c_str(), hash.c_str(), machine);
    }
    void verify_module(HMODULE module) const {
        require(module && !_wcsicmp(canonical_path(module_path(module).c_str()).c_str(), path.c_str()),
            "actual loaded module path equals pinned absolute file");
        HANDLE actual = CreateFileW(module_path(module).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(actual != INVALID_HANDLE_VALUE, "open actual module file identity");
        BY_HANDLE_FILE_INFORMATION expected{}, observed{};
        const bool have = GetFileInformationByHandle(file, &expected) && GetFileInformationByHandle(actual, &observed);
        CloseHandle(actual);
        require(have && expected.dwVolumeSerialNumber == observed.dwVolumeSerialNumber &&
            expected.nFileIndexHigh == observed.nFileIndexHigh && expected.nFileIndexLow == observed.nFileIndexLow &&
            expected.nFileSizeHigh == observed.nFileSizeHigh && expected.nFileSizeLow == observed.nFileSizeLow,
            "loaded module file identity equals held hashed file");
    }
    std::string small_text() const {
        LARGE_INTEGER bytes{}, zero{};
        require(GetFileSizeEx(file, &bytes) && bytes.QuadPart > 0 && bytes.QuadPart <= 65536,
            "ICD manifest bounded to64KiB");
        require(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE, "seek pinned manifest");
        std::string result(static_cast<size_t>(bytes.QuadPart), '\0');
        DWORD count = 0;
        require(ReadFile(file, result.data(), static_cast<DWORD>(result.size()), &count, nullptr) && count == result.size(),
            "read exact pinned manifest");
        return result;
    }
};

std::wstring manifest_library(const std::string &text) {
    /* Conservative manifest subset: one literal library_path key, one UTF-8
     * JSON string, no escaped key or Unicode escapes. The real Vulkan loader
     * still validates the whole JSON. Ambiguous input is rejected. */
    const std::string key = "\"library_path\"";
    size_t pos = text.find(key);
    require(pos != std::string::npos && text.find(key, pos + key.size()) == std::string::npos,
        "ICD manifest contains exactly one library_path");
    pos += key.size();
    auto whitespace = [&] { while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\r' ||
        text[pos] == '\n' || text[pos] == '\t')) ++pos; };
    whitespace(); require(pos < text.size() && text[pos++] == ':', "manifest library_path colon");
    whitespace(); require(pos < text.size() && text[pos++] == '"', "manifest library_path string");
    std::string value;
    bool closed = false;
    while (pos < text.size()) {
        char c = text[pos++];
        if (c == '"') { closed = true; break; }
        if (c == '\\') {
            require(pos < text.size(), "manifest escape complete"); c = text[pos++];
            require(c == '\\' || c == '/' || c == '"', "manifest path uses supported literal escape");
        }
        require(static_cast<unsigned char>(c) >= 32, "manifest path contains no control byte");
        value += c;
    }
    require(closed && !value.empty(), "manifest path string complete");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    require(count > 0, "manifest library path is UTF8");
    std::wstring result(count, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count) == count, "decode manifest library path");
    return result;
}

struct IcdPin {
    PinnedFile manifest, library;
    void configure(const wchar_t *manifest_path, const wchar_t *library_hash) {
        manifest.open(manifest_path, nullptr, false);
        auto path = manifest_library(manifest.small_text());
        if (!(path.size() > 2 && path[1] == L':') && path.compare(0, 2, L"\\\\")) {
            path = manifest.path.substr(0, manifest.path.find_last_of(L"\\/") + 1) + path;
        }
        library.open(path.c_str(), library_hash);
        require(!GetModuleHandleW(basename(library.path).c_str()), "private ICD basename not previously loaded");
        require(SetEnvironmentVariableW(L"VK_DRIVER_FILES", manifest.path.c_str()) &&
            SetEnvironmentVariableW(L"VK_ICD_FILENAMES", manifest.path.c_str()) &&
            SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", nullptr), "select only pinned private ICD manifest in this process");
    }
    void verify_loaded() const {
        library.verify_module(GetModuleHandleW(basename(library.path).c_str()));
        std::printf("ICD actual_path=%ls sha256=%s manifest_sha256=%s\n", library.path.c_str(),
            library.hash.c_str(), manifest.hash.c_str());
    }
};

struct DllApi {
    using Open = HRESULT (APIENTRY *)(D3D12DDIARG_OPENADAPTER *);
    using Tables = HRESULT (APIENTRY *)(D3D12DDI_DEVICE_FUNCS_CORE_0003 *,
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *, D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *);
    using Status = HRESULT (APIENTRY *)(D3D12DDI_HDEVICE);
    PinnedFile file;
    HMODULE module = nullptr;
    Open open = nullptr;
    Tables tables = nullptr;
    Status status = nullptr;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    bool executable_pointer(const void *pointer) const {
        if (!pointer || !module) return false;
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(pointer), &owner) || owner != module) return false;
        const auto base = reinterpret_cast<const unsigned char *>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const auto address = reinterpret_cast<uintptr_t>(pointer), start = reinterpret_cast<uintptr_t>(base);
        if (address < start || address - start >= nt->OptionalHeader.SizeOfImage) return false;
        const auto rva = address - start;
        const auto sections = IMAGE_FIRST_SECTION(nt);
        for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto &s = sections[i];
            if (rva >= s.VirtualAddress && rva - s.VirtualAddress < s.Misc.VirtualSize &&
                    (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) return true;
        }
        return false;
    }
    template <typename T> T export_pointer(const char *name) const {
        const auto value = GetProcAddress(module, name);
        if (!value) std::fprintf(stdout, "FAIL missing typed export: %s\n", name);
        require(executable_pointer(reinterpret_cast<const void *>(value)), "typed export belongs to selected DLL executable section");
        return reinterpret_cast<T>(value);
    }
    template <typename T> size_t audit_table(const T &table, const char *name) const {
        /* WDK table records here contain only function pointers. memcpy reads
         * representation without aliasing casts or calling an untyped pointer. */
        static_assert(sizeof(T) % sizeof(void *) == 0, "whole pointer table");
        size_t populated = 0;
        for (size_t i = 0; i < sizeof(T); i += sizeof(void *)) {
            const void *value = nullptr;
            std::memcpy(&value, reinterpret_cast<const unsigned char *>(&table) + i, sizeof(value));
            if (!value) continue;
            require(executable_pointer(value), "every populated DDI table pointer belongs to selected DLL executable section");
            ++populated;
        }
        std::printf("TABLE name=%s bytes=%llu populated=%llu all_owned=1\n", name,
            static_cast<unsigned long long>(sizeof(T)), static_cast<unsigned long long>(populated));
        return populated;
    }
    void load(const wchar_t *path, const wchar_t *hash) {
        file.open(path, hash);
        require(!GetModuleHandleW(basename(file.path).c_str()), "selected DLL basename is not already loaded");
        module = LoadLibraryExW(file.path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) std::fprintf(stdout, "LoadLibraryExW error=%lu\n", GetLastError());
        require(module != nullptr, "load exact independent core DLL");
        file.verify_module(module);
        open = export_pointer<Open>("OpenAdapter12");
        tables = export_pointer<Tables>("VioGpuD3D12BridgeGetTables");
        status = export_pointer<Status>("VioGpuD3D12BridgeStatus");
        check(tables(&device, &commands, &queues), "typed BridgeGetTables on actual DLL");
        audit_table(device, "device"); audit_table(commands, "command-list"); audit_table(queues, "queue");
    }
    /* Explicit only; owning session permits this only after typed drain and
     * complete callback/heap/context teardown. No destructor unloads the DLL. */
    void unload() {
        require(module != nullptr, "core module still retained until explicit teardown");
        require(FreeLibrary(module) != FALSE, "release exact core module after cleanup");
        module = nullptr; open = nullptr; tables = nullptr; status = nullptr;
    }
};
