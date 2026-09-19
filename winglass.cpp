#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <oleauto.h>
// WinHTTP is the only network dependency: one HTTPS GET to the GitHub releases
// API, from the resident process only. The editor and the watchdog stay offline.
#include <winhttp.h>
// GetApplicationUserModelId: the package identity is the only reliable way to
// tell two Store/MSIX applications apart, because they share one host process.
#include <appmodel.h>
#include <process.h>

#include "winglass-resource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <new>
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
#pragma comment(lib, "winhttp.lib")

namespace {

// Every string the resident process can display. Both languages sit side by
// side so a translation can be compared at a glance, and this table is the one
// place a new string has to be added. The editor keeps its own table, because
// the three executables share no code beyond the resource identifiers.
#define WINGLASS_TEXTS(T) \
    T(TrayTooltip, L"WinGlass \x5168\x5c40\x78e8\x7802\x73bb\x7483", L"WinGlass - global frosted glass") \
    T(MenuEditor, L"\x6253\x5f00\x914d\x7f6e\x7f16\x8f91\x5668", L"Open settings editor") \
    T(MenuConfig, L"\x6253\x5f00\x914d\x7f6e\x6587\x4ef6", L"Open configuration file") \
    T(MenuReload, L"\x91cd\x65b0\x52a0\x8f7d\x6548\x679c", L"Reload effect") \
    T(MenuStartupOn, L"\x542f\x7528\x5f00\x673a\x542f\x52a8", L"Enable startup at sign-in") \
    T(MenuStartupOff, L"\x5173\x95ed\x5f00\x673a\x542f\x52a8", L"Disable startup at sign-in") \
    T(MenuExit, L"\x9000\x51fa", L"Exit") \
    T(StartupEnableFailed, L"\x65e0\x6cd5\x542f\x7528\x5f00\x673a\x542f\x52a8\x3002\x8be6\x7ec6\x9519\x8bef\x5df2\x5199\x5165 winglass.log\x3002", L"Could not enable startup at sign-in. Details were written to winglass.log.") \
    T(StartupDisableFailed, L"\x65e0\x6cd5\x5173\x95ed\x5f00\x673a\x542f\x52a8\x3002\x8be6\x7ec6\x9519\x8bef\x5df2\x5199\x5165 winglass.log\x3002", L"Could not disable startup at sign-in. Details were written to winglass.log.") \
    T(MenuDownloadFormat, L"\x4e0b\x8f7d v%ls", L"Download v%ls") \
    T(BalloonUpdateTitle, L"WinGlass \x66f4\x65b0", L"WinGlass update") \
    T(BalloonUpdateFormat, L"WinGlass \x6709\x65b0\x7248\x672c %ls\xff0c\x70b9\x51fb\x6258\x76d8\x83dc\x5355\x4e0b\x8f7d\x3002", L"WinGlass %ls is available. Open the tray menu to download it.") \
    T(LabTitle, L"WinGlass \x80cc\x677f Alpha \x5b9e\x9a8c\x573a", L"WinGlass backdrop Alpha lab") \
    T(LabInstructions, L"\x4e0a\x6392\x666e\x901a Acrylic\xff0c\x4e0b\x6392 Layered Acrylic\x3002\x89c2\x5bdf\x52a8\x6001\x68cb\x76d8\x3001\x95ea\x70c1\x548c\x5931\x7126\x3002", L"Top: normal Acrylic. Bottom: layered Acrylic. Watch the moving pattern, flicker, and inactive behaviour.") \
    T(LabLevels, L"\x4ece\x5de6\x5230\x53f3\xff1a 20% / 40% / 60% / 80% / 100%", L"Left to right: 20% / 40% / 60% / 80% / 100%") \
    T(LabNormal, L"\x666e\x901a Acrylic\xff08\x7b56\x7565 Alpha\xff09", L"Normal Acrylic (policy Alpha)") \
    T(LabLayered, L"\x5206\x5c42 Acrylic\xff08\x7a97\x53e3 Alpha\xff09", L"Layered Acrylic (window Alpha)") \
    T(LabPulse, L"\x8109\x51b2 Alpha\xff08\x538b\x529b\x6d4b\x8bd5\xff09", L"Pulse Alpha (stress test)") \
    T(LabClose, L"\x5173\x95ed\x5b9e\x9a8c\x573a", L"Close lab")

enum class TextId {
#define WINGLASS_DECLARE_TEXT(id, zh, en) id,
    WINGLASS_TEXTS(WINGLASS_DECLARE_TEXT)
#undef WINGLASS_DECLARE_TEXT
};

struct TextEntry {
    const wchar_t* chinese;
    const wchar_t* english;
};

const TextEntry kTexts[] = {
#define WINGLASS_DEFINE_TEXT(id, zh, en) { zh, en },
    WINGLASS_TEXTS(WINGLASS_DEFINE_TEXT)
#undef WINGLASS_DEFINE_TEXT
};

enum class UiLanguage { Chinese, English };
UiLanguage g_language = UiLanguage::Chinese;

// An explicit tag wins; an empty or unknown one falls back to the Windows
// display language, so "auto" needs no special case.
void SelectLanguage(const std::wstring& tag) {
    if (tag == L"en" || tag == L"en-US" || tag == L"english") { g_language = UiLanguage::English; return; }
    if (tag == L"zh" || tag == L"zh-CN" || tag == L"chinese") { g_language = UiLanguage::Chinese; return; }
    g_language = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE) ? UiLanguage::Chinese : UiLanguage::English;
}

// An explicit non-empty --lang= wins over everything. When it is absent (or
// empty), the `ui_language` configuration value decides; "auto" and any other
// unknown tag fall through to the Windows display language. The configuration
// pointer is null during the earliest startup work, so the helper can be
// called before the file has been read.
void ResolveUiLanguage(const wchar_t* commandLine, const std::wstring* configLanguage = nullptr) {
    std::wstring tag;
    if (commandLine) {
        const wchar_t* flag = wcsstr(commandLine, L"--lang=");
        if (flag) {
            for (flag += 7; *flag && *flag != L' ' && *flag != L'"'; ++flag) tag += *flag;
        }
    }
    if (tag.empty() && configLanguage) tag = *configLanguage;
    SelectLanguage(tag);
}

const wchar_t* Str(TextId id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= std::size(kTexts)) return L"";
    return g_language == UiLanguage::English ? kTexts[index].english : kTexts[index].chinese;
}

// Diagnostic: prints the resolved language and every string, English in full
// and Chinese as a character count, so the table can be checked on a console
// that cannot display Chinese at all.
int LanguageSelfTest() {
    std::wprintf(L"language=%ls strings=%zu\n", g_language == UiLanguage::English ? L"en" : L"zh", std::size(kTexts));
    for (size_t i = 0; i < std::size(kTexts); ++i) {
        std::wprintf(L"  [%zu] zh_chars=%zu en=%ls\n", i, wcslen(kTexts[i].chinese), kTexts[i].english);
    }
    return 0;
}

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
    // This is the opacity of the Acrylic helper HWND itself. It intentionally
    // remains separate from glassOpacity below, whose historical meaning is
    // the multiplier used by the independent tint surface.
    double activeBackdropAlpha = 1.0;
    double inactiveBackdropAlpha = 1.0;
    double activeGlassOpacity = 0.98;
    double inactiveGlassOpacity = 0.96;
    Color activeTint{44, 62, 88};
    Color inactiveTint{31, 42, 60};
    double activeTintStrength = 0.34;
    double inactiveTintStrength = 0.30;
    int transitionMs = 180;
    // Full-screen browser video and games should remain pixel-perfect.  This
    // is enabled by default and can be overridden per application rule.
    bool excludeFullscreen = true;
};

// One entry of the `applications` section.  Every matcher that is present must
// match the window, so an entry can be as broad as a title substring or as
// narrow as one package identity.  An entry with no matcher never applies.
//
// The executable name alone cannot separate the applications Windows hosts
// inside a shared process: every Store/MSIX window arrives as
// ApplicationFrameHost.exe, which made "allow Notepad but not Calculator"
// impossible before the package identity became a matcher.
struct AppRule {
    std::wstring process;    // lower-case executable file name
    std::wstring className;  // lower-case window class name
    std::wstring title;      // lower-case substring of the window title
    std::wstring aumid;      // lower-case package identity (Store/MSIX apps)
    Rule rule{};

    bool AnyMatcher() const { return !process.empty() || !className.empty() || !title.empty() || !aumid.empty(); }
    // How much a matcher is trusted.  A rule that names a process must win
    // over one that only inspects a title, no matter which one the file lists
    // first; a broad rule silently shadowing a precise one is the kind of bug
    // that is impossible to explain to the person who wrote both.
    int Rank() const {
        if (!process.empty()) return 0;
        if (!aumid.empty()) return 1;
        if (!className.empty()) return 2;
        return 3;
    }
};

