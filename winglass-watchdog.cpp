#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

// This must match the marker set by winglass.exe. It proves that a journal entry
// still belongs to the original target HWND, rather than a reused handle.
static const wchar_t* kManagedProperty = L"WinGlass.ManagedTarget.v1";
static constexpr UINT_PTR kManagedMarkerValue = 0x57474C31; // "WGL1"

HANDLE ManagedMarker() {
    return reinterpret_cast<HANDLE>(kManagedMarkerValue);
}

void RestoreWindowStyle(HWND hwnd, LONG_PTR originalExStyle, bool hadLayered,
                        BYTE originalAlpha, COLORREF originalColorKey, DWORD originalLayerFlags) {
    if (!IsWindow(hwnd)) return;
    if (hadLayered) {
        const DWORD flags = originalLayerFlags ? originalLayerFlags : LWA_ALPHA;
        SetLayeredWindowAttributes(hwnd, originalColorKey, originalAlpha, flags);
    } else {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalExStyle);
    }
    RemovePropW(hwnd, kManagedProperty);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void RestoreJournal(const std::wstring& path) {
    std::wifstream input(path);
    if (!input) return;

    std::wstring line;
    while (std::getline(input, line)) {
        unsigned long long hwndValue = 0;
        unsigned long pid = 0;
        long long originalExStyle = 0;
        int hadLayered = 0;
        unsigned alpha = 255, colorKey = 0, layerFlags = 0;
        int markerRequired = 0;
        std::wistringstream record(line);
        if (!(record >> hwndValue >> pid >> originalExStyle >> hadLayered >> alpha >> colorKey >> layerFlags)) continue;
        // Seven-field journals from previous versions remain recoverable.
        record >> markerRequired;

        const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndValue));
        DWORD currentPid = 0;
        GetWindowThreadProcessId(hwnd, &currentPid);
        const bool markerMatches = markerRequired == 0 || GetPropW(hwnd, kManagedProperty) == ManagedMarker();
        if (IsWindow(hwnd) && currentPid == pid && markerMatches) {
            RestoreWindowStyle(hwnd, static_cast<LONG_PTR>(originalExStyle), hadLayered != 0,
                               static_cast<BYTE>(alpha), static_cast<COLORREF>(colorKey),
                               static_cast<DWORD>(layerFlags));
        }
    }
    input.close();
    DeleteFileW(path.c_str());
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 4 || wcscmp(argv[1], L"--watchdog") != 0) {
        if (argv) LocalFree(argv);
        return 2;
    }

    // The inherited handle refers to this exact parent process, avoiding PID
    // reuse races. Do not restore anything when the handle is malformed.
    const auto inheritedValue = static_cast<UINT_PTR>(_wcstoui64(argv[2], nullptr, 10));
    HANDLE parent = reinterpret_cast<HANDLE>(inheritedValue);
    const std::wstring journalPath = argv[3];
    if (!parent || parent == INVALID_HANDLE_VALUE) {
        LocalFree(argv);
        return 3;
    }

    const DWORD waitResult = WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
    if (waitResult == WAIT_OBJECT_0) RestoreJournal(journalPath);
    LocalFree(argv);
    return waitResult == WAIT_OBJECT_0 ? 0 : 4;
}
