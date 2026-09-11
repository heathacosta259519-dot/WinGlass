#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <oleauto.h>

#include "winglass-resource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

struct Color { uint8_t r = 44, g = 62, b = 88; };

struct Rule {
    bool enabled = true;
    double activeOpacity = 0.90;
    double inactiveOpacity = 0.82;
    bool activeAcrylic = true;
    bool inactiveAcrylic = true;
    // Retained only so old configuration files still parse successfully.
    // System Acrylic owns its blur radius and exposes no supported strength API.
    int activeBlurStrength = 12;
    int inactiveBlurStrength = 12;
    double activeGlassOpacity = 0.98;
    double inactiveGlassOpacity = 0.96;
    Color activeTint{44, 62, 88};
    Color inactiveTint{31, 42, 60};
    double activeTintStrength = 0.34;
    double inactiveTintStrength = 0.30;
    int transitionMs = 180;
};

struct Config {
    Rule global;
    std::unordered_map<std::wstring, Rule> apps;
    std::unordered_map<std::wstring, bool> blacklist;
    std::unordered_map<std::wstring, bool> blacklistClasses;
    FILETIME writeTime{};
    bool loaded = false;
};

struct WindowState {
    HWND target = nullptr;
    HWND backdrop = nullptr;
    // The tint surface is deliberately separate from the compositor backdrop.
    // This matches window-blur-fx: DWM owns blur, while WinGlass owns the color
    // wash in a small layered window above the blur surface.
    HWND tint = nullptr;
    LONG_PTR originalExStyle = 0;
    bool changedStyle = false;
    bool hadLayeredStyle = false;
    BYTE originalAlpha = 255;
    COLORREF originalColorKey = 0;
    DWORD originalLayerFlags = 0;
    DWORD processId = 0;
    // SetProp gives the watchdog an identity token that disappears when the
    // original HWND is destroyed. PID alone cannot detect HWND reuse inside
    // one long-running process.
    bool recoveryMarkerSet = false;
    uint64_t ruleRevision = 0;
    bool wasActive = false;
    bool backdropConfigured = false;
    // AccentPolicy is expensive and can briefly force a DWM recomposition. The
    // requested material is tracked separately so a focus change that keeps
    // glass enabled never sends the policy a second time.
    bool backdropRequestKnown = false;
    bool backdropRequested = false;
    Color tintColor{};
    BYTE tintAlpha = 0;
    Color fromTintColor{};
    Color toTintColor{};
    BYTE fromTintAlpha = 0;
    BYTE toTintAlpha = 0;
    Color lastTintColor{};
    BYTE lastTintAlpha = 0;
    bool tintApplied = false;
    bool visualInitialized = false;
    bool applied = false;
    double currentOpacity = 1.0;
    double fromOpacity = 1.0;
    double toOpacity = 1.0;
    ULONGLONG transitionStart = 0;
    BYTE lastAppliedAlpha = 255;
    bool hasPosition = false;
    bool forcePlacement = true;
    // Occlusion and minimization are reversible presentation states. Keeping
    // the target's original effect state avoids a costly style/backdrop
    // teardown while an Alt+Tab surface temporarily covers the desktop.
    bool suspended = false;
    ULONGLONG lastGeometrySample = 0;
    ULONGLONG lastPlacementValidation = 0;
    ULONGLONG backdropRetryAfter = 0;
    int lastX = 0;
    int lastY = 0;
    int lastW = 0;
    int lastH = 0;
    Rule rule{};
};

static const wchar_t* kBackdropClass = L"WinGlassBackdropWindow";
static const wchar_t* kTrayClass = L"WinGlassTrayWindow";
static const wchar_t* kManagedProperty = L"WinGlass.ManagedTarget.v1";
static constexpr UINT_PTR kManagedMarkerValue = 0x57474C31; // "WGL1"
static constexpr UINT kTrayMessage = WM_APP + 42;
static constexpr UINT kTrayOpenConfig = 41001;
static constexpr UINT kTrayReload = 41002;
static constexpr UINT kTrayStartup = 41003;
static constexpr UINT kTrayExit = 41004;
static constexpr UINT kTrayEditor = 41005;
static constexpr size_t kMaxTrackedWindows = 12;
// Keep the Run-key location and value name centralized so reads, writes, and
// post-operation verification always refer to the same startup entry.
static const wchar_t* kStartupRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kStartupValueName = L"WinGlass";
static Config g_config;
static std::unordered_map<HWND, WindowState> g_windows;
static std::wstring g_configPath;
static std::wstring g_statePath;
static std::wstring g_diagnosticsPath;
static std::wstring g_startupStatePath;
static HINSTANCE g_instance = nullptr;
static HWND g_trayWindow = nullptr;
static bool g_trayIconAdded = false;
static DWORD g_lastTrayError = ERROR_SUCCESS;
static ULONGLONG g_lastEmptySweepDiagnostic = 0;
static UINT g_taskbarCreatedMessage = 0;
static FILETIME g_configWriteTime{};
static bool g_cleanedUp = false;
// Existing HWNDs only need their process rule recalculated after a successful
// config reload. This removes repeated OpenProcess calls from periodic sweeps.
static uint64_t g_configRevision = 1;
static bool g_sweepRequested = false;
// Set while any opacity or tint transition is in flight. UpdateTrackedWindows
// uses this to synchronize one batch of layered-window updates to DWM.
static bool g_visualAnimationActive = false;
// A sandboxed launcher can draw on the interactive desktop without inheriting
// that desktop user's HKCU. This cache preserves the last confirmed setting
// for the tray label when HKCU cannot be queried across that identity boundary.
static bool g_startupStateKnown = false;
static bool g_startupStateEnabled = false;

void CleanupAllWindows();

std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

void WriteDiagnostic(const std::wstring& message) {
    if (g_diagnosticsPath.empty()) return;
    // Diagnostics are intentionally event-driven, never emitted from the
    // 8 ms render path. They make a future invisible-desktop failure
    // actionable without contributing measurable steady-state overhead.
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wofstream output(g_diagnosticsPath, std::ios::app);
    if (!output) return;
    output << time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
           << time.wHour << L':' << time.wMinute << L':' << time.wSecond
           << L"  " << message << L'\n';
}

BOOL CALLBACK CountVisibleTopLevelWindow(HWND hwnd, LPARAM value) {
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        *reinterpret_cast<bool*>(value) = true;
        return FALSE;
    }
    return TRUE;
}

bool CanSeeInteractiveDesktop() {
    // On an isolated desktop, both APIs below see no shell window even when
    // Explorer is alive in the same session. The visible top-level count is a
    // practical, permission-free test for whether effects and tray icons can
    // reach the user's desktop.
    if (GetForegroundWindow()) return true;
    bool hasVisibleWindow = false;
    EnumWindows(&CountVisibleTopLevelWindow, reinterpret_cast<LPARAM>(&hasVisibleWindow));
    return hasVisibleWindow;
}

bool IsExplorerInCurrentSession(DWORD processId, DWORD sessionId) {
    DWORD candidateSession = 0;
    if (!ProcessIdToSessionId(processId, &candidateSession) || candidateSession != sessionId) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const bool read = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!read) return false;
    std::wstring name = Lower(path);
    const size_t slash = name.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? name : name.substr(slash + 1)) == L"explorer.exe";
}