struct Config {
    Rule global;
    // Ordered: the first matching entry wins.  Sorted by rank while loading.
    std::vector<AppRule> apps;
    std::unordered_map<std::wstring, bool> blacklist;
    std::unordered_map<std::wstring, bool> blacklistClasses;
    FILETIME writeTime{};
    bool loaded = false;
    // Set while any entry matches on the window title.  Such a rule can start
    // or stop applying while both the HWND and the configuration stay exactly
    // the same, so rule resolution cannot be limited to configuration reloads.
    bool anyTitleMatcher = false;
    // "auto" / "zh" / "en"; an explicit --lang= overrides it.  Kept as the raw
    // tag so the same SelectLanguage() spelling rules apply as on the command
    // line, and "auto" needs no special case (it falls back to the display
    // language).
    std::wstring uiLanguage = L"auto";
    // Process-wide behaviour, not a per-window Rule field, so it lives beside
    // uiLanguage instead of inside the global appearance block. A plain bool,
    // unlike uiLanguage it needs no special spelling rules.
    bool checkUpdates = true;
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
    // The visual experiment established that a layered Acrylic helper keeps
    // live blur while its window alpha changes. These fields let that alpha
    // use the same focus-transition curve as the target and tint surfaces.
    BYTE backdropAlpha = 255;
    BYTE fromBackdropAlpha = 255;
    BYTE toBackdropAlpha = 255;
    BYTE lastBackdropAlpha = 255;
    bool backdropAlphaApplied = false;
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
static const wchar_t* kAlphaLabPatternClass = L"WinGlassAlphaLabPattern";
static const wchar_t* kAlphaLabControlClass = L"WinGlassAlphaLabControl";
static const wchar_t* kManagedProperty = L"WinGlass.ManagedTarget.v1";
static constexpr UINT_PTR kManagedMarkerValue = 0x57474C31; // "WGL1"
static constexpr UINT kTrayMessage = WM_APP + 42;
// The update worker posts its result to the tray window instead of touching the
// UI itself; the main thread owns g_updateVersion and the tray balloon.
static constexpr UINT kUpdateCheckMessage = WM_APP + 43;
static constexpr UINT_PTR kAlphaLabPatternTimer = 7001;
static constexpr UINT_PTR kAlphaLabPulseTimer = 7002;
static constexpr UINT kAlphaLabPulseCommand = 7101;
static constexpr UINT kAlphaLabCloseCommand = 7102;
static constexpr UINT kTrayOpenConfig = 41001;
static constexpr UINT kTrayReload = 41002;
static constexpr UINT kTrayStartup = 41003;
static constexpr UINT kTrayExit = 41004;
static constexpr UINT kTrayEditor = 41005;
// Stable id for the conditional "Download vX.Y.Z" item.
static constexpr UINT kTrayDownload = 41006;
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
// Update-check state. g_updateCheckEnabled mirrors Config::checkUpdates and is
// written by the main thread only; the worker reads it so a hot-reloaded
// `check_updates: false` stops the next request without stopping the thread.
static std::atomic<bool> g_updateCheckEnabled{true};
static HANDLE g_updateThread = nullptr;
static HANDLE g_updateStopEvent = nullptr;
// Non-empty only after a strictly newer release has been found. Written by the
// main thread when it handles kUpdateCheckMessage, read when the tray menu is
// built, so the worker never touches it.
static std::wstring g_updateVersion;
static constexpr DWORD kUpdateCheckIntervalMs = 24u * 60u * 60u * 1000u;

struct AlphaLabPane {
    HWND hwnd = nullptr;
    BYTE baseAlpha = 255;
    bool layered = false;
};

// The alpha lab is deliberately isolated from g_windows: it never modifies a
// foreign HWND, never creates a recovery journal entry, and can run alongside
// the resident effect without participating in its sweep or Z-order rules.
struct AlphaLabState {
    HWND control = nullptr;
    HWND pattern = nullptr;
    std::vector<AlphaLabPane> panes;
    bool pulse = false;
    ULONGLONG pulseStart = 0;
    int patternPhase = 0;
};

static AlphaLabState g_alphaLab;

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

// Strips one layer of matching quotes. Shared by the YAML reader and by the
// matcher fields, which accept the same quoted form in both configuration
// flavours.
std::wstring UnquoteYaml(std::wstring value) {
    value = Trim(value);
    if (value.size() >= 2 && ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\''))) return value.substr(1, value.size() - 2);
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

// ---------------------------------------------------------------------------
// GitHub release update check
//
// One HTTPS GET against the public releases API, run on a worker thread so the
// tray icon and the 8 ms effect loop are never blocked by DNS or a slow
// server. The worker posts a message to the tray window; only the main thread
// touches the menu and the balloon. Any failure is a single diagnostic line,
// never a dialog, and never affects window effects.
// ---------------------------------------------------------------------------

// The diagnostic prints English on purpose: it is a console report and the
// Chinese half of the table would need an encoding the console cannot promise.
const wchar_t* EnglishText(TextId id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= std::size(kTexts)) return L"";
    return kTexts[index].english;
}

std::wstring FormatText(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length <= 0) return format;
    std::wstring text(static_cast<size_t>(length), L'\0');
    va_start(args, format);
    _vsnwprintf_s(text.data(), text.size() + 1, _TRUNCATE, format, args);
    va_end(args);
    return text;
}

// WINGLASS_PRODUCTVERSION_STRING is a narrow literal because the resource
// script needs it that way. It is pure ASCII, so a byte-by-byte widen is
// enough and keeps a temporary edit of the header visible in --check-updates.
std::wstring ProductVersion() {
    std::wstring value;
    for (const char* p = WINGLASS_PRODUCTVERSION_STRING; *p; ++p) {
        value += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    }
    return value;
}

// Normalises a tag for display. StripVersionPrefix removes the optional
// leading "v" so a format string that already carries the "v" (the tray menu
// label) does not end up with two of them; DisplayTag re-adds exactly one for
// messages that spell the version on its own.
std::wstring StripVersionPrefix(const std::wstring& tag) {
    std::wstring value = Trim(tag);
    if (!value.empty() && (value[0] == L'v' || value[0] == L'V')) value.erase(value.begin());
    return value;
}

std::wstring DisplayTag(const std::wstring& tag) {
    return L"v" + StripVersionPrefix(tag);
}

// Parses the numeric dotted prefix of a version. Missing components are not
// invented here; CompareVersions treats them as zero. A non-numeric suffix
// stops the parse, so "1.2.0-beta" is 1.2.0. Returns false when no numeric
// component was found at all, which is how empty/garbage tags stay harmless.
bool ParseVersion(const std::wstring& value, std::vector<int>& parts) {
    parts.clear();
    std::wstring text = Trim(value);
    if (!text.empty() && (text[0] == L'v' || text[0] == L'V')) text.erase(text.begin());
    size_t pos = 0;
    while (pos < text.size()) {
        if (!iswdigit(text[pos])) break;
        long long number = 0;
        while (pos < text.size() && iswdigit(text[pos])) {
            if (number < 100000000) number = number * 10 + (text[pos] - L'0');
            ++pos;
        }
        parts.push_back(static_cast<int>(number));
        if (pos < text.size() && text[pos] == L'.') { ++pos; continue; }
        break;
    }
    return !parts.empty();
}

