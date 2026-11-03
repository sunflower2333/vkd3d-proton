// Count raw input events per device (mouse / digitizer / touch) for N seconds.
// Usage: rawinput-log.exe [seconds]
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
struct Stat { unsigned events = 0, absolute = 0, relative = 0, buttons = 0; std::string name; DWORD type = 0; USHORT page = 0, usage = 0; };
static std::map<HANDLE, Stat> stats;
static unsigned cursorChanges = 0;
static HCURSOR lastCursor = nullptr;
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        BYTE buf[4096];
        if (size <= sizeof(buf) && GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            auto *ri = (RAWINPUT *)buf;
            auto &s = stats[ri->header.hDevice];
            s.type = ri->header.dwType; ++s.events;
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) ++s.absolute; else ++s.relative;
                if (ri->data.mouse.usButtonFlags) ++s.buttons;
            }
        }
    }
    return DefWindowProcW(w, m, wp, lp);
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"rawlog";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"rawlog", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE devs[4] = {
        {0x01, 0x02, RIDEV_INPUTSINK, hwnd},  // mouse
        {0x0D, 0x01, RIDEV_INPUTSINK, hwnd},  // digitizer
        {0x0D, 0x02, RIDEV_INPUTSINK, hwnd},  // pen
        {0x0D, 0x04, RIDEV_INPUTSINK, hwnd},  // touch screen
    };
    for (auto &d : devs) if (!RegisterRawInputDevices(&d, 1, sizeof(d))) std::printf("register %x/%x failed %lu\n", d.usUsagePage, d.usUsage, GetLastError());
    const DWORD end = GetTickCount() + seconds * 1000;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        CURSORINFO ci{sizeof(ci)};
        if (GetCursorInfo(&ci) && ci.hCursor != lastCursor) { lastCursor = ci.hCursor; ++cursorChanges; }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    for (auto &kv : stats) {
        UINT n = 0; GetRawInputDeviceInfoA(kv.first, RIDI_DEVICENAME, nullptr, &n);
        std::string name(n + 1, 0); if (n) GetRawInputDeviceInfoA(kv.first, RIDI_DEVICENAME, name.data(), &n);
        RID_DEVICE_INFO info{sizeof(info)}; UINT isz = sizeof(info); GetRawInputDeviceInfoW(kv.first, RIDI_DEVICEINFO, &info, &isz);
        std::printf("device %p type=%lu events=%u abs=%u rel=%u buttons=%u hid=%04x/%04x %s\n", kv.first, kv.second.type,
            kv.second.events, kv.second.absolute, kv.second.relative, kv.second.buttons,
            info.dwType == RIM_TYPEHID ? info.hid.usUsagePage : 0, info.dwType == RIM_TYPEHID ? info.hid.usUsage : 0, name.c_str());
    }
    CURSORINFO ci{sizeof(ci)}; GetCursorInfo(&ci);
    std::printf("cursor changes=%u flags=%lx (1=showing, 2=suppressed)\n", cursorChanges, ci.flags);
    return 0;
}