bool RelaunchThroughShellAutomation(const std::wstring& executable) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool mustUninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        WriteDiagnostic(L"Shell Automation initialization failed, error=" + std::to_wstring(static_cast<unsigned long>(initialized)));
        return false;
    }

    CLSID shellClass{};
    IDispatch* shell = nullptr;
    HRESULT result = CLSIDFromProgID(L"Shell.Application", &shellClass);
    if (SUCCEEDED(result)) result = CoCreateInstance(shellClass, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                                                      reinterpret_cast<void**>(&shell));
    if (FAILED(result) || !shell) {
        if (mustUninitialize) CoUninitialize();
        WriteDiagnostic(L"Shell Automation server unavailable, error=" + std::to_wstring(static_cast<unsigned long>(result)));
        return false;
    }

    OLECHAR* methodName = const_cast<OLECHAR*>(L"ShellExecute");
    DISPID method = DISPID_UNKNOWN;
    result = shell->GetIDsOfNames(IID_NULL, &methodName, 1, LOCALE_USER_DEFAULT, &method);
    const size_t slash = executable.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? L"" : executable.substr(0, slash);
    VARIANT arguments[5]{};
    for (auto& argument : arguments) VariantInit(&argument);
    // IDispatch receives arguments in reverse order: show, operation,
    // directory, parameters, then executable path.
    arguments[0].vt = VT_I4; arguments[0].lVal = SW_SHOWNORMAL;
    arguments[1].vt = VT_BSTR; arguments[1].bstrVal = SysAllocString(L"open");
    arguments[2].vt = VT_BSTR; arguments[2].bstrVal = SysAllocString(directory.c_str());
    arguments[3].vt = VT_BSTR; arguments[3].bstrVal = SysAllocString(L"--interactive-relaunch");
    arguments[4].vt = VT_BSTR; arguments[4].bstrVal = SysAllocString(executable.c_str());
    DISPPARAMS parameters{arguments, nullptr, static_cast<UINT>(std::size(arguments)), 0};
    EXCEPINFO exception{};
    UINT argumentError = 0;
    if (SUCCEEDED(result)) result = shell->Invoke(method, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                                                   &parameters, nullptr, &exception, &argumentError);
    for (auto& argument : arguments) VariantClear(&argument);
    shell->Release();
    if (mustUninitialize) CoUninitialize();
    if (FAILED(result)) {
        WriteDiagnostic(L"Shell Automation launch failed, error=" + std::to_wstring(static_cast<unsigned long>(result)));
        return false;
    }
    return true;
}

bool RelaunchOnDefaultDesktop(const std::wstring& executable) {
    std::wstring command = L"\"" + executable + L"\" --interactive-relaunch";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    // A child normally inherits the launcher's hidden/isolated desktop. Name
    // the logged-in user's normal desktop explicitly so the child can enumerate
    // top-level windows and talk to the notification-area Explorer instance.
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &child)) {
        WriteDiagnostic(L"default-desktop relaunch failed, error=" + std::to_wstring(GetLastError()));
        return false;
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
}

bool RelaunchOnExplorerDesktop() {
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return false;

    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring executable = module;
    std::vector<DWORD> processIds(1024);
    DWORD bytesNeeded = 0;
    if (!EnumProcesses(processIds.data(), static_cast<DWORD>(processIds.size() * sizeof(DWORD)), &bytesNeeded)) return false;
    const size_t count = std::min(processIds.size(), static_cast<size_t>(bytesNeeded / sizeof(DWORD)));
    for (size_t index = 0; index < count; ++index) {
        const DWORD explorerId = processIds[index];
        if (!explorerId || !IsExplorerInCurrentSession(explorerId, sessionId)) continue;

        HANDLE explorer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorerId);
        HANDLE explorerToken = nullptr;
        HANDLE primaryToken = nullptr;
        if (!explorer || !OpenProcessToken(explorer, TOKEN_QUERY | TOKEN_DUPLICATE, &explorerToken) ||
            !DuplicateTokenEx(explorerToken, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY,
                              nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
            if (primaryToken) CloseHandle(primaryToken);
            if (explorerToken) CloseHandle(explorerToken);
            if (explorer) CloseHandle(explorer);
            continue;
        }

        std::wstring command = L"\"" + executable + L"\" --interactive-relaunch";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
        PROCESS_INFORMATION child{};
        bool launched = CreateProcessWithTokenW(primaryToken, 0, executable.c_str(), mutableCommand.data(),
                                                 CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                                                 &startup, &child) != FALSE;
        DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
        if (!launched) {
            // Some restricted launchers lack SeImpersonatePrivilege but can
            // still create a process from the same user's Explorer token.
            // Try the complementary primary-token API before giving up.
            launched = CreateProcessAsUserW(primaryToken, executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                                             FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                                             &startup, &child) != FALSE;
            launchError = launched ? ERROR_SUCCESS : GetLastError();
        }
        if (launched) {
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            CloseHandle(primaryToken);
            CloseHandle(explorerToken);
            CloseHandle(explorer);
            return true;
        }
        WriteDiagnostic(L"Explorer-token desktop relaunch failed, error=" + std::to_wstring(launchError));
        CloseHandle(primaryToken);
        CloseHandle(explorerToken);
        CloseHandle(explorer);
    }
    if (RelaunchThroughShellAutomation(executable)) return true;
    return RelaunchOnDefaultDesktop(executable);
}

bool ParseBool(const std::wstring& value, bool fallback) {
    const auto v = Lower(Trim(value));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return fallback;
}

double ParseDouble(const std::wstring& value, double fallback, double lo, double hi) {
    try { return std::clamp(std::stod(Trim(value)), lo, hi); }
    catch (...) { return fallback; }
}

int ParseInt(const std::wstring& value, int fallback, int lo, int hi) {
    try { return std::clamp(std::stoi(Trim(value)), lo, hi); }
    catch (...) { return fallback; }
}

Color ParseColor(const std::wstring& value, Color fallback) {
    auto v = Trim(value);
    if (!v.empty() && v[0] == L'#') v.erase(v.begin());
    if (v.size() != 6) return fallback;
    try {
        const auto n = std::stoul(v, nullptr, 16);
        return Color{static_cast<uint8_t>((n >> 16) & 0xff), static_cast<uint8_t>((n >> 8) & 0xff), static_cast<uint8_t>(n & 0xff)};
    } catch (...) { return fallback; }
}

void SetField(Rule& rule, const std::wstring& key, const std::wstring& value) {
    const auto k = Lower(Trim(key));
    if (k == L"enabled") rule.enabled = ParseBool(value, rule.enabled);
    else if (k == L"active_opacity") rule.activeOpacity = ParseDouble(value, rule.activeOpacity, 0.05, 1.0);
    else if (k == L"inactive_opacity") rule.inactiveOpacity = ParseDouble(value, rule.inactiveOpacity, 0.05, 1.0);
    else if (k == L"active_acrylic") rule.activeAcrylic = ParseBool(value, rule.activeAcrylic);
    else if (k == L"inactive_acrylic") rule.inactiveAcrylic = ParseBool(value, rule.inactiveAcrylic);
    else if (k == L"active_blur_strength" || k == L"active_blur_radius") rule.activeBlurStrength = ParseInt(value, rule.activeBlurStrength, 0, 64);
    else if (k == L"inactive_blur_strength" || k == L"inactive_blur_radius") rule.inactiveBlurStrength = ParseInt(value, rule.inactiveBlurStrength, 0, 64);
    else if (k == L"active_glass_opacity") rule.activeGlassOpacity = ParseDouble(value, rule.activeGlassOpacity, 0.05, 1.0);
    else if (k == L"inactive_glass_opacity") rule.inactiveGlassOpacity = ParseDouble(value, rule.inactiveGlassOpacity, 0.05, 1.0);
    else if (k == L"active_tint_color") rule.activeTint = ParseColor(value, rule.activeTint);
    else if (k == L"inactive_tint_color") rule.inactiveTint = ParseColor(value, rule.inactiveTint);
    else if (k == L"active_tint_strength") rule.activeTintStrength = ParseDouble(value, rule.activeTintStrength, 0.0, 1.0);
    else if (k == L"inactive_tint_strength") rule.inactiveTintStrength = ParseDouble(value, rule.inactiveTintStrength, 0.0, 1.0);
    else if (k == L"transition_ms") rule.transitionMs = ParseInt(value, rule.transitionMs, 0, 5000);
}

bool SameFileTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

bool LoadIniConfig(Config& result, const std::wstring& path) {
    std::wifstream input(path);
    if (!input) return false;
    Config next;
    std::wstring section = L"global";
    std::wstring appName;
    std::wstring line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            appName.clear();
            if (section.rfind(L"app:", 0) == 0) { appName = Lower(Trim(section.substr(4))); next.apps[appName] = next.global; }
            continue;
        }
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        if (section == L"global") SetField(next.global, key, value);
        else if (section.rfind(L"app:", 0) == 0 && !appName.empty()) SetField(next.apps[appName], key, value);
        else if (section == L"blacklist") next.blacklist[Lower(key)] = ParseBool(value, true);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) next.writeTime = data.ftLastWriteTime;
    next.loaded = true;
    result = std::move(next);
    return true;
}