// Numeric component-by-component comparison. An unparsable side compares equal
// so a malformed tag can never trigger an update offer.
int CompareVersions(const std::wstring& left, const std::wstring& right) {
    std::vector<int> a, b;
    if (!ParseVersion(left, a) || !ParseVersion(right, b)) return 0;
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// Scans a JSON body for "tag_name": "...". The endpoint is small and its
// schema is stable, so a full JSON parser would be more risk than value. An
// escaped quote is copied literally; tag names never contain one in practice.
bool ExtractTagName(const std::wstring& json, std::wstring& tag) {
    const std::wstring key = L"\"tag_name\"";
    size_t pos = json.find(key);
    if (pos == std::wstring::npos) return false;
    pos += key.size();
    while (pos < json.size() && iswspace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != L':') return false;
    ++pos;
    while (pos < json.size() && iswspace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != L'"') return false;
    ++pos;
    tag.clear();
    while (pos < json.size() && json[pos] != L'"') {
        if (json[pos] == L'\\' && pos + 1 < json.size()) { tag += json[pos + 1]; pos += 2; continue; }
        tag += json[pos];
        ++pos;
    }
    return pos < json.size();
}

// Synchronous HTTPS GET. Returns the raw tag and a short ASCII reason on
// failure. WinHttpSetTimeouts keeps a stalled server from holding the worker
// (and shutdown) for more than roughly five seconds per phase.
bool FetchLatestReleaseTag(std::wstring& tag, std::wstring& error) {
    error.clear();
    const wchar_t* host = L"api.github.com";
    const wchar_t* resource = L"/repos/heathacosta259519-dot/WinGlass/releases/latest";
    HINTERNET session = WinHttpOpen(L"WinGlass", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"WinHttpOpen error=" + std::to_wstring(GetLastError()); return false; }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        error = L"WinHttpConnect error=" + std::to_wstring(GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", resource, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        error = L"WinHttpOpenRequest error=" + std::to_wstring(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    // GitHub answers 403 to a request without a User-Agent, so send one that
    // also identifies the exact build making the request.
    const std::wstring headers = L"User-Agent: WinGlass/" + ProductVersion();
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    bool ok = true;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = L"WinHttpSendRequest error=" + std::to_wstring(GetLastError());
        ok = false;
    }
    if (ok && !WinHttpReceiveResponse(request, nullptr)) {
        error = L"WinHttpReceiveResponse error=" + std::to_wstring(GetLastError());
        ok = false;
    }

    DWORD status = 0;
    if (ok) {
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            error = L"WinHttpQueryHeaders error=" + std::to_wstring(GetLastError());
            ok = false;
        }
    }
    // A redirect or an error page is not a release; only 200 carries the body
    // this parser expects.
    if (ok && status != 200) {
        error = L"HTTP " + std::to_wstring(status);
        ok = false;
    }

    std::string body;
    if (ok) {
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                error = L"WinHttpQueryDataAvailable error=" + std::to_wstring(GetLastError());
                ok = false;
                break;
            }
            if (available == 0) break;
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) {
                error = L"WinHttpReadData error=" + std::to_wstring(GetLastError());
                ok = false;
                break;
            }
            if (read == 0) break;
            body.append(chunk.data(), read);
            // The release payload is a few KB; a runaway response is dropped
            // rather than allowed to grow without bound.
            if (body.size() > 4u * 1024u * 1024u) break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok) return false;
    if (body.empty()) { error = L"empty response"; return false; }

    const int wide = MultiByteToWideChar(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), nullptr, 0);
    if (wide <= 0) { error = L"invalid UTF-8 body"; return false; }
    std::wstring json(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), json.data(), wide);
    if (!ExtractTagName(json, tag)) { error = L"tag_name missing"; return false; }
    return true;
}

unsigned __stdcall UpdateCheckThreadProc(void*) {
    // The first check runs immediately; later iterations wait a full day after
    // the previous check, which is what caps the request rate. While the
    // feature is disabled the loop only polls the flag once a second.
    for (;;) {
        if (g_updateCheckEnabled.load(std::memory_order_relaxed)) {
            std::wstring tag, error;
            if (!FetchLatestReleaseTag(tag, error)) {
                WriteDiagnostic(L"github update check failed: " + error);
            } else {
                std::wstring latest = Trim(tag);
                if (!latest.empty() && (latest[0] == L'v' || latest[0] == L'V')) latest.erase(latest.begin());
                std::vector<int> parsed;
                if (latest.empty() || !ParseVersion(latest, parsed)) {
                    WriteDiagnostic(L"github update check returned an unrecognised tag");
                } else if (CompareVersions(latest, ProductVersion()) > 0) {
                    auto* payload = new (std::nothrow) std::wstring(tag);
                    if (payload && !PostMessageW(g_trayWindow, kUpdateCheckMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                        delete payload;
                    }
                }
            }
        }
        const DWORD waitMs = g_updateCheckEnabled.load(std::memory_order_relaxed) ? kUpdateCheckIntervalMs : 1000;
        if (WaitForSingleObject(g_updateStopEvent, waitMs) == WAIT_OBJECT_0) break;
    }
    return 0;
}

void StartUpdateCheckThread() {
    if (g_updateThread) return;
    g_updateStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_updateStopEvent) return;
    unsigned threadId = 0;
    g_updateThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &UpdateCheckThreadProc, nullptr, 0, &threadId));
    if (!g_updateThread) {
        CloseHandle(g_updateStopEvent);
        g_updateStopEvent = nullptr;
    }
}

void StopUpdateCheckThread() {
    if (g_updateStopEvent) SetEvent(g_updateStopEvent);
    if (g_updateThread) {
        WaitForSingleObject(g_updateThread, 30000);
        CloseHandle(g_updateThread);
        g_updateThread = nullptr;
    }
    if (g_updateStopEvent) {
        CloseHandle(g_updateStopEvent);
        g_updateStopEvent = nullptr;
    }
}

// --update-self-test: no network. The fixed table exercises the JSON tag scan
// and the numeric comparison, including the malformed inputs that must not
// crash or offer an update.
int UpdateSelfTest() {
    size_t failures = 0;

    struct TagCase { const wchar_t* json; const wchar_t* expected; };
    const TagCase tagCases[] = {
        { L"{\"tag_name\": \"v1.2.0\"}", L"v1.2.0" },
        { L"{ \"tag_name\" : \"1.2.0\" }", L"1.2.0" },
        { L"{\"tag_name\": \"\"}", L"" },
        { L"{\"message\": \"Not Found\"}", nullptr },
    };
    for (const TagCase& test : tagCases) {
        std::wstring parsed;
        const bool found = ExtractTagName(test.json, parsed);
        const bool ok = test.expected == nullptr ? !found : (found && parsed == test.expected);
        if (!ok) ++failures;
        std::wprintf(L"tag %ls json=%ls -> %ls\n", ok ? L"ok" : L"FAIL", test.json,
                     found ? parsed.c_str() : L"<missing>");
    }

    struct VersionCase { const wchar_t* left; const wchar_t* right; int expected; };
    const VersionCase versionCases[] = {
        { L"v1.2.0", L"1.1.0", 1 },
        { L"1.1.0", L"1.1.0", 0 },
        { L"1.0.9", L"1.1.0", -1 },
        { L"1.10.0", L"1.9.9", 1 },
        { L"2.0", L"1.9.9", 1 },
        { L"1.1.0", L"1.1.0.0", 0 },
        { L"", L"1.1.0", 0 },
        { L"garbage", L"1.1.0", 0 },
        { L"v1.2.0-beta", L"1.1.0", 1 },
    };
    for (const VersionCase& test : versionCases) {
        const int result = CompareVersions(test.left, test.right);
        const bool ok = result == test.expected;
        if (!ok) ++failures;
        const wchar_t* verdict = result > 0 ? L"newer" : result < 0 ? L"older" : L"equal";
        std::wprintf(L"version %ls vs %ls -> %ls %ls\n", test.left, test.right, verdict, ok ? L"ok" : L"FAIL");
    }

    std::wprintf(L"failures=%zu\n", failures);
    return failures == 0 ? 0 : 1;
}

// --check-updates: synchronous real-network diagnostic. The resident check
// ignores this command's outcome; the diagnostic deliberately runs even when
// `check_updates` is false so the endpoint can always be probed by hand.
int UpdateCheckDiagnostic() {
    std::wstring tag, error;
    if (!FetchLatestReleaseTag(tag, error)) {
        std::wprintf(L"version=%ls latest=? status=failed\n", ProductVersion().c_str());
        std::wprintf(L"reason=%ls\n", error.c_str());
        return 4;
    }
    std::wstring latest = Trim(tag);
    if (!latest.empty() && (latest[0] == L'v' || latest[0] == L'V')) latest.erase(latest.begin());
    std::vector<int> parsed;
    if (latest.empty() || !ParseVersion(latest, parsed)) {
        std::wprintf(L"version=%ls latest=? status=failed\n", ProductVersion().c_str());
        return 4;
    }
    const int comparison = CompareVersions(latest, ProductVersion());
    const wchar_t* status = comparison > 0 ? L"newer" : L"current";
    std::wprintf(L"version=%ls latest=%ls status=%ls\n", ProductVersion().c_str(), latest.c_str(), status);
    std::wprintf(L"menu_label=%ls\n", FormatText(EnglishText(TextId::MenuDownloadFormat), StripVersionPrefix(tag).c_str()).c_str());
    std::wprintf(L"balloon=%ls\n", FormatText(EnglishText(TextId::BalloonUpdateFormat), DisplayTag(tag).c_str()).c_str());
    return comparison > 0 ? 3 : 0;
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
    else if (k == L"active_backdrop_alpha") rule.activeBackdropAlpha = ParseDouble(value, rule.activeBackdropAlpha, 0.0, 1.0);
    else if (k == L"inactive_backdrop_alpha") rule.inactiveBackdropAlpha = ParseDouble(value, rule.inactiveBackdropAlpha, 0.0, 1.0);
    else if (k == L"active_glass_opacity") rule.activeGlassOpacity = ParseDouble(value, rule.activeGlassOpacity, 0.05, 1.0);
    else if (k == L"inactive_glass_opacity") rule.inactiveGlassOpacity = ParseDouble(value, rule.inactiveGlassOpacity, 0.05, 1.0);
    else if (k == L"active_tint_color") rule.activeTint = ParseColor(value, rule.activeTint);
    else if (k == L"inactive_tint_color") rule.inactiveTint = ParseColor(value, rule.inactiveTint);
    else if (k == L"active_tint_strength") rule.activeTintStrength = ParseDouble(value, rule.activeTintStrength, 0.0, 1.0);
    else if (k == L"inactive_tint_strength") rule.inactiveTintStrength = ParseDouble(value, rule.inactiveTintStrength, 0.0, 1.0);
    else if (k == L"transition_ms") rule.transitionMs = ParseInt(value, rule.transitionMs, 0, 5000);
    else if (k == L"exclude_fullscreen" || k == L"exclude_fullscreen_video") rule.excludeFullscreen = ParseBool(value, rule.excludeFullscreen);
}