std::wstring StripYamlComment(const std::wstring& line) {
    bool quoted = false; wchar_t quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if ((c == L'\'' || c == L'"') && (!quoted || c == quote)) { quoted = !quoted; quote = quoted ? c : 0; }
        else if (c == L'#' && !quoted && (i == 0 || iswspace(line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

std::wstring UnquoteYaml(std::wstring value) {
    value = Trim(value);
    if (value.size() >= 2 && ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\''))) return value.substr(1, value.size() - 2);
    return value;
}

double ParseFlexibleOpacity(const std::wstring& value, double fallback) {
    try {
        double n = std::stod(UnquoteYaml(value));
        if (n > 1.0) n = n <= 100.0 ? n / 100.0 : n / 255.0;
        return std::clamp(n, 0.0, 1.0);
    } catch (...) { return fallback; }
}

void SetYamlStateField(Rule& rule, bool focused, const std::wstring& key, const std::wstring& rawValue) {
    const auto k = Lower(Trim(key));
    const auto value = UnquoteYaml(rawValue);
    if (k == L"enabled") rule.enabled = ParseBool(value, rule.enabled);
    else if (k == L"target_opacity" || k == L"opacity") { if (focused) rule.activeOpacity = ParseFlexibleOpacity(value, rule.activeOpacity); else rule.inactiveOpacity = ParseFlexibleOpacity(value, rule.inactiveOpacity); }
    else if (k == L"enable_glass" || k == L"acrylic") { if (focused) rule.activeAcrylic = ParseBool(value, rule.activeAcrylic); else rule.inactiveAcrylic = ParseBool(value, rule.inactiveAcrylic); }
    else if (k == L"blur_strength" || k == L"blur_radius") { if (focused) rule.activeBlurStrength = ParseInt(value, rule.activeBlurStrength, 0, 64); else rule.inactiveBlurStrength = ParseInt(value, rule.inactiveBlurStrength, 0, 64); }
    else if (k == L"glass_color" || k == L"tint_color") { if (focused) rule.activeTint = ParseColor(value, rule.activeTint); else rule.inactiveTint = ParseColor(value, rule.inactiveTint); }
    else if (k == L"glass_opacity") { if (focused) rule.activeGlassOpacity = ParseFlexibleOpacity(value, rule.activeGlassOpacity); else rule.inactiveGlassOpacity = ParseFlexibleOpacity(value, rule.inactiveGlassOpacity); }
    else if (k == L"tint_opacity" || k == L"tint_strength") { if (focused) rule.activeTintStrength = ParseFlexibleOpacity(value, rule.activeTintStrength); else rule.inactiveTintStrength = ParseFlexibleOpacity(value, rule.inactiveTintStrength); }
    else if (k == L"animation_duration_ms" || k == L"transition_ms") rule.transitionMs = ParseInt(value, rule.transitionMs, 0, 5000);
}

bool LoadYamlConfig(Config& result, const std::wstring& path) {
    std::wifstream input(path);
    if (!input) return false;
    Config next;
    std::wstring section, state, appName;
    Rule appRule = next.global;
    bool appOpen = false;
    auto commitApp = [&]() { if (appOpen && !appName.empty()) next.apps[Lower(appName)] = appRule; appOpen = false; appName.clear(); state.clear(); };
    std::wstring line;
    while (std::getline(input, line)) {
        line = StripYamlComment(line);
        if (Trim(line).empty()) continue;
        const size_t indent = line.find_first_not_of(L" \t");
        const std::wstring body = Trim(line);
        if (indent == std::wstring::npos) continue;
        if (indent == 0 && body.back() == L':') {
            commitApp(); section = Lower(Trim(body.substr(0, body.size() - 1))); state.clear(); continue;
        }
        std::wstring item = body;
        if (!item.empty() && item.front() == L'-') item = Trim(item.substr(1));
        const auto colon = item.find(L':');
        const std::wstring key = colon == std::wstring::npos ? Lower(item) : Lower(Trim(item.substr(0, colon)));
        const std::wstring value = colon == std::wstring::npos ? L"" : Trim(item.substr(colon + 1));
        if (section == L"blacklist") {
            if (key == L"process" && !value.empty()) next.blacklist[Lower(UnquoteYaml(value))] = true;
            else if (key == L"class_name" && !value.empty()) next.blacklistClasses[Lower(UnquoteYaml(value))] = true;
            continue;
        }
        if (section == L"global") {
            if (key == L"focused" || key == L"unfocused") state = key;
            else if (key == L"type") state = Lower(UnquoteYaml(value));
            else if (key == L"config") continue;
            else if (key == L"enabled" && state.empty()) next.global.enabled = ParseBool(UnquoteYaml(value), next.global.enabled);
            else if (state == L"focused" || state == L"unfocused") SetYamlStateField(next.global, state == L"focused", key, value);
            continue;
        }
        if (section == L"applications") {
            if (key == L"match") { commitApp(); appOpen = true; appRule = next.global; state = L"match"; continue; }
            if (key == L"process" && !value.empty() && (state == L"match" || !appOpen)) { if (!appOpen) { appOpen = true; appRule = next.global; } appName = UnquoteYaml(value); state = L"match"; continue; }
            if (key == L"focused" || key == L"unfocused") { state = key; continue; }
            if (key == L"type") { state = Lower(UnquoteYaml(value)); continue; }
            if (key == L"config" || key == L"rules") continue;
            if (appOpen && (state == L"focused" || state == L"unfocused")) SetYamlStateField(appRule, state == L"focused", key, value);
        }
    }
    commitApp();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) next.writeTime = data.ftLastWriteTime;
    next.loaded = true;
    result = std::move(next);
    return true;
}

bool LoadConfig(Config& result, const std::wstring& path) {
    const auto lowerPath = Lower(path);
    return lowerPath.size() >= 5 && lowerPath.substr(lowerPath.size() - 5) == L".yaml" ? LoadYamlConfig(result, path) : LoadIniConfig(result, path);
}

HANDLE ManagedMarker() {
    return reinterpret_cast<HANDLE>(kManagedMarkerValue);
}

void RestoreWindowStyle(HWND hwnd, LONG_PTR originalExStyle, bool hadLayered, BYTE originalAlpha, COLORREF originalColorKey, DWORD originalLayerFlags) {
    if (!IsWindow(hwnd)) return;
    if (hadLayered) {
        const DWORD flags = originalLayerFlags ? originalLayerFlags : LWA_ALPHA;
        SetLayeredWindowAttributes(hwnd, originalColorKey, originalAlpha, flags);
    } else {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalExStyle);
    }
    // Remove the cross-process recovery token only after restoring the target.
    // A destroyed HWND drops properties automatically, so a reused HWND never
    // inherits this marker.
    RemovePropW(hwnd, kManagedProperty);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
        // Old seven-field journals remain readable. New records require the
        // marker when SetProp succeeded at application time.
        record >> markerRequired;
        const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndValue));
        DWORD currentPid = 0; GetWindowThreadProcessId(hwnd, &currentPid);
        const bool markerMatches = markerRequired == 0 || GetPropW(hwnd, kManagedProperty) == ManagedMarker();
        if (currentPid == pid && IsWindow(hwnd) && markerMatches) {
            RestoreWindowStyle(hwnd, static_cast<LONG_PTR>(originalExStyle), hadLayered != 0,
                               static_cast<BYTE>(alpha), static_cast<COLORREF>(colorKey),
                               static_cast<DWORD>(layerFlags));
        }
    }
    input.close(); DeleteFileW(path.c_str());
}

void PersistJournal() {
    if (g_statePath.empty()) return;
    const std::wstring tempPath = g_statePath + L".tmp";
    if (g_windows.empty()) { DeleteFileW(g_statePath.c_str()); DeleteFileW(tempPath.c_str()); return; }
    std::wofstream output(tempPath, std::ios::trunc);
    if (!output) return;
    for (const auto& [hwnd, state] : g_windows) {
        if (!IsWindow(hwnd) || !state.processId) continue;
        output << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)) << L' '
               << state.processId << L' ' << static_cast<long long>(state.originalExStyle) << L' '
               << (state.hadLayeredStyle ? 1 : 0) << L' ' << static_cast<unsigned>(state.originalAlpha) << L' '
               << static_cast<unsigned>(state.originalColorKey) << L' ' << state.originalLayerFlags << L' '
               << (state.recoveryMarkerSet ? 1 : 0) << L'\n';
    }
    output.flush();
    const bool writeSucceeded = output.good();
    output.close();
    if (writeSucceeded) {
        MoveFileExW(tempPath.c_str(), g_statePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
        // Never replace a valid recovery journal with a partial write.
        DeleteFileW(tempPath.c_str());
    }
}

void StartWatchdog() {
    wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring watchdog = module;
    const size_t slash = watchdog.find_last_of(L"\\/");
    watchdog = (slash == std::wstring::npos ? L"" : watchdog.substr(0, slash + 1)) + L"winglass-watchdog.exe";
    if (GetFileAttributesW(watchdog.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    HANDLE parentHandle = nullptr;
    // Pass a real inheritable process handle instead of a PID. The kernel
    // object remains bound to this exact process even if its PID is reused.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parentHandle,
                         SYNCHRONIZE, TRUE, 0)) return;
    std::wstring command = L"\"" + watchdog + L"\" --watchdog " +
        std::to_wstring(reinterpret_cast<UINT_PTR>(parentHandle)) + L" \"" + g_statePath + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (CreateProcessW(watchdog.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    CloseHandle(parentHandle);
}

bool ReloadConfig() {
    Config fresh;
    if (!LoadConfig(fresh, g_configPath)) return false;
    g_config = std::move(fresh);
    g_configWriteTime = g_config.writeTime;
    ++g_configRevision;
    // ApplyWindow compares the actual requested material on the next update.
    // Do not blindly reapply AccentPolicy here: that visibly flashes a window
    // even if the reload changed only tint or opacity.
    for (auto& [_, state] : g_windows) state.forcePlacement = true;
    g_sweepRequested = true;
    return true;
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    const bool enabled = RegQueryValueExW(key, kStartupValueName, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
                         (type == REG_SZ || type == REG_EXPAND_SZ);
    RegCloseKey(key);
    return enabled;
}

void LoadStartupStateCache() {
    std::wifstream input(g_startupStatePath);
    std::wstring value;
    if (input >> value) {
        g_startupStateKnown = true;
        g_startupStateEnabled = value == L"enabled";
    }
}

void PersistStartupStateCache(bool enabled) {
    g_startupStateKnown = true;
    g_startupStateEnabled = enabled;
    if (!enabled) {
        DeleteFileW(g_startupStatePath.c_str());
        return;
    }
    // The cache is only a UI-state bridge for a restricted launcher. The
    // elevated helper remains the authority for the actual Run registration.
    std::wofstream output(g_startupStatePath, std::ios::trunc);
    if (output) output << L"enabled\n";
}

bool StartupEnabledForMenu() {
    if (IsStartupEnabled()) return true;
    return g_startupStateKnown && g_startupStateEnabled;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key = nullptr; DWORD disposition = 0;
    const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, kStartupRunKey, 0, nullptr, 0,
                                           KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition);
    if (opened != ERROR_SUCCESS) {
        WriteDiagnostic(L"opening startup registry key failed, error=" + std::to_wstring(opened));
        return false;
    }
    bool ok = false;
    if (enabled) {
        wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
        const std::wstring command = L"\"" + std::wstring(module) + L"\"";
        const LSTATUS written = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                                                reinterpret_cast<const BYTE*>(command.c_str()),
                                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        ok = written == ERROR_SUCCESS && IsStartupEnabled();
        if (!ok) WriteDiagnostic(L"enabling startup failed, error=" + std::to_wstring(written));
    } else {
        const LSTATUS removed = RegDeleteValueW(key, kStartupValueName);
        // Successful API calls are not enough here: verify that the exact
        // current-user Run value is gone before reporting a successful toggle.
        ok = (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND) && !IsStartupEnabled();
        if (!ok) WriteDiagnostic(L"disabling startup failed, error=" + std::to_wstring(removed));
    }
    RegCloseKey(key);
    return ok;
}

bool SetStartupEnabledElevated(bool enabled) {
    wchar_t module[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, module, MAX_PATH)) return false;
    // The helper accepts only these two fixed arguments and exits before the
    // normal desktop, window-effect, and single-instance startup paths run.
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = enabled ? L"--set-startup=enable" : L"--set-startup=disable";
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        WriteDiagnostic(L"elevated startup helper could not start, error=" + std::to_wstring(GetLastError()));
        return false;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (exitCode != ERROR_SUCCESS) {
        WriteDiagnostic(L"elevated startup helper failed, exit=" + std::to_wstring(exitCode));
        return false;
    }
    PersistStartupStateCache(enabled);
    WriteDiagnostic(L"startup registration changed by elevated helper");
    return true;
}

NOTIFYICONDATAW MakeTrayIconData() {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon); icon.hWnd = g_trayWindow; icon.uID = 1;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION); // never end up with no icon at all
    wcscpy_s(icon.szTip, L"WinGlass \x5168\x5c40\x78e8\x7802\x73bb\x7483");
    return icon;
}

bool AddTrayIcon() {
    if (!g_trayWindow) return false;
    auto icon = MakeTrayIconData();
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
        g_trayIconAdded = false;
        const DWORD error = GetLastError();
        if (error != g_lastTrayError) {
            WriteDiagnostic(L"tray registration failed, error=" + std::to_wstring(error));
            g_lastTrayError = error;
        }
        return false;
    }
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    if (g_lastTrayError != ERROR_SUCCESS) WriteDiagnostic(L"tray registration recovered");
    g_lastTrayError = ERROR_SUCCESS;
    g_trayIconAdded = true;
    return true;
}

void DestroyTrayIcon() {
    if (!g_trayWindow) return;
    if (g_trayIconAdded) {
        auto icon = MakeTrayIconData();
        Shell_NotifyIconW(NIM_DELETE, &icon);
        g_trayIconAdded = false;
    }
    DestroyWindow(g_trayWindow); g_trayWindow = nullptr;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayEditor, L"\x6253\x5f00\x914d\x7f6e\x7f16\x8f91\x5668");
    AppendMenuW(menu, MF_STRING, kTrayOpenConfig, L"\x6253\x5f00\x914d\x7f6e\x6587\x4ef6");
    AppendMenuW(menu, MF_STRING, kTrayReload, L"\x91cd\x65b0\x52a0\x8f7d\x6548\x679c");
    const bool startupEnabled = StartupEnabledForMenu();
    AppendMenuW(menu, MF_STRING, kTrayStartup,
                startupEnabled ? L"\x5173\x95ed\x5f00\x673a\x542f\x52a8" : L"\x542f\x7528\x5f00\x673a\x542f\x52a8");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"\x9000\x51fa");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
    if (command) PostMessageW(hwnd, WM_COMMAND, command, 0);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage && message == g_taskbarCreatedMessage) { g_trayIconAdded = false; AddTrayIcon(); return 0; }
    const UINT trayEvent = LOWORD(lParam);
    if (message == kTrayMessage && (trayEvent == WM_RBUTTONUP || trayEvent == WM_LBUTTONUP || trayEvent == WM_CONTEXTMENU)) { ShowTrayMenu(hwnd); return 0; }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case kTrayEditor: {
            wchar_t module[MAX_PATH]{};
            GetModuleFileNameW(nullptr, module, MAX_PATH);
            std::wstring editor = module;
            const size_t slash = editor.find_last_of(L"\\/");
            editor = (slash == std::wstring::npos ? L"" : editor.substr(0, slash + 1)) + L"winglass-config.exe";
            ShellExecuteW(hwnd, L"open", editor.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case kTrayOpenConfig:
            ShellExecuteW(hwnd, L"open", g_configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kTrayReload:
            ReloadConfig();
            return 0;
        case kTrayStartup:
        {
            const bool enableStartup = !StartupEnabledForMenu();
            bool changed = SetStartupEnabled(enableStartup);
            // A process launched by an isolated automation host may have a
            // read-only view of the desktop user's Run key. In that specific
            // case, ask Windows to run this narrowly-scoped helper as the
            // interactive user instead of leaving the toggle half-working.
            if (!changed) changed = SetStartupEnabledElevated(enableStartup);
            if (changed) {
                PersistStartupStateCache(enableStartup);
            } else {
                MessageBoxW(hwnd,
                            enableStartup ? L"\x65e0\x6cd5\x542f\x7528\x5f00\x673a\x542f\x52a8\x3002\x8be6\x7ec6\x9519\x8bef\x5df2\x5199\x5165 winglass.log\x3002"
                                          : L"\x65e0\x6cd5\x5173\x95ed\x5f00\x673a\x542f\x52a8\x3002\x8be6\x7ec6\x9519\x8bef\x5df2\x5199\x5165 winglass.log\x3002",
                            L"WinGlass", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case kTrayExit:
            CleanupAllWindows();
            PostQuitMessage(0);
            return 0;
        default: break;
        }
    }
    if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
    if (message == WM_ENDSESSION && wParam) { CleanupAllWindows(); PostQuitMessage(0); return 0; }
    if (message == WM_DESTROY) { g_trayWindow = nullptr; return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateTrayIcon() {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = TrayProc; wc.hInstance = g_instance; wc.lpszClassName = kTrayClass;
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        WriteDiagnostic(L"tray window class registration failed, error=" + std::to_wstring(GetLastError()));
        return false;
    }
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    // A hidden top-level window receives Explorer's TaskbarCreated broadcast;
    // a message-only HWND does not, which made the icon disappear after restarts.
    g_trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTrayClass, L"WinGlass", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_instance, nullptr);
    if (!g_trayWindow) {
        WriteDiagnostic(L"tray window creation failed, error=" + std::to_wstring(GetLastError()));
        return false;
    }
    AddTrayIcon(); // Explorer may not be ready yet; the main loop retries.
    return true;
}

std::wstring ProcessName(HWND hwnd) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    // QueryFullProcessImageName only needs LIMITED_INFORMATION. Requesting
    // VM_READ rejects more protected processes and grants no benefit here.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    wchar_t path[MAX_PATH]{}; DWORD len = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &len)) {
        result = Lower(path);
        const auto slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos) result = result.substr(slash + 1);
    }
    CloseHandle(process);
    return result;
}