// Which window property a matcher key addresses. Keeping the accepted spellings
// in one place is what lets the YAML reader ask "is this key a matcher?" without
// repeating the list.
enum class MatchField { None, Process, ClassName, Title, Aumid };

MatchField ClassifyMatchKey(const std::wstring& key) {
    const auto k = Lower(Trim(key));
    if (k == L"process" || k == L"match_process") return MatchField::Process;
    if (k == L"class_name" || k == L"class" || k == L"match_class" || k == L"match_class_name") return MatchField::ClassName;
    if (k == L"title" || k == L"match_title" || k == L"title_contains") return MatchField::Title;
    if (k == L"aumid" || k == L"match_aumid" || k == L"package" || k == L"package_family_name") return MatchField::Aumid;
    return MatchField::None;
}

// Matches the *window* rather than its appearance.  Both configuration
// flavours accept these keys: YAML writes them inside the `match:` block, INI
// writes them as extra keys of an [app:...] section.  Returns false when the
// key is not a matcher, so the YAML reader can fall through to appearance.
bool SetMatchField(AppRule& entry, const std::wstring& key, const std::wstring& rawValue) {
    const auto value = Lower(UnquoteYaml(rawValue));
    switch (ClassifyMatchKey(key)) {
    case MatchField::Process: entry.process = value; return true;
    case MatchField::ClassName: entry.className = value; return true;
    case MatchField::Title: entry.title = value; return true;
    case MatchField::Aumid: entry.aumid = value; return true;
    case MatchField::None: break;
    }
    return false;
}

// An entry whose matchers are all empty would match every window and silently
// swallow whatever reached it, so it is dropped rather than honoured.
void FinalizeConfig(Config& config) {
    config.apps.erase(std::remove_if(config.apps.begin(), config.apps.end(),
                                     [](const AppRule& entry) { return !entry.AnyMatcher(); }),
                      config.apps.end());
    // A precise rule must win over a broad one regardless of file order: a
    // title rule listed above a process rule must not shadow it.
    std::stable_sort(config.apps.begin(), config.apps.end(),
                     [](const AppRule& left, const AppRule& right) { return left.Rank() < right.Rank(); });
    config.anyTitleMatcher = std::any_of(config.apps.begin(), config.apps.end(),
                                         [](const AppRule& entry) { return !entry.title.empty(); });
}

bool SameFileTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

// Configuration files are read as UTF-8. std::wifstream widens every byte
// instead, which quietly turns a Chinese window title written into a rule into
// byte soup that can never match a real caption - and a title rule is exactly
// where a non-ASCII value is most likely. A UTF-8 BOM is tolerated, and a file
// that is not valid UTF-8 falls back to the active code page so an older ANSI
// configuration still loads.
bool ReadConfigText(const std::wstring& path, std::wstring& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) offset = 3;
    text.clear();
    const int size = static_cast<int>(bytes.size() - offset);
    if (size <= 0) return true;
    const char* source = bytes.data() + offset;
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int wide = MultiByteToWideChar(codePage, flags, source, size, nullptr, 0);
    if (wide <= 0) { codePage = CP_ACP; flags = 0; wide = MultiByteToWideChar(codePage, flags, source, size, nullptr, 0); }
    if (wide <= 0) return false;
    text.assign(static_cast<size_t>(wide), L'\0');
    return MultiByteToWideChar(codePage, flags, source, size, text.data(), wide) > 0;
}

bool LoadIniConfig(Config& result, const std::wstring& path) {
    std::wstring text;
    if (!ReadConfigText(path, text)) return false;
    std::wistringstream input(text);
    Config next;
    std::wstring section = L"global";
    int appIndex = -1;
    std::wstring line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            appIndex = -1;
            if (section.rfind(L"app:", 0) == 0) {
                const auto name = Trim(section.substr(4));
                AppRule entry;
                // "[app:*]" seeds a rule that is matched by its other keys
                // alone, which is how a title-only rule is written in INI.
                if (name != L"*" && !name.empty()) entry.process = Lower(name);
                entry.rule = next.global;
                next.apps.push_back(std::move(entry));
                appIndex = static_cast<int>(next.apps.size()) - 1;
            }
            continue;
        }
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        if (section == L"global") {
            // ui_language and check_updates belong to the whole configuration,
            // not to a Rule, so they are handled here instead of inside
            // SetField(). check_updates is a normal bool parsed by ParseBool.
            if (Lower(key) == L"ui_language") next.uiLanguage = Lower(Trim(value));
            else if (Lower(key) == L"check_updates") next.checkUpdates = ParseBool(value, next.checkUpdates);
            else SetField(next.global, key, value);
        }
        else if (appIndex >= 0) { SetField(next.apps[appIndex].rule, key, value); SetMatchField(next.apps[appIndex], key, value); }
        else if (section == L"blacklist") next.blacklist[Lower(key)] = ParseBool(value, true);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) next.writeTime = data.ftLastWriteTime;
    FinalizeConfig(next);
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
    else if (k == L"backdrop_alpha") { if (focused) rule.activeBackdropAlpha = ParseFlexibleOpacity(value, rule.activeBackdropAlpha); else rule.inactiveBackdropAlpha = ParseFlexibleOpacity(value, rule.inactiveBackdropAlpha); }
    // Preserve the established configuration contract: glass_opacity affects
    // tint density only. New configurations use backdrop_alpha to control the
    // Acrylic material's visibility without changing the tint calculation.
    else if (k == L"glass_opacity") { if (focused) rule.activeGlassOpacity = ParseFlexibleOpacity(value, rule.activeGlassOpacity); else rule.inactiveGlassOpacity = ParseFlexibleOpacity(value, rule.inactiveGlassOpacity); }
    else if (k == L"tint_opacity" || k == L"tint_strength") { if (focused) rule.activeTintStrength = ParseFlexibleOpacity(value, rule.activeTintStrength); else rule.inactiveTintStrength = ParseFlexibleOpacity(value, rule.inactiveTintStrength); }
    else if (k == L"animation_duration_ms" || k == L"transition_ms") rule.transitionMs = ParseInt(value, rule.transitionMs, 0, 5000);
    else if (k == L"exclude_fullscreen" || k == L"exclude_fullscreen_video") rule.excludeFullscreen = ParseBool(value, rule.excludeFullscreen);
}