bool IsExcluded(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return true;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return true;
    wchar_t cls[128]{}; GetClassNameW(hwnd, cls, 128);
    const auto c = Lower(cls);
    return c == L"progman" || c == L"workerw" || c == L"shell_traywnd" || c == L"shell_secondarytraywnd" || c == L"windows.ui.core.corewindow" || c == L"winglassbackdropwindow";
}

Rule RuleFor(HWND hwnd, std::wstring& process) {
    process = ProcessName(hwnd);
    Rule rule = g_config.global;
    const auto it = g_config.apps.find(process);
    if (it != g_config.apps.end()) rule = it->second;
    return rule;
}

// SetWindowCompositionAttribute is the compositor entry point used by the
// Windows shell for legacy Acrylic surfaces. It is dynamically resolved so the
// executable still starts on systems where the export is unavailable.
enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

enum WindowCompositionAttribute { WCA_ACCENT_POLICY = 19 };

struct AccentPolicy {
    int accentState = ACCENT_DISABLED;
    int accentFlags = 0;
    DWORD gradientColor = 0;
    int animationId = 0;
};

struct WindowCompositionAttributeData {
    WindowCompositionAttribute attribute = WCA_ACCENT_POLICY;
    void* data = nullptr;
    SIZE_T sizeOfData = 0;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

SetWindowCompositionAttributeFn GetCompositionSetter() {
    static const auto setter = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return setter;
}

bool SetBackdropComposition(HWND backdrop, bool acrylic) {
    const auto setter = GetCompositionSetter();
    if (!setter || !backdrop) return false;

    AccentPolicy policy{};
    policy.accentState = acrylic ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_ENABLE_GRADIENT;
    // Keep AccentPolicy uncoloured. The separate tint HWND below applies the
    // configured colour without reducing the contrast of DWM's real blur.
    policy.accentFlags = 0;
    policy.gradientColor = 0;
    WindowCompositionAttributeData data{WCA_ACCENT_POLICY, &policy, sizeof(policy)};
    if (setter(backdrop, &data)) return true;

    // Some Windows builds reject Acrylic state 4 but accept legacy blur state 3.
    if (acrylic) {
        policy.accentState = ACCENT_ENABLE_BLURBEHIND;
        policy.accentFlags = 0;
        policy.gradientColor = 0;
        if (setter(backdrop, &data)) return true;
    }
    return false;
}

void DisableBackdropComposition(HWND backdrop) {
    const auto setter = GetCompositionSetter();
    if (!setter || !backdrop) return;
    AccentPolicy policy{};
    WindowCompositionAttributeData data{WCA_ACCENT_POLICY, &policy, sizeof(policy)};
    setter(backdrop, &data);
}

bool ConfigureBackdrop(WindowState& state, bool requested) {
    // Disabling glass means no DWM material at all; the independent tint layer
    // remains available and is updated by ApplyWindow below.
    if (!requested) {
        if (state.backdropConfigured) DisableBackdropComposition(state.backdrop);
        state.backdropConfigured = false;
        return true;
    }
    state.backdropConfigured = SetBackdropComposition(state.backdrop, true);
    return state.backdropConfigured;
}

bool SameColor(const Color& left, const Color& right) {
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

BYTE LerpByte(BYTE from, BYTE to, double progress) {
    return static_cast<BYTE>(std::clamp(std::lround(from + (to - from) * progress), 0l, 255l));
}

Color LerpColor(const Color& from, const Color& to, double progress) {
    return Color{LerpByte(static_cast<BYTE>(from.r), static_cast<BYTE>(to.r), progress),
                 LerpByte(static_cast<BYTE>(from.g), static_cast<BYTE>(to.g), progress),
                 LerpByte(static_cast<BYTE>(from.b), static_cast<BYTE>(to.b), progress)};
}

double SmoothStep(double progress) {
    // Ease both ends of every state transition. Linear alpha changes look
    // mechanical and reveal individual 8-bit opacity steps near the endpoints.
    return progress * progress * (3.0 - 2.0 * progress);
}

bool AuxPlacementMismatch(HWND aux, const RECT& expected) {
    if (!aux || !IsWindow(aux)) return true;
    RECT actual{};
    if (!GetWindowRect(aux, &actual)) return true;
    return actual.left != expected.left || actual.top != expected.top ||
           actual.right != expected.right || actual.bottom != expected.bottom;
}

bool AuxZOrderMismatch(const WindowState& state) {
    if (!state.target || !state.tint || !state.backdrop) return true;
    // GW_HWNDNEXT walks toward lower Z order for top-level windows. The
    // expected chain is target > tint > backdrop, matching window-blur-fx.
    return GetWindow(state.target, GW_HWNDNEXT) != state.tint ||
           GetWindow(state.tint, GW_HWNDNEXT) != state.backdrop;
}

bool GetWindowFrameRect(HWND hwnd, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) ||
        GetWindowRect(hwnd, &rect)) {
        return rect.right > rect.left && rect.bottom > rect.top;
    }
    return false;
}

WindowState* FindAuxState(HWND hwnd, bool tint) {
    for (auto& [_, state] : g_windows) {
        if ((tint && state.tint == hwnd) || (!tint && state.backdrop == hwnd)) return &state;
    }
    return nullptr;
}

void PaintTintWindow(HWND hwnd, HDC dc) {
    WindowState* state = FindAuxState(hwnd, true);
    if (!state || !dc) return;
    // The brush is short-lived because tint colour can change on a hot reload.
    // This avoids stale GDI objects while keeping the paint path simple.
    const COLORREF color = RGB(state->tintColor.r, state->tintColor.g, state->tintColor.b);
    HBRUSH brush = CreateSolidBrush(color);
    if (brush) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The Acrylic window is a visual-only shadow. Returning HTTRANSPARENT is a
    // second click-through guarantee in addition to WS_EX_TRANSPARENT.
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) {
        PaintTintWindow(hwnd, reinterpret_cast<HDC>(wp));
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintTintWindow(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureBackdropClass() {
    static bool registered = false; if (registered) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = BackdropProc; wc.hInstance = g_instance; wc.lpszClassName = kBackdropClass; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS; return registered;
}

bool WindowIdentityMatches(const WindowState& state) {
    if (!state.target || !IsWindow(state.target)) return false;
    DWORD currentPid = 0;
    GetWindowThreadProcessId(state.target, &currentPid);
    if (currentPid != state.processId) return false;
    return !state.recoveryMarkerSet || GetPropW(state.target, kManagedProperty) == ManagedMarker();
}

void DestroyAuxiliaryWindows(WindowState& state) {
    if (state.tint && IsWindow(state.tint)) DestroyWindow(state.tint);
    if (state.backdrop && IsWindow(state.backdrop)) {
        DisableBackdropComposition(state.backdrop);
        DestroyWindow(state.backdrop);
    }
    state.tint = nullptr;
    state.backdrop = nullptr;
}

void DestroyState(WindowState& state) {
    DestroyAuxiliaryWindows(state);
    // If the HWND has been recycled, restoring its style would mutate an
    // unrelated new window. The PID + property marker guard prevents that.
    if (WindowIdentityMatches(state)) {
        RestoreWindowStyle(state.target, state.originalExStyle, state.hadLayeredStyle,
                           state.originalAlpha, state.originalColorKey, state.originalLayerFlags);
    }
    state = {};
}

void SuspendState(WindowState& state) {
    if (state.suspended) return;
    // Do not restore the target alpha here. A minimized or fully covered
    // window has no visible pixels, and retaining the modified style lets a
    // later restore resume in one placement pass instead of recreating both
    // companion windows after the normal sweep interval.
    if (state.tint && IsWindow(state.tint)) ShowWindow(state.tint, SW_HIDE);
    if (state.backdrop && IsWindow(state.backdrop)) ShowWindow(state.backdrop, SW_HIDE);
    state.forcePlacement = true;
    state.suspended = true;
}

bool ResolveRule(HWND hwnd, Rule& rule) {
    if (IsExcluded(hwnd)) return false;
    std::wstring process; rule = RuleFor(hwnd, process);
    wchar_t className[128]{}; GetClassNameW(hwnd, className, 128);
    const auto processEntry = g_config.blacklist.find(process);
    const auto classEntry = g_config.blacklistClasses.find(Lower(className));
    const bool processBlocked = processEntry != g_config.blacklist.end() && processEntry->second;
    const bool classBlocked = classEntry != g_config.blacklistClasses.end() && classEntry->second;
    return !processBlocked && !classBlocked && rule.enabled;
}

bool ApplyWindow(HWND hwnd, bool refreshRule, HWND foreground, ULONGLONG now) {
    auto it = g_windows.find(hwnd);
    if (it != g_windows.end() && refreshRule && !WindowIdentityMatches(it->second)) {
        // Drop only our auxiliary windows when an HWND was recycled. Restoring
        // or immediately applying to the replacement could modify an unrelated
        // window during the same stale enumeration pass. A later sweep may
        // evaluate that new window from scratch.
        DestroyAuxiliaryWindows(it->second);
        g_windows.erase(it);
        return false;
    }
    if (it != g_windows.end() && refreshRule &&
        (!it->second.backdrop || !IsWindow(it->second.backdrop) ||
         !it->second.tint || !IsWindow(it->second.tint))) {
        // Auxiliary HWNDs belong to this process and should not disappear on
        // their own. If one does, fully restore the target and let a later
        // sweep create a coherent pair rather than running half-configured.
        DestroyState(it->second);
        g_windows.erase(it);
        return false;
    }
    Rule rule{};
    if (it == g_windows.end()) {
        if (!ResolveRule(hwnd, rule)) return false;
    } else {
        if (refreshRule && it->second.ruleRevision != g_configRevision) {
            if (!ResolveRule(hwnd, rule)) return false;
        } else {
            rule = it->second.rule;
        }
    }
    WindowState* state = nullptr;
    if (it == g_windows.end()) {
        WindowState fresh{}; fresh.target = hwnd; fresh.rule = rule; fresh.ruleRevision = g_configRevision;
        fresh.currentOpacity = 1.0; fresh.fromOpacity = 1.0; fresh.toOpacity = 1.0;
        GetWindowThreadProcessId(hwnd, &fresh.processId);
        if (!fresh.processId) return false;
        fresh.originalExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE); LONG_PTR ex = fresh.originalExStyle;
        fresh.hadLayeredStyle = (ex & WS_EX_LAYERED) != 0;
        // Per-pixel layered windows cannot be reconstructed from constant-alpha
        // metadata, so leave them untouched rather than making exit lossy.
        if (fresh.hadLayeredStyle) {
            if (!GetLayeredWindowAttributes(hwnd, &fresh.originalColorKey, &fresh.originalAlpha, &fresh.originalLayerFlags)) return false;
        } else {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) return false;
            fresh.changedStyle = true;
        }
        fresh.recoveryMarkerSet = SetPropW(hwnd, kManagedProperty, ManagedMarker()) != FALSE;
        // This is deliberately not a layered bitmap window. DWM must own the
        // surface so it can continuously blur the real desktop behind it.
        fresh.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                         kBackdropClass, L"", WS_POPUP, 0, 0, 0, 0,
                                         nullptr, nullptr, g_instance, nullptr);
        // Tint is layered, but the backdrop above is intentionally not. A
        // layered backdrop would turn DWM blur into a bitmap overlay and lose
        // the real background sampling that makes window-blur-fx effective.
        fresh.tint = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                     kBackdropClass, L"", WS_POPUP, 0, 0, 0, 0,
                                     nullptr, nullptr, g_instance, nullptr);
        if (!fresh.backdrop || !fresh.tint) {
            if (fresh.tint) DestroyWindow(fresh.tint);
            if (fresh.backdrop) DestroyWindow(fresh.backdrop);
            RestoreWindowStyle(hwnd, fresh.originalExStyle, fresh.hadLayeredStyle, fresh.originalAlpha, fresh.originalColorKey, fresh.originalLayerFlags);
            return false;
        }
        if (!SetLayeredWindowAttributes(fresh.tint, 0, 0, LWA_ALPHA)) {
            DestroyWindow(fresh.tint);
            DestroyWindow(fresh.backdrop);
            RestoreWindowStyle(hwnd, fresh.originalExStyle, fresh.hadLayeredStyle,
                               fresh.originalAlpha, fresh.originalColorKey, fresh.originalLayerFlags);
            return false;
        }
        state = &g_windows.emplace(hwnd, fresh).first->second;
        PersistJournal();
    } else state = &it->second;
    // A successful application means this target is again visible enough to
    // own auxiliary surfaces. This is also the fast path after restoring a
    // minimized window or releasing Alt following the task switcher.
    const bool wasSuspended = state->suspended;
    state->suspended = false;
    const bool active = hwnd == foreground;
    const bool requestedBackdrop = active ? rule.activeAcrylic : rule.inactiveAcrylic;
    const Color desiredTintColor = active ? rule.activeTint : rule.inactiveTint;
    const double tintStrength = active ? rule.activeTintStrength : rule.inactiveTintStrength;
    const double glassOpacity = active ? rule.activeGlassOpacity : rule.inactiveGlassOpacity;
    const BYTE desiredTintAlpha = static_cast<BYTE>(
        std::clamp(tintStrength * glassOpacity * 255.0, 0.0, 255.0));
    const double targetOpacity = active ? rule.activeOpacity : rule.inactiveOpacity;
    state->rule = rule;
    state->ruleRevision = g_configRevision;

    // A first application should establish the complete visual state at once.
    // Subsequent focus/configuration changes begin at the pixels already on
    // screen, which prevents a tint jump in the first animation frame.
    const bool isFirstVisual = !state->visualInitialized;
    const bool transitionChanged = isFirstVisual || active != state->wasActive ||
                                   state->toOpacity != targetOpacity ||
                                   state->toTintAlpha != desiredTintAlpha ||
                                   !SameColor(state->toTintColor, desiredTintColor);
    if (isFirstVisual) {
        state->currentOpacity = state->fromOpacity = state->toOpacity = targetOpacity;
        state->tintColor = state->fromTintColor = state->toTintColor = desiredTintColor;
        state->tintAlpha = state->fromTintAlpha = state->toTintAlpha = desiredTintAlpha;
        state->transitionStart = now;
        state->visualInitialized = true;
    } else if (transitionChanged) {
        state->fromOpacity = state->currentOpacity;
        state->toOpacity = targetOpacity;
        state->fromTintColor = state->tintColor;
        state->toTintColor = desiredTintColor;
        state->fromTintAlpha = state->tintAlpha;
        state->toTintAlpha = desiredTintAlpha;
        state->transitionStart = now;
    }
    state->wasActive = active;

    const ULONGLONG elapsed = now - state->transitionStart;
    const double progress = isFirstVisual || rule.transitionMs == 0 ? 1.0 :
        std::min(1.0, elapsed / static_cast<double>(rule.transitionMs));
    const double easedProgress = SmoothStep(progress);
    state->currentOpacity = state->fromOpacity + (state->toOpacity - state->fromOpacity) * easedProgress;
    state->tintColor = LerpColor(state->fromTintColor, state->toTintColor, easedProgress);
    state->tintAlpha = LerpByte(state->fromTintAlpha, state->toTintAlpha, easedProgress);
    const bool transitionInFlight = progress < 1.0;
    if (transitionInFlight) g_visualAnimationActive = true;
    const BYTE targetAlpha = static_cast<BYTE>(std::clamp(state->currentOpacity * 255.0, 10.0, 255.0));
    if (!state->applied || targetAlpha != state->lastAppliedAlpha) {
        if (SetLayeredWindowAttributes(hwnd, 0, targetAlpha, LWA_ALPHA)) {
            state->lastAppliedAlpha = targetAlpha;
            state->applied = true;
        }
    }
    if (state->tint) {
        // Constant-alpha layered tint is independent from target opacity and
        // therefore never recolours the target window's text pixels.
        const bool tintChanged = wasSuspended || !state->tintApplied || state->lastTintAlpha != state->tintAlpha ||
                                 state->lastTintColor.r != state->tintColor.r ||
                                 state->lastTintColor.g != state->tintColor.g ||
                                 state->lastTintColor.b != state->tintColor.b;
        if (tintChanged) {
            if (SetLayeredWindowAttributes(state->tint, 0, state->tintAlpha, LWA_ALPHA)) {
                InvalidateRect(state->tint, nullptr, TRUE);
                ShowWindow(state->tint, state->tintAlpha ? SW_SHOWNOACTIVATE : SW_HIDE);
                state->lastTintColor = state->tintColor;
                state->lastTintAlpha = state->tintAlpha;
                state->tintApplied = true;
            }
        }
    }
    // Active windows and transitions retain 120 Hz geometry tracking. Stable
    // inactive windows need only 20 Hz; this removes most DWM frame queries
    // without affecting drag/resize smoothness or focus animations.
    const ULONGLONG geometryInterval = active || transitionInFlight ? 8 : 50;
    if (!wasSuspended && state->hasPosition && now - state->lastGeometrySample < geometryInterval) return true;
    state->lastGeometrySample = now;

    RECT rect{};
    if (!GetWindowFrameRect(hwnd, rect)) {
        SuspendState(*state);
        return true;
    }
    if (state->backdrop && IsWindow(state->backdrop) && state->tint && IsWindow(state->tint)) {
        const int w = rect.right - rect.left, h = rect.bottom - rect.top;
        const bool moved = !state->hasPosition || state->lastX != rect.left || state->lastY != rect.top;
        const bool resized = !state->hasPosition || state->lastW != w || state->lastH != h;
        if (moved || resized) {
            state->lastX = rect.left; state->lastY = rect.top;
            state->lastW = w; state->lastH = h; state->hasPosition = true;
            state->forcePlacement = true;
        }
        if (!moved && !resized && !state->forcePlacement && now - state->lastPlacementValidation >= 50) {
            // Another application or a shell Z-order update can move an
            // auxiliary window without changing the target rectangle. This
            // validation is intentionally throttled; doing three User32 calls
            // per target at 120 Hz provided no smoother visible motion.
            state->lastPlacementValidation = now;
            state->forcePlacement = AuxPlacementMismatch(state->backdrop, rect) ||
                                    AuxPlacementMismatch(state->tint, rect) ||
                                    AuxZOrderMismatch(*state);
        }
        const bool backdropModeChanged = !state->backdropRequestKnown ||
                                         state->backdropRequested != requestedBackdrop;
        const bool retryBackdrop = requestedBackdrop && !state->backdropConfigured &&
            (state->backdropRetryAfter == 0 || now >= state->backdropRetryAfter);
        if (backdropModeChanged || retryBackdrop) {
            const bool configured = ConfigureBackdrop(*state, requestedBackdrop);
            state->backdropRequestKnown = true;
            state->backdropRequested = requestedBackdrop;
            state->backdropRetryAfter = configured || !requestedBackdrop ? 0 : now + 1000;
        }
        if (state->backdropConfigured && requestedBackdrop) {
            ShowWindow(state->backdrop, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(state->backdrop, SW_HIDE);
        }
        if (state->forcePlacement) {
            const bool backdropPlaced = SetWindowPos(state->backdrop, hwnd, rect.left, rect.top, w, h,
                                                      SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
            // Insert tint immediately below the target after the backdrop has
            // been positioned, producing target > tint > backdrop ordering.
            const bool tintPlaced = SetWindowPos(state->tint, hwnd, rect.left, rect.top, w, h,
                                                  SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
            state->forcePlacement = !(backdropPlaced && tintPlaced);
            if (!state->forcePlacement) state->lastPlacementValidation = now;
        }
    }
    return true;
}

struct SweepContext {
    HRGN covered = nullptr;
    std::unordered_set<HWND> selected;
    size_t eligibleRank = 0;
    HWND foreground = nullptr;
};

bool WindowRectForOcclusion(HWND hwnd, RECT& rect) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (!GetWindowFrameRect(hwnd, rect)) return false;
    const RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT clipped{};
    if (!IntersectRect(&clipped, &rect, &screen)) return false;
    rect = clipped;
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool IsOpaqueOccluder(HWND hwnd, bool ownWindow) {
    if (ownWindow) return false;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT) return false;
    if ((exStyle & WS_EX_LAYERED) == 0) return true;
    // A WinGlass-managed target is considered a geometric occluder on purpose:
    // the lower window does not need its own expensive glass surface.
    if (g_windows.find(hwnd) != g_windows.end()) return true;
    COLORREF colorKey = 0; BYTE alpha = 255; DWORD flags = 0;
    if (!GetLayeredWindowAttributes(hwnd, &colorKey, &alpha, &flags)) return false;
    if ((flags & LWA_ALPHA) && alpha < 255) return false;
    if (flags & LWA_COLORKEY) return false;
    return true;
}

BOOL CALLBACK EnumSweepProc(HWND hwnd, LPARAM value) {
    auto& context = *reinterpret_cast<SweepContext*>(value);
    RECT rect{};
    if (!WindowRectForOcclusion(hwnd, rect)) return TRUE;

    wchar_t cls[128]{}; GetClassNameW(hwnd, cls, 128);
    const auto className = Lower(cls);
    const bool ownWindow = className == Lower(kBackdropClass) || className == Lower(kTrayClass);
    const bool eligible = !IsExcluded(hwnd);
    if (eligible) {
        HRGN windowRegion = CreateRectRgn(rect.left, rect.top, rect.right, rect.bottom);
        HRGN visibleRegion = CreateRectRgn(0, 0, 0, 0);
        const int regionType = CombineRgn(visibleRegion, windowRegion, context.covered, RGN_DIFF);
        const bool fullyOccluded = regionType == NULLREGION;
        const bool tooLow = context.eligibleRank >= kMaxTrackedWindows;
        if (hwnd == context.foreground || (!fullyOccluded && !tooLow)) context.selected.insert(hwnd);
        ++context.eligibleRank;
        DeleteObject(visibleRegion); DeleteObject(windowRegion);
    }

    // Use geometric coverage deliberately: once a top-level window occupies
    // every pixel, processing windows below it buys no visible result.
    if (IsOpaqueOccluder(hwnd, ownWindow)) {
        HRGN windowRegion = CreateRectRgn(rect.left, rect.top, rect.right, rect.bottom);
        CombineRgn(context.covered, context.covered, windowRegion, RGN_OR);
        DeleteObject(windowRegion);
    }
    return TRUE;
}

void Sweep(HWND foreground, ULONGLONG now) {
    SweepContext context{};
    context.covered = CreateRectRgn(0, 0, 0, 0);
    context.foreground = foreground;
    EnumWindows(EnumSweepProc, reinterpret_cast<LPARAM>(&context));
    DeleteObject(context.covered);

    if (context.selected.empty() && now - g_lastEmptySweepDiagnostic >= 15000) {
        WriteDiagnostic(L"window sweep selected no eligible targets");
        g_lastEmptySweepDiagnostic = now;
    }

    std::unordered_set<HWND> accepted;
    for (HWND hwnd : context.selected) {
        if (ApplyWindow(hwnd, true, foreground, now)) accepted.insert(hwnd);
    }
    for (auto it = g_windows.begin(); it != g_windows.end();) {
        if (!WindowIdentityMatches(it->second)) {
            DestroyState(it->second);
            it = g_windows.erase(it);
            continue;
        }
        if (accepted.find(it->first) != accepted.end()) {
            ++it;
            continue;
        }

        if (IsExcluded(it->first)) {
            // Rules, blacklists, and deleted windows are permanent changes;
            // they still require full restoration rather than suspension.
            DestroyState(it->second);
            it = g_windows.erase(it);
            continue;
        }
        if (it->second.ruleRevision != g_configRevision) {
            Rule currentRule{};
            if (!ResolveRule(it->first, currentRule)) {
                DestroyState(it->second);
                it = g_windows.erase(it);
                continue;
            }
            it->second.rule = currentRule;
            it->second.ruleRevision = g_configRevision;
        }

        // The target is still eligible but is currently too low in Z order,
        // fully covered, or minimized. Preserve it for instant resumption.
        SuspendState(it->second);
        ++it;
    }
    PersistJournal();
}

void UpdateTrackedWindows(HWND foreground, ULONGLONG now) {
    g_visualAnimationActive = false;
    std::vector<HWND> handles; handles.reserve(g_windows.size());
    for (const auto& [hwnd, _] : g_windows) handles.push_back(hwnd);
    for (HWND hwnd : handles) {
        const auto found = g_windows.find(hwnd);
        if (found == g_windows.end() || !IsWindow(hwnd)) continue;
        if (!found->second.suspended) {
            ApplyWindow(hwnd, false, foreground, now);
        } else if (hwnd == foreground && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
            // Some applications receive foreground activation just before
            // their restore animation clears iconic state. Polling at the
            // existing 120 Hz cadence closes that timing gap without adding a
            // cross-process event hook or waiting for the next full sweep.
            ApplyWindow(hwnd, true, foreground, now);
        }
    }
    if (g_visualAnimationActive) {
        // Present one coherent opacity/tint batch at the compositor boundary.
        // Calling DwmFlush per window causes visible phase differences when the
        // foreground window changes and several managed windows are animated.
        DwmFlush();
    }
}

bool RunCompositionSelfTest() {
    // This verifies API availability and that the current compositor accepts an
    // Acrylic policy. Visual blur still requires an interactive desktop check.
    if (!GetCompositionSetter() || !EnsureBackdropClass()) return false;
    HWND testWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                      kBackdropClass, L"", WS_POPUP,
                                      -30000, -30000, 64, 64,
                                      nullptr, nullptr, g_instance, nullptr);
    if (!testWindow) return false;
    const bool accepted = SetBackdropComposition(testWindow, true);
    DisableBackdropComposition(testWindow);
    DestroyWindow(testWindow);
    return accepted;
}