bool LoadYamlConfig(Config& result, const std::wstring& path) {
    std::wstring text;
    if (!ReadConfigText(path, text)) return false;
    std::wistringstream input(text);
    Config next;
    std::wstring section, state;
    AppRule appEntry;
    bool appOpen = false;
    // Entries are collected in file order here and only ordered by specificity
    // once the whole file has been read.
    auto commitApp = [&]() { if (appOpen && appEntry.AnyMatcher()) next.apps.push_back(appEntry); appOpen = false; appEntry = AppRule{}; state.clear(); };
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
        const bool listItem = !item.empty() && item.front() == L'-';
        if (listItem) item = Trim(item.substr(1));
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
            else if (key == L"ui_language" && state.empty()) next.uiLanguage = Lower(UnquoteYaml(value));
            else if (key == L"check_updates" && state.empty()) next.checkUpdates = ParseBool(UnquoteYaml(value), next.checkUpdates);
            else if (state == L"focused" || state == L"unfocused") SetYamlStateField(next.global, state == L"focused", key, value);
            continue;
        }
        if (section == L"applications") {
            // A list item opens a new entry only when it names a matcher, the
            // match block, or an appearance group. The `- type:` items nested
            // under `rules:` are list items too, and they must stay inside the
            // entry already open - treating every "- " line as a new entry
            // silently moved a rule's appearance onto a following rule.
            const bool opensEntry = listItem && (key == L"match" || key == L"focused" || key == L"unfocused" ||
                                                 ClassifyMatchKey(key) != MatchField::None);
            if (opensEntry) {
                commitApp();
                appOpen = true;
                appEntry = AppRule{};
                appEntry.rule = next.global;
                state.clear();
            }
            if (key == L"match") { state = L"match"; continue; }
            // A matcher written directly on the list item is the short form and
            // opens an entry on its own.
            if (appOpen && (state == L"match" || opensEntry) && SetMatchField(appEntry, key, value)) { state = L"match"; continue; }
            if (key == L"focused" || key == L"unfocused") { state = key; continue; }
            if (key == L"type") { state = Lower(UnquoteYaml(value)); continue; }
            if (key == L"config" || key == L"rules") continue;
            if (appOpen && (state == L"focused" || state == L"unfocused")) SetYamlStateField(appEntry.rule, state == L"focused", key, value);
        }
    }
    commitApp();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) next.writeTime = data.ftLastWriteTime;
    FinalizeConfig(next);
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
    // The worker reads this before every request, so a hot reload takes effect
    // without the main loop waiting for a join.
    g_updateCheckEnabled.store(g_config.checkUpdates);
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
    wcscpy_s(icon.szTip, Str(TextId::TrayTooltip));
    return icon;
}

// Reuses the existing tray icon and shows an informational balloon. Runs on
// the main thread only, after kUpdateCheckMessage has delivered the tag.
void ShowUpdateBalloon(const std::wstring& tag) {
    if (!g_trayWindow || !g_trayIconAdded) return;
    auto icon = MakeTrayIconData();
    icon.uFlags |= NIF_INFO;
    icon.dwInfoFlags = NIIF_INFO;
    const std::wstring title = Str(TextId::BalloonUpdateTitle);
    const std::wstring text = FormatText(Str(TextId::BalloonUpdateFormat), DisplayTag(tag).c_str());
    wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
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
    AppendMenuW(menu, MF_STRING, kTrayEditor, Str(TextId::MenuEditor));
    AppendMenuW(menu, MF_STRING, kTrayOpenConfig, Str(TextId::MenuConfig));
    AppendMenuW(menu, MF_STRING, kTrayReload, Str(TextId::MenuReload));
    if (!g_updateVersion.empty()) {
        // A strictly newer release is the only condition that adds this item,
        // and it stays for the rest of the session once found.
        AppendMenuW(menu, MF_STRING, kTrayDownload,
                    FormatText(Str(TextId::MenuDownloadFormat), StripVersionPrefix(g_updateVersion).c_str()).c_str());
    }
    const bool startupEnabled = StartupEnabledForMenu();
    AppendMenuW(menu, MF_STRING, kTrayStartup,
                startupEnabled ? Str(TextId::MenuStartupOff) : Str(TextId::MenuStartupOn));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, Str(TextId::MenuExit));
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
    if (command) PostMessageW(hwnd, WM_COMMAND, command, 0);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage && message == g_taskbarCreatedMessage) { g_trayIconAdded = false; AddTrayIcon(); return 0; }
    if (message == kUpdateCheckMessage) {
        // The worker owns nothing user-visible: it hands over a heap tag and
        // the main thread publishes the version, then shows the balloon.
        std::wstring* payload = reinterpret_cast<std::wstring*>(lParam);
        if (payload) {
            if (!payload->empty()) {
                g_updateVersion = *payload;
                ShowUpdateBalloon(*payload);
            }
            delete payload;
        }
        return 0;
    }
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
        case kTrayDownload:
            // Always open the repository's latest-release page; the exact tag
            // found by the check is only used for the label.
            ShellExecuteW(hwnd, L"open", L"https://github.com/heathacosta259519-dot/WinGlass/releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
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
                            enableStartup ? Str(TextId::StartupEnableFailed) : Str(TextId::StartupDisableFailed),
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

std::wstring WindowClassName(HWND hwnd) {
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    return Lower(cls);
}

// GetWindowText does not send WM_GETTEXT across processes: for a foreign window
// User32 returns the cached caption, so this stays cheap and cannot be blocked
// by an unresponsive application.
std::wstring WindowTitle(HWND hwnd) {
    wchar_t title[512]{};
    const int length = GetWindowTextW(hwnd, title, 512);
    if (length <= 0) return L"";
    return Lower(std::wstring(title, static_cast<size_t>(length)));
}

// Store/MSIX applications all arrive as ApplicationFrameHost.exe, and the
// package identity that tells them apart belongs to the *hosted* process. The
// hosted application draws into a child window owned by that other process, so
// the first child with a different process id is the one to ask.
struct HostedProcessProbe {
    DWORD hostPid = 0;
    DWORD hostedPid = 0;
};

BOOL CALLBACK HostedProcessProc(HWND child, LPARAM value) {
    auto& probe = *reinterpret_cast<HostedProcessProbe*>(value);
    DWORD childPid = 0;
    GetWindowThreadProcessId(child, &childPid);
    if (childPid && childPid != probe.hostPid) { probe.hostedPid = childPid; return FALSE; }
    return TRUE;
}

std::wstring AumidForWindow(HWND hwnd, const std::wstring& process) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    if (process == L"applicationframehost.exe") {
        HostedProcessProbe probe{};
        probe.hostPid = pid;
        EnumChildWindows(hwnd, HostedProcessProc, reinterpret_cast<LPARAM>(&probe));
        if (!probe.hostedPid) return L"";
        pid = probe.hostedPid;
    }
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!processHandle) return L"";
    // The API reports the size it needs, which avoids depending on a maximum
    // constant that not every SDK header defines.
    UINT32 length = 0;
    std::wstring aumid;
    if (GetApplicationUserModelId(processHandle, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0) {
        aumid.assign(length, L'\0');
        UINT32 written = length;
        if (GetApplicationUserModelId(processHandle, &written, aumid.data()) != ERROR_SUCCESS) aumid.clear();
    }
    CloseHandle(processHandle);
    // c_str() drops the terminator the API counted, then normalise for the
    // case-insensitive comparison against configuration values.
    return aumid.empty() ? std::wstring() : Lower(std::wstring(aumid.c_str()));
}

// Forward declaration because full-screen exclusion is part of the generic
// window filter, while the application-specific rule lookup is defined below.
Rule RuleFor(HWND hwnd, std::wstring& process);

bool IsKnownBrowserProcess(const std::wstring& process) {
    // Chromium/Firefox video full-screen windows are normally borderless, but
    // some browser builds keep an overlapped style while they resize the same
    // top-level HWND.  The process check makes that transition safe without
    // touching ordinary maximized browser windows.
    return process == L"msedge.exe" || process == L"chrome.exe" ||
           process == L"firefox.exe" || process == L"brave.exe" ||
           process == L"opera.exe" || process == L"vivaldi.exe";
}

bool CoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
    // DWM's extended frame can differ from the monitor by a few physical
    // pixels because of invisible resize borders and mixed-DPI rounding.
    constexpr int kFullscreenTolerancePx = 8;
    return std::abs(windowRect.left - monitorRect.left) <= kFullscreenTolerancePx &&
           std::abs(windowRect.top - monitorRect.top) <= kFullscreenTolerancePx &&
           std::abs(windowRect.right - monitorRect.right) <= kFullscreenTolerancePx &&
           std::abs(windowRect.bottom - monitorRect.bottom) <= kFullscreenTolerancePx;
}

bool IsFullscreenSurface(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return false;
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo) ||
        !CoversMonitor(windowRect, monitorInfo.rcMonitor)) return false;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const bool borderless = (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
    if (borderless) return true;

    // A browser can briefly retain WS_CAPTION during its full-screen resize.
    // Only accept that case for a known browser process; this prevents a
    // normally maximized business application from being mistaken for video.
    return IsKnownBrowserProcess(ProcessName(hwnd));
}

bool FullscreenExclusionEnabled(HWND hwnd) {
    if (!IsFullscreenSurface(hwnd)) return false;
    std::wstring process;
    const Rule rule = RuleFor(hwnd, process);
    return rule.excludeFullscreen;
}

bool IsTooltipWindowClass(const std::wstring& className) {
    // Native Win32 tooltips use tooltips_class32/msctls_tooltip32. Chromium
    // based shells have used several private names over time, so match the
    // stable "tooltip" token as well. Class matching is deliberately used
    // instead of size/title heuristics to avoid hiding notification popups or
    // small legitimate utility windows.
    return className == L"tooltips_class32" || className == L"msctls_tooltip32" ||
           className.find(L"tooltip") != std::wstring::npos;
}