int SelfTest() {
    if (!LoadConfig(g_config, g_configPath)) { std::fwprintf(stderr, L"config load failed: %ls\n", g_configPath.c_str()); return 2; }
    if (!RunCompositionSelfTest()) { std::fwprintf(stderr, L"system Acrylic composition unavailable\n"); return 3; }
    std::wprintf(L"config and system Acrylic API ok: %ls global enabled=%d active_target=%.2f glass=%.2f apps=%zu blacklist=%zu classes=%zu transition_ms=%d\n", g_configPath.c_str(), g_config.global.enabled ? 1 : 0, g_config.global.activeOpacity, g_config.global.activeGlassOpacity, g_config.apps.size(), g_config.blacklist.size(), g_config.blacklistClasses.size(), g_config.global.transitionMs);
    return 0;
}

void CleanupAllWindows() {
    if (g_cleanedUp) return;
    g_cleanedUp = true;
    for (auto& [_, state] : g_windows) if (state.backdrop && IsWindow(state.backdrop)) ShowWindow(state.backdrop, SW_HIDE);
    for (auto& [_, state] : g_windows) DestroyState(state);
    g_windows.clear();
    PersistJournal();
    DwmFlush();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    g_instance = instance;
    // DWM frame bounds are physical pixels. Per-monitor V2 prevents User32 from
    // virtualizing SetWindowPos at 125%/150% scaling and keeps every Acrylic
    // shadow exactly aligned with its target on mixed-DPI desktops.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH); g_configPath = module; const auto slash = g_configPath.find_last_of(L"\\/");
    const auto base = slash == std::wstring::npos ? std::wstring() : g_configPath.substr(0, slash + 1);
    g_statePath = base + L"winglass.state";
    g_diagnosticsPath = base + L"winglass.log";
    g_startupStatePath = base + L"winglass.startup-state";
    g_configPath = base + L"config.yaml";
    // This is deliberately before config parsing and the desktop check: the
    // elevated helper must remain usable even when the normal process is on a
    // non-interactive desktop or a user has temporarily broken their config.
    if (wcsstr(commandLine, L"--set-startup=enable")) return SetStartupEnabled(true) ? 0 : 5;
    if (wcsstr(commandLine, L"--set-startup=disable")) return SetStartupEnabled(false) ? 0 : 5;
    LoadStartupStateCache();
    WIN32_FILE_ATTRIBUTE_DATA configData{};
    if (!GetFileAttributesExW(g_configPath.c_str(), GetFileExInfoStandard, &configData)) g_configPath = base + L"config.ini";
    if (!LoadConfig(g_config, g_configPath)) return 2;
    g_configWriteTime = g_config.writeTime;
    if (wcsstr(commandLine, L"--self-test")) return SelfTest();
    const bool relaunchAttempt = wcsstr(commandLine, L"--interactive-relaunch") != nullptr;
    if (!CanSeeInteractiveDesktop()) {
        if (!relaunchAttempt && RelaunchOnExplorerDesktop()) {
            WriteDiagnostic(L"restarted on the user interactive desktop");
            return 0;
        }
        WriteDiagnostic(relaunchAttempt ?
            L"interactive relaunch still cannot see the user desktop" :
            L"cannot see the user desktop and Explorer-token relaunch failed");
        // A background-desktop instance cannot affect windows or host a tray
        // icon. Exit instead of looking healthy while doing nothing.
        return 4;
    }
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\WinGlass.SingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleInstance) CloseHandle(singleInstance); return 0; }
    RestoreJournal(g_statePath);
    EnsureBackdropClass();
    SetProcessShutdownParameters(0x3ff, SHUTDOWN_NORETRY);
    StartWatchdog();
    CreateTrayIcon();
    MSG msg{};
    ULONGLONG lastTrack = 0, lastSweep = 0, lastConfigCheck = 0, lastTrayRetry = 0;
    HWND lastForeground = nullptr;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        const ULONGLONG now = GetTickCount64();
        const HWND foreground = GetForegroundWindow();
        if (foreground != lastForeground || g_sweepRequested) {
            // Alt+Tab changes foreground before its translucent/opaque shell
            // surface disappears. Sweep immediately on both edges so the
            // selected target regains its retained helper windows on the next
            // frame, rather than waiting up to the ordinary 300 ms cadence.
            lastForeground = foreground;
            g_sweepRequested = false;
            Sweep(foreground, now);
            lastSweep = now;
        }
        // Drive state interpolation at 120 Hz. DwmFlush above naturally caps
        // visible presents to the monitor refresh rate while the finer clock
        // keeps 8-bit alpha steps from bunching up on 120/144 Hz displays.
        if (now - lastTrack >= 8) { lastTrack = now; UpdateTrackedWindows(foreground, now); }
        if (now - lastSweep >= 300) { lastSweep = now; Sweep(foreground, now); }
        if (now - lastConfigCheck >= 120) {
            lastConfigCheck = now; WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(g_configPath.c_str(), GetFileExInfoStandard, &data) && !SameFileTime(g_configWriteTime, data.ftLastWriteTime)) ReloadConfig();
        }
        if (!g_trayIconAdded && now - lastTrayRetry >= 2000) {
            lastTrayRetry = now;
            if (!g_trayWindow) CreateTrayIcon(); else AddTrayIcon();
        }
        Sleep(8);
    }
    CleanupAllWindows();
    DestroyTrayIcon();
    ReleaseMutex(singleInstance); CloseHandle(singleInstance);
    return 0;
}