bool IsExcluded(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return true;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return true;
    const auto c = WindowClassName(hwnd);
    if (IsTooltipWindowClass(c)) return true;
    if (c == L"progman" || c == L"workerw" || c == L"shell_traywnd" ||
        c == L"shell_secondarytraywnd" || c == L"windows.ui.core.corewindow" ||
        c == L"winglassbackdropwindow" || c == L"winglassalphalabpattern" ||
        c == L"winglassalphalabcontrol") return true;
    // Full-screen video is a temporary exclusion: the normal window rule is
    // preserved and will be reapplied as soon as the browser leaves full-screen.
    return FullscreenExclusionEnabled(hwnd);
}

// Collects only the identifiers a rule actually asks for. A rule table that
// matches on the executable name alone therefore never pays for a window title
// read, and the UWP host probe only runs when an entry really needs a package
// identity. This matters because the 8 ms foreground check reaches this code
// whenever the foreground window is full-screen shaped.
class WindowIdentity {
public:
    explicit WindowIdentity(HWND hwnd) : hwnd_(hwnd) {}
    const std::wstring& Process() { if (!processKnown_) { process_ = ProcessName(hwnd_); processKnown_ = true; } return process_; }
    const std::wstring& ClassName() { if (!classKnown_) { class_ = WindowClassName(hwnd_); classKnown_ = true; } return class_; }
    const std::wstring& Title() { if (!titleKnown_) { title_ = WindowTitle(hwnd_); titleKnown_ = true; } return title_; }
    const std::wstring& Aumid() { if (!aumidKnown_) { aumid_ = AumidForWindow(hwnd_, Process()); aumidKnown_ = true; } return aumid_; }

private:
    HWND hwnd_;
    std::wstring process_, class_, title_, aumid_;
    bool processKnown_ = false, classKnown_ = false, titleKnown_ = false, aumidKnown_ = false;
};

// Every matcher that is present must match, cheapest and most specific first,
// so one rejection short-circuits the expensive lookups.
bool MatchesAppRule(const AppRule& entry, WindowIdentity& identity) {
    if (!entry.process.empty() && entry.process != identity.Process()) return false;
    if (!entry.aumid.empty() && entry.aumid != identity.Aumid()) return false;
    if (!entry.className.empty() && entry.className != identity.ClassName()) return false;
    if (!entry.title.empty() && identity.Title().find(entry.title) == std::wstring::npos) return false;
    return true;
}

Rule RuleFor(HWND hwnd, std::wstring& process) {
    WindowIdentity identity(hwnd);
    process = identity.Process();
    Rule rule = g_config.global;
    // Config::apps is ordered by specificity, so the first match is the most
    // precise rule the file describes.
    for (const AppRule& entry : g_config.apps) {
        if (MatchesAppRule(entry, identity)) { rule = entry.rule; break; }
    }
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

// AccentPolicy expects packed ABGR, not the RGB order used by COLORREF. The
// production backdrop remains uncoloured; the alpha lab supplies a neutral
// black tint solely to observe how the system material treats this channel.
DWORD AccentGradientColor(Color color, BYTE alpha) {
    return (static_cast<DWORD>(alpha) << 24) |
           (static_cast<DWORD>(color.b) << 16) |
           (static_cast<DWORD>(color.g) << 8) |
           static_cast<DWORD>(color.r);
}

bool SetBackdropComposition(HWND backdrop, bool acrylic, DWORD gradientColor = 0) {
    const auto setter = GetCompositionSetter();
    if (!setter || !backdrop) return false;

    AccentPolicy policy{};
    policy.accentState = acrylic ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_ENABLE_GRADIENT;
    // Keep production AccentPolicy uncoloured. The separate tint HWND below
    // applies the configured colour without reducing DWM blur contrast. The
    // caller can opt into a gradient only for the isolated alpha experiment.
    policy.accentFlags = 0;
    policy.gradientColor = gradientColor;
    WindowCompositionAttributeData data{WCA_ACCENT_POLICY, &policy, sizeof(policy)};
    if (setter(backdrop, &data)) return true;

    // Some Windows builds reject Acrylic state 4 but accept legacy blur state 3.
    if (acrylic) {
        policy.accentState = ACCENT_ENABLE_BLURBEHIND;
        policy.accentFlags = 0;
        policy.gradientColor = gradientColor;
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

// The pattern is a real top-level window below the test panes. This matters:
// Acrylic must sample a moving compositor surface, rather than a pre-rendered
// bitmap, or the lab would not expose the failure mode we need to evaluate.
LRESULT CALLBACK AlphaLabPatternProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_TIMER && wParam == kAlphaLabPatternTimer) {
        ++g_alphaLab.patternPhase;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        constexpr COLORREF colors[] = {
            RGB(32, 96, 176), RGB(196, 66, 108), RGB(40, 152, 120),
            RGB(218, 150, 48), RGB(102, 76, 190), RGB(42, 146, 182)
        };
        constexpr int cell = 36;
        const int startColumn = -(g_alphaLab.patternPhase % cell);
        const int startRow = -((g_alphaLab.patternPhase / 2) % cell);
        for (int y = startRow; y < client.bottom; y += cell) {
            for (int x = startColumn; x < client.right; x += cell) {
                const int column = (x - startColumn) / cell;
                const int row = (y - startRow) / cell;
                const size_t color = static_cast<size_t>((column + row + g_alphaLab.patternPhase / 6) % std::size(colors));
                HBRUSH brush = CreateSolidBrush(colors[color]);
                if (brush) {
                    RECT square{x, y, x + cell, y + cell};
                    FillRect(dc, &square, brush);
                    DeleteObject(brush);
                }
            }
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// The normal row deliberately reapplies AccentPolicy while pulsing. The
// production effect must never do this at frame rate; showing its visual cost
// side by side with window-alpha updates is the point of the experiment.
bool ApplyAlphaLabPane(const AlphaLabPane& pane, BYTE alpha) {
    if (!pane.hwnd || !IsWindow(pane.hwnd)) return false;
    if (pane.layered) return SetLayeredWindowAttributes(pane.hwnd, 0, alpha, LWA_ALPHA) != FALSE;
    return SetBackdropComposition(pane.hwnd, true, AccentGradientColor(Color{0, 0, 0}, alpha));
}

void PulseAlphaLabPanes(ULONGLONG now) {
    const double phase = static_cast<double>(now - g_alphaLab.pulseStart) * 6.283185307179586 / 2200.0;
    const double scale = 0.20 + 0.80 * ((std::sin(phase) + 1.0) * 0.5);
    bool changed = false;
    for (const AlphaLabPane& pane : g_alphaLab.panes) {
        const BYTE alpha = static_cast<BYTE>(std::clamp(std::lround(pane.baseAlpha * scale), 1l, 255l));
        changed = ApplyAlphaLabPane(pane, alpha) || changed;
    }
    // A single compositor flush keeps both comparison rows on the same visual
    // phase without imposing this cost on the resident effect's normal loop.
    if (changed) DwmFlush();
}

void DestroyAlphaLabSurfaces() {
    if (g_alphaLab.control) KillTimer(g_alphaLab.control, kAlphaLabPulseTimer);
    if (g_alphaLab.pattern) KillTimer(g_alphaLab.pattern, kAlphaLabPatternTimer);
    for (const AlphaLabPane& pane : g_alphaLab.panes) {
        if (pane.hwnd && IsWindow(pane.hwnd)) {
            DisableBackdropComposition(pane.hwnd);
            DestroyWindow(pane.hwnd);
        }
    }
    if (g_alphaLab.pattern && IsWindow(g_alphaLab.pattern)) DestroyWindow(g_alphaLab.pattern);
    g_alphaLab.panes.clear();
    g_alphaLab.pattern = nullptr;
}

LRESULT CALLBACK AlphaLabControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case kAlphaLabPulseCommand:
            g_alphaLab.pulse = SendMessageW(reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_alphaLab.pulse) {
                g_alphaLab.pulseStart = GetTickCount64();
                SetTimer(hwnd, kAlphaLabPulseTimer, 75, nullptr);
            } else {
                KillTimer(hwnd, kAlphaLabPulseTimer);
                for (const AlphaLabPane& pane : g_alphaLab.panes) ApplyAlphaLabPane(pane, pane.baseAlpha);
                DwmFlush();
            }
            return 0;
        case kAlphaLabCloseCommand:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
    }
    if (message == WM_TIMER && wParam == kAlphaLabPulseTimer && g_alphaLab.pulse) {
        PulseAlphaLabPanes(GetTickCount64());
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY) {
        DestroyAlphaLabSurfaces();
        g_alphaLab.control = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureAlphaLabClasses() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW pattern{};
    pattern.lpfnWndProc = AlphaLabPatternProc;
    pattern.hInstance = g_instance;
    pattern.lpszClassName = kAlphaLabPatternClass;
    pattern.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&pattern) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSW control{};
    control.lpfnWndProc = AlphaLabControlProc;
    control.hInstance = g_instance;
    control.lpszClassName = kAlphaLabControlClass;
    control.hCursor = LoadCursor(nullptr, IDC_ARROW);
    control.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    if (!RegisterClassW(&control) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

int RunBackdropAlphaLab() {
    if (!GetCompositionSetter() || !EnsureBackdropClass() || !EnsureAlphaLabClasses()) return 3;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workWidth = work.right - work.left;
    const int paneGap = 12;
    const int paneWidth = std::clamp((workWidth - 180 - paneGap * 6) / 5, 120, 220);
    const int paneHeight = std::clamp(paneWidth * 2 / 3, 90, 150);
    const int patternWidth = paneWidth * 5 + paneGap * 6;
    const int patternHeight = paneHeight * 2 + paneGap * 3;
    const int controlHeight = 170;
    const int left = std::max(work.left + 20, work.left + (workWidth - patternWidth) / 2);
    const int top = work.top + 28;

    g_alphaLab = {};
    g_alphaLab.control = CreateWindowExW(WS_EX_DLGMODALFRAME, kAlphaLabControlClass, Str(TextId::LabTitle),
                                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                          left, top, patternWidth, controlHeight,
                                          nullptr, nullptr, g_instance, nullptr);
    if (!g_alphaLab.control) return 5;

    CreateWindowExW(0, L"STATIC", Str(TextId::LabInstructions), WS_CHILD | WS_VISIBLE,
                    16, 14, patternWidth - 32, 38, g_alphaLab.control, nullptr, g_instance, nullptr);
    CreateWindowExW(0, L"STATIC", Str(TextId::LabNormal), WS_CHILD | WS_VISIBLE,
                    16, 60, 260, 20, g_alphaLab.control, nullptr, g_instance, nullptr);
    CreateWindowExW(0, L"STATIC", Str(TextId::LabLayered), WS_CHILD | WS_VISIBLE,
                    16, 82, 260, 20, g_alphaLab.control, nullptr, g_instance, nullptr);
    CreateWindowExW(0, L"STATIC", Str(TextId::LabLevels), WS_CHILD | WS_VISIBLE,
                    286, 61, 320, 20, g_alphaLab.control, nullptr, g_instance, nullptr);
    HWND pulse = CreateWindowExW(0, L"BUTTON", Str(TextId::LabPulse), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 patternWidth - 370, 108, 210, 26, g_alphaLab.control,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAlphaLabPulseCommand)), g_instance, nullptr);
    CreateWindowExW(0, L"BUTTON", Str(TextId::LabClose), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    patternWidth - 146, 108, 120, 26, g_alphaLab.control,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAlphaLabCloseCommand)), g_instance, nullptr);
    if (pulse) SendMessageW(pulse, BM_SETCHECK, BST_UNCHECKED, 0);

    const int patternTop = top + controlHeight + paneGap;
    g_alphaLab.pattern = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                          kAlphaLabPatternClass, L"", WS_POPUP,
                                          left, patternTop, patternWidth, patternHeight,
                                          g_alphaLab.control, nullptr, g_instance, nullptr);
    if (!g_alphaLab.pattern) {
        DestroyWindow(g_alphaLab.control);
        return 5;
    }
    SetTimer(g_alphaLab.pattern, kAlphaLabPatternTimer, 33, nullptr);

    constexpr BYTE alphas[] = {51, 102, 153, 204, 255};
    for (int row = 0; row < 2; ++row) {
        const bool layered = row == 1;
        for (size_t column = 0; column < std::size(alphas); ++column) {
            const int x = left + paneGap + static_cast<int>(column) * (paneWidth + paneGap);
            const int y = patternTop + paneGap + row * (paneHeight + paneGap);
            const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                                  (layered ? WS_EX_LAYERED : 0);
            HWND pane = CreateWindowExW(exStyle, kBackdropClass, L"", WS_POPUP, x, y, paneWidth, paneHeight,
                                        g_alphaLab.control, nullptr, g_instance, nullptr);
            if (!pane || !SetBackdropComposition(pane, true) ||
                (layered && !SetLayeredWindowAttributes(pane, 0, alphas[column], LWA_ALPHA))) {
                if (pane) DestroyWindow(pane);
                DestroyWindow(g_alphaLab.control);
                return 3;
            }
            if (!layered && !ApplyAlphaLabPane(AlphaLabPane{pane, alphas[column], false}, alphas[column])) {
                DestroyWindow(pane);
                DestroyWindow(g_alphaLab.control);
                return 3;
            }
            g_alphaLab.panes.push_back(AlphaLabPane{pane, alphas[column], layered});

        }
    }

    // The panels are top-level popup windows so Acrylic samples the moving
    // pattern beneath them. Their owner keeps them above the pattern and ties
    // their lifetime to the control window without involving target windows.
    ShowWindow(g_alphaLab.pattern, SW_SHOWNOACTIVATE);
    for (const AlphaLabPane& pane : g_alphaLab.panes) ShowWindow(pane.hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(g_alphaLab.control, SW_SHOWNORMAL);
    UpdateWindow(g_alphaLab.control);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// A non-interactive guard for build verification. It confirms both candidate
// routes are accepted by this Windows build, but intentionally does not claim
// visual success: live blur, flicker, and focus behaviour require the lab on a
// real desktop and human inspection.
int BackdropAlphaLabSelfTest() {
    if (!GetCompositionSetter() || !EnsureBackdropClass()) return 1;
    HWND normal = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                  kBackdropClass, L"", WS_POPUP,
                                  -30000, -30000, 64, 64, nullptr, nullptr, g_instance, nullptr);
    HWND layered = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                   kBackdropClass, L"", WS_POPUP,
                                   -30000, -30000, 64, 64, nullptr, nullptr, g_instance, nullptr);
    const bool normalOk = normal && SetBackdropComposition(normal, true, AccentGradientColor(Color{0, 0, 0}, 153));
    const bool layeredOk = layered && SetBackdropComposition(layered, true) &&
                           SetLayeredWindowAttributes(layered, 0, 153, LWA_ALPHA) != FALSE;
    if (normal) {
        DisableBackdropComposition(normal);
        DestroyWindow(normal);
    }
    if (layered) {
        DisableBackdropComposition(layered);
        DestroyWindow(layered);
    }
    std::wprintf(L"backdrop_alpha_lab normal=%d layered=%d\n", normalOk ? 1 : 0, layeredOk ? 1 : 0);
    return normalOk && layeredOk ? 0 : 1;
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
        // A title rule can start or stop matching with no configuration change
        // at all, so those rules are re-evaluated on every sweep instead of
        // only after a reload. A failure here is finished off by Sweep, which
        // restores the window instead of suspending it.
        const bool stale = it->second.ruleRevision != g_configRevision || g_config.anyTitleMatcher;
        if (refreshRule && stale) {
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
        // Record the package identity once per newly tracked window. Every
        // Store application arrives as ApplicationFrameHost.exe, so this line
        // is where a user reads the value an `aumid` rule expects.
        {
            const std::wstring owner = ProcessName(hwnd);
            if (owner == L"applicationframehost.exe") {
                const std::wstring aumid = AumidForWindow(hwnd, owner);
                if (!aumid.empty()) WriteDiagnostic(L"hosted window package identity: " + aumid);
            }
        }
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
        // The Alpha lab demonstrated that this Windows build preserves live
        // Acrylic on a layered helper while accepting stable window alpha.
        // Keep the material policy separate so this does not add DWM work to
        // the focus-transition hot path.
        fresh.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                         kBackdropClass, L"", WS_POPUP, 0, 0, 0, 0,
                                         nullptr, nullptr, g_instance, nullptr);
        fresh.tint = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                     kBackdropClass, L"", WS_POPUP, 0, 0, 0, 0,
                                     nullptr, nullptr, g_instance, nullptr);
        if (!fresh.backdrop || !fresh.tint) {
            if (fresh.tint) DestroyWindow(fresh.tint);
            if (fresh.backdrop) DestroyWindow(fresh.backdrop);
            RestoreWindowStyle(hwnd, fresh.originalExStyle, fresh.hadLayeredStyle, fresh.originalAlpha, fresh.originalColorKey, fresh.originalLayerFlags);
            return false;
        }
        // Start both layers hidden. ApplyWindow supplies their first real
        // alpha before either helper is shown, avoiding a one-frame flash.
        if (!SetLayeredWindowAttributes(fresh.backdrop, 0, 0, LWA_ALPHA) ||
            !SetLayeredWindowAttributes(fresh.tint, 0, 0, LWA_ALPHA)) {
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
    const double backdropAlpha = active ? rule.activeBackdropAlpha : rule.inactiveBackdropAlpha;
    const BYTE desiredTintAlpha = static_cast<BYTE>(
        std::clamp(tintStrength * glassOpacity * 255.0, 0.0, 255.0));
    const BYTE desiredBackdropAlpha = static_cast<BYTE>(
        std::clamp(backdropAlpha * 255.0, 0.0, 255.0));
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
                                   state->toBackdropAlpha != desiredBackdropAlpha ||
                                   !SameColor(state->toTintColor, desiredTintColor);
    if (isFirstVisual) {
        state->currentOpacity = state->fromOpacity = state->toOpacity = targetOpacity;
        state->tintColor = state->fromTintColor = state->toTintColor = desiredTintColor;
        state->tintAlpha = state->fromTintAlpha = state->toTintAlpha = desiredTintAlpha;
        state->backdropAlpha = state->fromBackdropAlpha = state->toBackdropAlpha = desiredBackdropAlpha;
        state->transitionStart = now;
        state->visualInitialized = true;
    } else if (transitionChanged) {
        state->fromOpacity = state->currentOpacity;
        state->toOpacity = targetOpacity;
        state->fromTintColor = state->tintColor;
        state->toTintColor = desiredTintColor;
        state->fromTintAlpha = state->tintAlpha;
        state->toTintAlpha = desiredTintAlpha;
        state->fromBackdropAlpha = state->backdropAlpha;
        state->toBackdropAlpha = desiredBackdropAlpha;
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
    state->backdropAlpha = LerpByte(state->fromBackdropAlpha, state->toBackdropAlpha, easedProgress);
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
    if (state->backdrop) {
        // Unlike SetWindowCompositionAttribute, this window-alpha update did
        // not flash under the lab's continuous pulse test. Cache successful
        // calls so stable windows make no redundant User32 transitions.
        const bool backdropAlphaChanged = wasSuspended || !state->backdropAlphaApplied ||
                                          state->lastBackdropAlpha != state->backdropAlpha;
        if (backdropAlphaChanged && SetLayeredWindowAttributes(state->backdrop, 0, state->backdropAlpha, LWA_ALPHA)) {
            state->lastBackdropAlpha = state->backdropAlpha;
            state->backdropAlphaApplied = true;
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
    // A lab can run beside the resident instance. Keep every lab HWND out of
    // both target selection and geometric occlusion, or the resident sweep
    // would change the test control while it is meant to compare DWM alone.
    const bool ownWindow = className == Lower(kBackdropClass) || className == Lower(kTrayClass) ||
                           className == Lower(kAlphaLabPatternClass) || className == Lower(kAlphaLabControlClass);
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
        // Mirrors ApplyWindow: a title rule can stop matching without any
        // configuration change, and suspending here would leave the target
        // translucent with its companion windows hidden.
        if (it->second.ruleRevision != g_configRevision || g_config.anyTitleMatcher) {
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

bool BackdropAlphaConfigSelfTest() {
    // Exercise both public spellings without touching the user's on-disk
    // configuration. This keeps a future parser refactor from accepting the
    // new field in YAML but silently dropping the INI compatibility spelling.
    Rule yamlRule{};
    SetYamlStateField(yamlRule, true, L"backdrop_alpha", L"42");
    SetYamlStateField(yamlRule, false, L"backdrop_alpha", L"0.73");
    Rule iniRule{};
    SetField(iniRule, L"active_backdrop_alpha", L"0.42");
    SetField(iniRule, L"inactive_backdrop_alpha", L"0.73");
    return std::abs(yamlRule.activeBackdropAlpha - 0.42) < 0.0001 &&
           std::abs(yamlRule.inactiveBackdropAlpha - 0.73) < 0.0001 &&
           std::abs(iniRule.activeBackdropAlpha - 0.42) < 0.0001 &&
           std::abs(iniRule.inactiveBackdropAlpha - 0.73) < 0.0001;
}

int SelfTest() {
    if (!LoadConfig(g_config, g_configPath)) { std::fwprintf(stderr, L"config load failed: %ls\n", g_configPath.c_str()); return 2; }
    if (!BackdropAlphaConfigSelfTest()) { std::fwprintf(stderr, L"backdrop alpha configuration parsing failed\n"); return 6; }
    if (!RunCompositionSelfTest()) { std::fwprintf(stderr, L"system Acrylic composition unavailable\n"); return 3; }
    std::wprintf(L"config and system Acrylic API ok: %ls global enabled=%d active_target=%.2f backdrop=%.2f glass=%.2f apps=%zu blacklist=%zu classes=%zu transition_ms=%d\n", g_configPath.c_str(), g_config.global.enabled ? 1 : 0, g_config.global.activeOpacity, g_config.global.activeBackdropAlpha, g_config.global.activeGlassOpacity, g_config.apps.size(), g_config.blacklist.size(), g_config.blacklistClasses.size(), g_config.global.transitionMs);
    // Listing the resolved matchers makes a rule that failed to parse visible
    // without having to watch a window behave differently. Specificity order is
    // what decides which entry wins, so it is printed in resolution order.
    for (size_t index = 0; index < g_config.apps.size(); ++index) {
        const AppRule& entry = g_config.apps[index];
        std::wprintf(L"  rule[%zu] process=%ls class=%ls title=%ls aumid=%ls active_target=%.2f glass=%d enabled=%d\n", index,
                     entry.process.empty() ? L"-" : entry.process.c_str(),
                     entry.className.empty() ? L"-" : entry.className.c_str(),
                     entry.title.empty() ? L"-" : entry.title.c_str(),
                     entry.aumid.empty() ? L"-" : entry.aumid.c_str(),
                     entry.rule.activeOpacity,
                     entry.rule.activeAcrylic ? 1 : 0,
                     entry.rule.enabled ? 1 : 0);
    }
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
    // Before anything can show a message: an explicit --lang= wins, otherwise
    // the Windows display language decides. The `ui_language` configuration
    // value is applied a second time once the file has been read, because the
    // elevated startup helper below must keep working with a broken config.
    ResolveUiLanguage(commandLine);
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
    // Now that the file is loaded, let `ui_language` take effect unless the
    // command line carried an explicit tag.
    ResolveUiLanguage(commandLine, &g_config.uiLanguage);
    // The worker starts after the tray window exists; publish the configured
    // switch before it can read the atomic.
    g_updateCheckEnabled.store(g_config.checkUpdates);
    if (wcsstr(commandLine, L"--lang-self-test")) return LanguageSelfTest();
    if (wcsstr(commandLine, L"--self-test")) return SelfTest();
    if (wcsstr(commandLine, L"--update-self-test")) return UpdateSelfTest();
    if (wcsstr(commandLine, L"--check-updates")) return UpdateCheckDiagnostic();
    if (wcsstr(commandLine, L"--backdrop-alpha-lab-self-test")) return BackdropAlphaLabSelfTest();
    if (wcsstr(commandLine, L"--backdrop-alpha-lab")) {
        // This diagnostic must be launched from the real user desktop. Unlike
        // the resident process it does not relaunch through Explorer, because
        // a relaunch would lose the requested lab command-line mode.
        if (!CanSeeInteractiveDesktop()) return 4;
        return RunBackdropAlphaLab();
    }
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
    StartUpdateCheckThread();
    MSG msg{};
    ULONGLONG lastTrack = 0, lastSweep = 0, lastConfigCheck = 0, lastTrayRetry = 0;
    HWND lastForeground = nullptr;
    bool lastForegroundFullscreen = false;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        const ULONGLONG now = GetTickCount64();
        const HWND foreground = GetForegroundWindow();
        // Full-screen video often keeps the same browser HWND and therefore
        // does not generate a foreground change.  Check only the foreground
        // geometry on the existing 8 ms cadence and request one immediate
        // sweep when the full-screen state toggles; no process scan is added
        // to the steady-state path for ordinary windows.
        const bool foregroundFullscreen = FullscreenExclusionEnabled(foreground);
        if (foregroundFullscreen != lastForegroundFullscreen) {
            lastForegroundFullscreen = foregroundFullscreen;
            g_sweepRequested = true;
        }
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
    // Join the worker before the tray window is destroyed, then drain any tag
    // it queued meanwhile so the heap payloads are not leaked.
    StopUpdateCheckThread();
    MSG pending{};
    while (PeekMessageW(&pending, g_trayWindow, kUpdateCheckMessage, kUpdateCheckMessage, PM_REMOVE)) {
        delete reinterpret_cast<std::wstring*>(pending.lParam);
    }
    DestroyTrayIcon();
    ReleaseMutex(singleInstance); CloseHandle(singleInstance);
    return 0;
}
