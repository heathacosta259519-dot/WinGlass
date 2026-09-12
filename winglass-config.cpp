#define _UNICODE
#define UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include "winglass-resource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

struct Color {
    int r = 53;
    int g = 62;
    int b = 98;
};

struct Appearance {
    bool glass = true;
    double targetOpacity = 0.70;
    Color color{};
    double glassOpacity = 0.98;
    double tintOpacity = 0.10;
    int animationMs = 400;
    // When enabled, a borderless window covering an entire monitor is
    // temporarily restored so browser full-screen video stays unmodified.
    bool excludeFullscreen = true;
};

struct AppRule {
    // WinGlass matches permanent rules by executable name, not the ephemeral
    // PID displayed by the process picker. The remaining matchers narrow the
    // rule further: every matcher that is set must match, and a rule may have
    // no process name at all (a title-only rule). The editor preserves them
    // verbatim, so opening a hand-written rule and saving it cannot silently
    // reduce it to its process name.
    std::wstring process;
    std::wstring className;
    std::wstring title;
    std::wstring aumid;
    Appearance focused{};
    Appearance unfocused{};

    bool AnyMatcher() const { return !process.empty() || !className.empty() || !title.empty() || !aumid.empty(); }
};

struct ProcessCandidate {
    std::wstring display;
    std::wstring process;
    std::wstring path;
    DWORD processId = 0;
    // Store and MSIX applications have no process name of their own: this
    // carries the package identity the resident process matches instead.
    std::wstring aumid;
};

struct EditorConfig {
    bool enabled = true;
    Appearance focused{};
    Appearance unfocused{};
    std::vector<std::wstring> blacklistProcesses;
    std::vector<std::wstring> blacklistClasses;
    std::vector<AppRule> appRules;
};

enum ControlId {
    IDC_ENABLED = 100,
    IDC_F_TARGET, IDC_F_GLASS, IDC_F_COLOR, IDC_F_GLASS_OPACITY, IDC_F_TINT, IDC_F_ANIMATION, IDC_F_FULLSCREEN,
    IDC_U_TARGET, IDC_U_GLASS, IDC_U_COLOR, IDC_U_GLASS_OPACITY, IDC_U_TINT, IDC_U_ANIMATION, IDC_U_FULLSCREEN,
    IDC_BLACK_PROCESSES, IDC_BLACK_CLASSES,
    IDC_PICK_PROCESS, IDC_SELECTED_PROCESS, IDC_ADD_PROCESS_BLACKLIST, IDC_ADD_APP_RULE,
    IDC_APP_RULES, IDC_REMOVE_APP_RULE,
    IDC_APP_F_TARGET, IDC_APP_F_GLASS, IDC_APP_F_COLOR, IDC_APP_F_GLASS_OPACITY, IDC_APP_F_TINT, IDC_APP_F_ANIMATION, IDC_APP_F_FULLSCREEN,
    IDC_APP_U_TARGET, IDC_APP_U_GLASS, IDC_APP_U_COLOR, IDC_APP_U_GLASS_OPACITY, IDC_APP_U_TINT, IDC_APP_U_ANIMATION, IDC_APP_U_FULLSCREEN,
    IDC_SAVE, IDC_RELOAD, IDC_OPEN_FOLDER, IDC_EXIT, IDC_STATUS,
    // Appended after the original ids so the values above keep their meaning.
    // These are only ever used inside this process.
    IDC_TABS,
    IDC_APP_MATCH_CLASS, IDC_APP_MATCH_TITLE, IDC_APP_MATCH_AUMID
};

struct Controls {
    HWND enabled = nullptr;
    HWND fTarget = nullptr;
    HWND fGlass = nullptr;
    HWND fColor = nullptr;
    HWND fGlassOpacity = nullptr;
    HWND fTint = nullptr;
    HWND fAnimation = nullptr;
    HWND fFullscreen = nullptr;
    HWND uTarget = nullptr;
    HWND uGlass = nullptr;
    HWND uColor = nullptr;
    HWND uGlassOpacity = nullptr;
    HWND uTint = nullptr;
    HWND uAnimation = nullptr;
    HWND uFullscreen = nullptr;
    HWND blacklistProcesses = nullptr;
    HWND blacklistClasses = nullptr;
    HWND selectedProcess = nullptr;
    HWND pickProcess = nullptr;
    HWND addProcessBlacklist = nullptr;
    HWND addAppRule = nullptr;
    HWND appRules = nullptr;
    HWND removeAppRule = nullptr;
    HWND appFTarget = nullptr;
    HWND appFGlass = nullptr;
    HWND appFColor = nullptr;
    HWND appFGlassOpacity = nullptr;
    HWND appFTint = nullptr;
    HWND appFAnimation = nullptr;
    HWND appFFullscreen = nullptr;
    HWND appUTarget = nullptr;
    HWND appUGlass = nullptr;
    HWND appUColor = nullptr;
    HWND appUGlassOpacity = nullptr;
    HWND appUTint = nullptr;
    HWND appUAnimation = nullptr;
    HWND appUFullscreen = nullptr;
    HWND status = nullptr;
    HWND tabs = nullptr;
    HWND appMatchClass = nullptr;
    HWND appMatchTitle = nullptr;
    HWND appMatchAumid = nullptr;
} g_controls;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HFONT g_font = nullptr;
// The editor is split into one page per concern so that each page owns the
// whole window. The appearance groups, the rule editor and the blacklist lists
// used to compete for the same fixed vertical space, which left no room for the
// matcher fields and would clip longer labels in another language.
enum class EditorPage { Global = 0, Applications = 1, Blacklist = 2 };
constexpr int kEditorPageCount = 3;
std::vector<HWND> g_pageControls[kEditorPageCount];
int g_activePage = 0;
// Cleared while a control that must stay visible on every page is created.
bool g_registerPageControls = true;
std::wstring g_configPath;
std::wstring g_originalText;
EditorConfig g_config;
std::vector<ProcessCandidate> g_processCandidates;
int g_selectedAppRule = -1;
std::wstring g_selectedProcess;
std::wstring g_selectedProcessDisplay;
std::wstring g_selectedAumid;
std::wstring g_pickerChosenProcess;
std::wstring g_pickerChosenDisplay;
std::wstring g_pickerChosenAumid;

static constexpr int IDC_PICKER_LIST = 300;
static constexpr int IDC_PICKER_TABS = 301;
static constexpr int IDC_PICKER_OPEN = 302;
static constexpr int IDC_PICKER_CANCEL = 303;
static constexpr int IDC_PICKER_REFRESH = 304;
HWND g_pickerWindow = nullptr;
HWND g_pickerList = nullptr;
HWND g_pickerTabs = nullptr;
HWND g_pickerOpen = nullptr;
HIMAGELIST g_pickerImages = nullptr;
std::vector<ProcessCandidate> g_pickerCandidates;

// The pages mirror familiar process explorers: applications are the visible
// desktop programs, processes are the complete process snapshot, windows
// exposes each individual visible top-level window, and packages lists the
// installed Store applications by package identity.
enum class PickerPage { Applications, Processes, Windows, Packages };
PickerPage g_pickerPage = PickerPage::Applications;

// The live process picker updates the status line before the UI helper's
// implementation appears later in this single-file editor.
void SetStatus(const std::wstring& text);

std::wstring ProcessPathFromId(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"";
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const bool read = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    return read ? std::wstring(path) : L"";
}

void AddProcessCandidate(const std::wstring& process, DWORD processId, const std::wstring& path = L"") {
    if (process.empty() || processId == 0) return;
    for (const auto& candidate : g_processCandidates) {
        if (candidate.processId == processId) return;
    }
    ProcessCandidate candidate{};
    candidate.process = process;
    candidate.path = path;
    candidate.processId = processId;
    candidate.display = process + L"  [PID " + std::to_wstring(processId) + L"]";
    g_processCandidates.push_back(std::move(candidate));
}

BOOL CALLBACK VisibleWindowProcessProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    const std::wstring path = ProcessPathFromId(processId);
    if (!path.empty()) {
        std::wstring name(path);
        const size_t slash = name.find_last_of(L"\\/");
        AddProcessCandidate(slash == std::wstring::npos ? name : name.substr(slash + 1), processId, path);
    }
    return TRUE;
}

std::wstring ExecutableNameFromPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring HexIdentifier(UINT_PTR value) {
    wchar_t buffer[32]{};
    // Fixed-width hexadecimal IDs make PID/window rows quick to scan, in the
    // same way as conventional process and memory inspection tools.
    swprintf_s(buffer, L"%08llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::wstring Trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Unquote(const std::wstring& value) {
    const std::wstring trimmed = Trim(value);
    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

std::wstring StripYamlComment(const std::wstring& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'"') quoted = !quoted;
        if (!quoted && line[i] == L'#' && (i == 0 || line[i - 1] != L'\\')) {
            return line.substr(0, i);
        }
    }
    return line;
}

bool ParseBool(const std::wstring& value, bool fallback) {
    const std::wstring v = Trim(value);
    if (v == L"true" || v == L"1" || v == L"yes" || v == L"on") return true;
    if (v == L"false" || v == L"0" || v == L"no" || v == L"off") return false;
    return fallback;
}

double ParseDouble(const std::wstring& value, double fallback, double minimum, double maximum) {
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(Trim(value).c_str(), &end);
    if (!end || *end != L'\0') return fallback;
    return std::clamp(parsed, minimum, maximum);
}

int ParseInt(const std::wstring& value, int fallback, int minimum, int maximum) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(Trim(value).c_str(), &end, 10);
    if (!end || *end != L'\0') return fallback;
    return static_cast<int>(std::clamp(parsed, static_cast<long>(minimum), static_cast<long>(maximum)));
}

bool ParseColor(const std::wstring& value, Color& out) {
    std::wstring text = Unquote(value);
    if (!text.empty() && text.front() == L'#') text.erase(text.begin());
    if (text.size() != 6) return false;
    unsigned long parsed = 0;
    wchar_t* end = nullptr;
    parsed = std::wcstoul(text.c_str(), &end, 16);
    if (!end || *end != L'\0' || parsed > 0xFFFFFFul) return false;
    out.r = static_cast<int>((parsed >> 16) & 0xFF);
    out.g = static_cast<int>((parsed >> 8) & 0xFF);
    out.b = static_cast<int>(parsed & 0xFF);
    return true;
}

std::wstring ColorText(const Color& color) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

// Configuration text is UTF-8 on disk. A std::wifstream widens every byte
// instead of decoding, and a std::wofstream truncates every wide character to
// one byte, so either one silently destroys the non-ASCII window title a rule
// may legitimately carry. This mirrors the reader in winglass.cpp; the three
// executables deliberately share no code beyond the resource identifiers.
std::wstring ReadAll(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return L"";
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) offset = 3;
    const int size = static_cast<int>(bytes.size() - offset);
    if (size <= 0) return L"";
    // A file that is not valid UTF-8 falls back to the active code page, so an
    // older ANSI configuration still opens.
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int wide = MultiByteToWideChar(codePage, flags, bytes.data() + offset, size, nullptr, 0);
    if (wide <= 0) { codePage = CP_ACP; flags = 0; wide = MultiByteToWideChar(codePage, flags, bytes.data() + offset, size, nullptr, 0); }
    if (wide <= 0) return L"";
    std::wstring text(static_cast<size_t>(wide), L'\0');
    if (MultiByteToWideChar(codePage, flags, bytes.data() + offset, size, text.data(), wide) <= 0) return L"";
    return text;
}

// Writes UTF-8 with CRLF line endings. Normalising here keeps the file's line
// endings the same no matter what the edit controls handed back.
bool WriteAll(const std::wstring& path, const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size() + text.size() / 16);
    for (const wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') normalized += L"\r\n";
        else normalized += c;
    }
    const int size = static_cast<int>(normalized.size());
    if (size <= 0) return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(), size, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    std::string encoded(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(), size, encoded.data(), bytes, nullptr, nullptr) <= 0) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.flush();
    return output.good();
}

void AddUnique(std::vector<std::wstring>& values, const std::wstring& value) {
    const std::wstring item = Unquote(value);
    if (item.empty()) return;
    if (std::find(values.begin(), values.end(), item) == values.end()) values.push_back(item);
}

void SetAppearanceField(Appearance& appearance, const std::wstring& key, const std::wstring& value) {
    if (key == L"target_opacity" || key == L"opacity") appearance.targetOpacity = ParseDouble(value, appearance.targetOpacity, 0.05, 1.0);
    else if (key == L"enable_glass" || key == L"acrylic") appearance.glass = ParseBool(value, appearance.glass);
    else if (key == L"glass_color" || key == L"tint_color") ParseColor(value, appearance.color);
    else if (key == L"glass_opacity") appearance.glassOpacity = ParseDouble(value, appearance.glassOpacity, 0.05, 1.0);
    else if (key == L"tint_opacity" || key == L"tint_strength") appearance.tintOpacity = ParseDouble(value, appearance.tintOpacity, 0.0, 1.0);
    else if (key == L"animation_duration_ms" || key == L"transition_ms") appearance.animationMs = ParseInt(value, appearance.animationMs, 0, 5000);
    else if (key == L"exclude_fullscreen" || key == L"exclude_fullscreen_video") appearance.excludeFullscreen = ParseBool(value, appearance.excludeFullscreen);
}

// Matcher keys accepted inside a YAML `match:` block or as extra keys of an INI
// [App:...] section. Kept in one place so loading and saving cannot drift.
bool IsMatcherKey(const std::wstring& key) {
    return key == L"process" || key == L"class_name" || key == L"class" || key == L"title" ||
           key == L"title_contains" || key == L"aumid" || key == L"package";
}

bool SetMatcher(AppRule& rule, const std::wstring& key, const std::wstring& rawValue) {
    // The editor keeps the user's own spelling, so unlike the resident process
    // it must not lower-case the value here.
    const std::wstring value = Unquote(rawValue);
    if (key == L"process") { rule.process = value; return true; }
    if (key == L"class_name" || key == L"class") { rule.className = value; return true; }
    if (key == L"title" || key == L"title_contains") { rule.title = value; return true; }
    if (key == L"aumid" || key == L"package") { rule.aumid = value; return true; }
    return false;
}

bool LoadYaml(EditorConfig& result, const std::wstring& text) {
    result = {};
    enum class Section { None, Global, Focused, Unfocused, Blacklist, Applications };
    Section section = Section::None;
    AppRule currentRule{};
    bool appOpen = false;
    bool appFocused = true;
    const auto commitRule = [&]() {
        if (appOpen && currentRule.AnyMatcher()) result.appRules.push_back(std::move(currentRule));
        currentRule = {};
        appOpen = false;
    };
    std::wistringstream input(text);
    std::wstring line;
    while (std::getline(input, line)) {
        const std::wstring clean = StripYamlComment(line);
        const std::wstring body = Trim(clean);
        if (body.empty()) continue;
        const size_t indent = clean.find_first_not_of(L" \t");
        if (indent == 0 && body == L"applications:") {
            section = Section::Applications;
            continue;
        }
        if (indent == 0 && body == L"global:") { section = Section::Global; continue; }
        if (indent == 0 && body == L"blacklist:") { section = Section::Blacklist; continue; }
        std::wstring item = body;
        const bool listItem = !item.empty() && item.front() == L'-';
        if (listItem) item = Trim(item.substr(1));
        const size_t colon = item.find(L':');
        const std::wstring key = colon == std::wstring::npos ? item : Trim(item.substr(0, colon));
        const std::wstring value = colon == std::wstring::npos ? L"" : Trim(item.substr(colon + 1));
        if (section == Section::Blacklist) {
            if (key == L"process") AddUnique(result.blacklistProcesses, value);
            else if (key == L"class_name") AddUnique(result.blacklistClasses, value);
            continue;
        }
        if (section == Section::Global) {
            if (key == L"enabled") result.enabled = ParseBool(value, result.enabled);
            else if (key == L"focused") section = Section::Focused;
            else if (key == L"unfocused") section = Section::Unfocused;
            continue;
        }
        if (section == Section::Focused) {
            if (key == L"unfocused") section = Section::Unfocused;
            else SetAppearanceField(result.focused, key, value);
            continue;
        }
        if (section == Section::Unfocused) {
            SetAppearanceField(result.unfocused, key, value);
            continue;
        }
        if (section == Section::Applications) {
            // A list item opens a new rule when it names a matcher or the match
            // block. The "- type:" items nested under rules: are list items too
            // and must stay inside the rule that is already open.
            const bool opensRule = listItem && (key == L"match" || IsMatcherKey(key));
            if (opensRule) {
                commitRule();
                appOpen = true;
                currentRule.focused = result.focused;
                currentRule.unfocused = result.unfocused;
            }
            if (key == L"match") continue;
            if (appOpen && SetMatcher(currentRule, key, value)) continue;
            if (key == L"type") {
                appFocused = Unquote(value) != L"unfocused";
                continue;
            }
            if (key == L"rules" || key == L"config") continue;
            if (appOpen) SetAppearanceField(appFocused ? currentRule.focused : currentRule.unfocused, key, value);
        }
    }
    commitRule();
    return true;
}

bool LoadIniFallback(EditorConfig& result, const std::wstring& text) {
    result = {};
    std::wistringstream input(text);
    std::wstring line;
    std::wstring section;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') { section = line.substr(1, line.size() - 2); continue; }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        const std::wstring key = Trim(line.substr(0, equals));
        const std::wstring value = Trim(line.substr(equals + 1));
        if (section == L"Global") {
            if (key == L"enabled") result.enabled = ParseBool(value, result.enabled);
            else if (key == L"active_opacity") result.focused.targetOpacity = ParseDouble(value, result.focused.targetOpacity, 0.05, 1.0);
            else if (key == L"inactive_opacity") result.unfocused.targetOpacity = ParseDouble(value, result.unfocused.targetOpacity, 0.05, 1.0);
            else if (key == L"active_acrylic") result.focused.glass = ParseBool(value, result.focused.glass);
            else if (key == L"inactive_acrylic") result.unfocused.glass = ParseBool(value, result.unfocused.glass);
            else if (key == L"active_tint_color") ParseColor(value, result.focused.color);
            else if (key == L"inactive_tint_color") ParseColor(value, result.unfocused.color);
            else if (key == L"active_tint_strength") result.focused.tintOpacity = ParseDouble(value, result.focused.tintOpacity, 0, 1);
            else if (key == L"inactive_tint_strength") result.unfocused.tintOpacity = ParseDouble(value, result.unfocused.tintOpacity, 0, 1);
            else if (key == L"transition_ms") result.focused.animationMs = result.unfocused.animationMs = ParseInt(value, 400, 0, 5000);
            else if (key == L"exclude_fullscreen" || key == L"exclude_fullscreen_video") result.focused.excludeFullscreen = result.unfocused.excludeFullscreen = ParseBool(value, true);
        }
    }
    return true;
}

bool LoadConfigFile(EditorConfig& result, const std::wstring& yamlPath) {
    const std::wstring yaml = ReadAll(yamlPath);
    if (!yaml.empty()) return LoadYaml(result, yaml);
    std::wstring iniPath = yamlPath;
    const size_t slash = iniPath.find_last_of(L"\\/");
    iniPath = (slash == std::wstring::npos ? L"" : iniPath.substr(0, slash + 1)) + L"config.ini";
    const std::wstring ini = ReadAll(iniPath);
    return ini.empty() ? false : LoadIniFallback(result, ini);
}

std::wstring JoinLines(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (const auto& value : values) {
        // std::wofstream in text mode expands \n to CRLF on Windows. Writing
        // \r\n here would produce CRCRLF each time a user clicks Save.
        if (!result.empty()) result += L"\n";
        result += value;
    }
    return result;
}

std::wstring AppearanceYaml(const Appearance& appearance, const wchar_t* name) {
    std::wostringstream out;
    out << L"  " << name << L":\n"
        << L"    target_opacity: " << appearance.targetOpacity << L"\n"
        << L"    enable_glass: " << (appearance.glass ? L"true" : L"false") << L"\n"
        << L"    exclude_fullscreen: " << (appearance.excludeFullscreen ? L"true" : L"false") << L"\n"
        << L"    glass_color: \"" << ColorText(appearance.color) << L"\"\n"
        << L"    glass_opacity: " << appearance.glassOpacity << L"\n"
        << L"    tint_opacity: " << appearance.tintOpacity << L"\n"
        << L"    animation_duration_ms: " << appearance.animationMs << L"\n";
    return out.str();
}

std::wstring AppearanceFieldsYaml(const Appearance& appearance, int spaces) {
    const std::wstring indent(static_cast<size_t>(spaces), L' ');
    std::wostringstream out;
    out << indent << L"target_opacity: " << appearance.targetOpacity << L"\n"
        << indent << L"enable_glass: " << (appearance.glass ? L"true" : L"false") << L"\n"
        << indent << L"exclude_fullscreen: " << (appearance.excludeFullscreen ? L"true" : L"false") << L"\n"
        << indent << L"glass_color: \"" << ColorText(appearance.color) << L"\"\n"
        << indent << L"glass_opacity: " << appearance.glassOpacity << L"\n"
        << indent << L"tint_opacity: " << appearance.tintOpacity << L"\n"
        << indent << L"animation_duration_ms: " << appearance.animationMs << L"\n";
    return out.str();
}

bool SaveConfigFile(const EditorConfig& config, const std::wstring& path) {
    const auto safeYamlName = [](const std::wstring& value) {
        // Process and class names are emitted as quoted YAML scalars. Reject
        // control/quote characters instead of allowing hand-edited input to
        // escape the scalar and inject unrelated configuration keys.
        return !value.empty() && value.find_first_of(L"\"\r\n") == std::wstring::npos;
    };
    for (const auto& process : config.blacklistProcesses) if (!safeYamlName(process)) return false;
    for (const auto& className : config.blacklistClasses) if (!safeYamlName(className)) return false;
    for (const auto& rule : config.appRules) {
        // A rule with no matcher would be dropped by the resident process, so
        // writing one would silently widen the configuration instead of
        // applying it.
        if (!rule.AnyMatcher()) return false;
        for (const std::wstring* matcher : {&rule.process, &rule.className, &rule.title, &rule.aumid}) {
            if (!matcher->empty() && !safeYamlName(*matcher)) return false;
        }
    }

    // Write beside the destination and atomically replace it only after the
    // stream is known-good. A crash or full disk can no longer truncate the
    // user's last valid config.yaml halfway through a save.
    const std::wstring tempPath = path + L".tmp";
    std::wostringstream output;
    output << L"# WinGlass configuration generated by winglass-config.exe\n"
           << L"# Changes are picked up automatically by the running WinGlass process.\n\n"
           << L"blacklist:\n";
    for (const auto& process : config.blacklistProcesses) output << L"  - process: \"" << process << L"\"\n";
    for (const auto& className : config.blacklistClasses) output << L"  - class_name: \"" << className << L"\"\n";
    output << L"\nglobal:\n"
           << L"  enabled: " << (config.enabled ? L"true" : L"false") << L"\n"
           << AppearanceYaml(config.focused, L"focused")
           << AppearanceYaml(config.unfocused, L"unfocused");
    if (!config.appRules.empty()) {
        output << L"\napplications:\n";
        for (const auto& rule : config.appRules) {
            // Only the matchers that are actually set are written back: an empty
            // matcher in the file would read as "match nothing" and change the
            // rule's meaning on the next load.
            output << L"  - match:\n";
            if (!rule.process.empty()) output << L"      process: \"" << rule.process << L"\"\n";
            if (!rule.className.empty()) output << L"      class_name: \"" << rule.className << L"\"\n";
            if (!rule.title.empty()) output << L"      title: \"" << rule.title << L"\"\n";
            if (!rule.aumid.empty()) output << L"      aumid: \"" << rule.aumid << L"\"\n";
            output << L"    rules:\n"
                   << L"      - type: focused\n"
                   << L"        config:\n"
                   << AppearanceFieldsYaml(rule.focused, 10)
                   << L"      - type: unfocused\n"
                   << L"        config:\n"
                   << AppearanceFieldsYaml(rule.unfocused, 10);
        }
    }
    const bool writeSucceeded = WriteAll(tempPath, output.str());
    if (!writeSucceeded) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

// Shows one page's controls and hides the others'. The tab strip and the
// controls that belong to every page are registered to no page, so they are
// never hidden.
void ActivatePage(int page) {
    if (page < 0 || page >= kEditorPageCount) return;
    g_activePage = page;
    for (int i = 0; i < kEditorPageCount; ++i) {
        const int command = i == page ? SW_SHOW : SW_HIDE;
        for (HWND control : g_pageControls[i]) if (control && IsWindow(control)) ShowWindow(control, command);
    }
    if (g_controls.tabs) SendMessageW(g_controls.tabs, TCM_SETCURSEL, page, 0);
    // Group boxes and edit fields leave trails where one page replaces another,
    // because the background belongs to the main window.
    if (g_window) RedrawWindow(g_window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// Which page a control lives on, so validation can reveal the page it is about
// to complain about instead of reporting an error about a hidden field.
int PageOfControl(HWND control) {
    for (int i = 0; i < kEditorPageCount; ++i) {
        if (std::find(g_pageControls[i].begin(), g_pageControls[i].end(), control) != g_pageControls[i].end()) return i;
    }
    return -1;
}

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                 int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    if (control && g_font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    if (control && g_registerPageControls) g_pageControls[g_activePage].push_back(control);
    return control;
}

HWND MakeLabel(const wchar_t* text, int x, int y, int width = 150) {
    return MakeControl(L"STATIC", text, 0, 0, x, y, width, 22);
}

HWND MakeEdit(int id, int x, int y, int width = 100) {
    return MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id, x, y, width, 24);
}

void MakeAppearanceGroup(const wchar_t* title, int baseId, int x, int y) {
    MakeControl(L"BUTTON", title, BS_GROUPBOX, 0, x, y, 365, 248);
    MakeLabel(L"\x76ee\x6807\x900f\x660e\x5ea6 (0.05 - 1.0)", x + 16, y + 32);
    MakeLabel(L"\x542f\x7528\x6bdb\x73bb\x7483", x + 16, y + 62);
    MakeLabel(L"\x73bb\x7483/\x67d3\x8272\x989c\x8272 (#RRGGBB)", x + 16, y + 92);
    MakeLabel(L"\x73bb\x7483\x900f\x660e\x5ea6 (0 - 1)", x + 16, y + 122);
    MakeLabel(L"\x67d3\x8272\x5f3a\x5ea6 (0 - 1)", x + 16, y + 152);
    MakeLabel(L"\x52a8\x753b\x65f6\x957f (\x6beb\x79d2)", x + 16, y + 182);
    MakeLabel(L"\x5168\x5c4f\x89c6\x9891\x65f6\x6392\x9664", x + 16, y + 212);

    HWND target = MakeEdit(baseId, x + 205, y + 29);
    HWND glass = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 1, x + 205, y + 59, 22, 22);
    HWND color = MakeEdit(baseId + 2, x + 205, y + 89, 125);
    HWND glassOpacity = MakeEdit(baseId + 3, x + 205, y + 119);
    HWND tint = MakeEdit(baseId + 4, x + 205, y + 149);
    HWND animation = MakeEdit(baseId + 5, x + 205, y + 179, 125);
    HWND fullscreen = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 6, x + 205, y + 209, 22, 22);
    if (baseId == IDC_F_TARGET) {
        g_controls.fTarget = target; g_controls.fGlass = glass; g_controls.fColor = color;
        g_controls.fGlassOpacity = glassOpacity; g_controls.fTint = tint; g_controls.fAnimation = animation; g_controls.fFullscreen = fullscreen;
    } else {
        g_controls.uTarget = target; g_controls.uGlass = glass; g_controls.uColor = color;
        g_controls.uGlassOpacity = glassOpacity; g_controls.uTint = tint; g_controls.uAnimation = animation; g_controls.uFullscreen = fullscreen;
    }
}

void MakeAppAppearanceGroup(const wchar_t* title, int baseId, int x, int y, bool focused) {
    MakeControl(L"BUTTON", title, BS_GROUPBOX, 0, x, y, 365, 248);
    MakeLabel(L"\x76ee\x6807\x900f\x660e\x5ea6 (0.05 - 1.0)", x + 16, y + 32);
    MakeLabel(L"\x542f\x7528\x6bdb\x73bb\x7483", x + 16, y + 62);
    MakeLabel(L"\x73bb\x7483/\x67d3\x8272\x989c\x8272 (#RRGGBB)", x + 16, y + 92);
    MakeLabel(L"\x73bb\x7483\x900f\x660e\x5ea6 (0 - 1)", x + 16, y + 122);
    MakeLabel(L"\x67d3\x8272\x5f3a\x5ea6 (0 - 1)", x + 16, y + 152);
    MakeLabel(L"\x52a8\x753b\x65f6\x957f (\x6beb\x79d2)", x + 16, y + 182);
    MakeLabel(L"\x5168\x5c4f\x89c6\x9891\x65f6\x6392\x9664", x + 16, y + 212);

    HWND target = MakeEdit(baseId, x + 205, y + 29);
    HWND glass = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 1, x + 205, y + 59, 22, 22);
    HWND color = MakeEdit(baseId + 2, x + 205, y + 89, 125);
    HWND glassOpacity = MakeEdit(baseId + 3, x + 205, y + 119);
    HWND tint = MakeEdit(baseId + 4, x + 205, y + 149);
    HWND animation = MakeEdit(baseId + 5, x + 205, y + 179, 125);
    HWND fullscreen = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 6, x + 205, y + 209, 22, 22);
    if (focused) {
        g_controls.appFTarget = target; g_controls.appFGlass = glass; g_controls.appFColor = color;
        g_controls.appFGlassOpacity = glassOpacity; g_controls.appFTint = tint; g_controls.appFAnimation = animation; g_controls.appFFullscreen = fullscreen;
    } else {
        g_controls.appUTarget = target; g_controls.appUGlass = glass; g_controls.appUColor = color;
        g_controls.appUGlassOpacity = glassOpacity; g_controls.appUTint = tint; g_controls.appUAnimation = animation; g_controls.appUFullscreen = fullscreen;
    }
}

std::wstring ControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length), L'\0');
    if (length > 0) GetWindowTextW(control, value.data(), length + 1);
    return value;
}

void SetControlText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void PutAppearance(const Appearance& appearance, HWND target, HWND glass, HWND color,
                   HWND glassOpacity, HWND tint, HWND animation, HWND fullscreen) {
    SetControlText(target, std::to_wstring(appearance.targetOpacity));
    SendMessageW(glass, BM_SETCHECK, appearance.glass ? BST_CHECKED : BST_UNCHECKED, 0);
    SetControlText(color, ColorText(appearance.color));
    SetControlText(glassOpacity, std::to_wstring(appearance.glassOpacity));
    SetControlText(tint, std::to_wstring(appearance.tintOpacity));
    SetControlText(animation, std::to_wstring(appearance.animationMs));
    SendMessageW(fullscreen, BM_SETCHECK, appearance.excludeFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
}

void EnableAppRuleControls(bool enabled) {
    const HWND controls[] = {
        g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor, g_controls.appFGlassOpacity,
        g_controls.appFTint, g_controls.appFAnimation, g_controls.appUTarget, g_controls.appUGlass,
        g_controls.appUColor, g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation,
        g_controls.appFFullscreen, g_controls.appUFullscreen,
        g_controls.removeAppRule,
        g_controls.appMatchClass, g_controls.appMatchTitle, g_controls.appMatchAumid
    };
    for (HWND control : controls) if (control) EnableWindow(control, enabled);
}

void PutAppRule(const AppRule& rule) {
    SetControlText(g_controls.appMatchClass, rule.className);
    SetControlText(g_controls.appMatchTitle, rule.title);
    SetControlText(g_controls.appMatchAumid, rule.aumid);
    PutAppearance(rule.focused, g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor,
                  g_controls.appFGlassOpacity, g_controls.appFTint, g_controls.appFAnimation, g_controls.appFFullscreen);
    PutAppearance(rule.unfocused, g_controls.appUTarget, g_controls.appUGlass, g_controls.appUColor,
                  g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation, g_controls.appUFullscreen);
}

void PopulateAppRuleList() {
    // A combo box keeps the rule selector compact and avoids accidental
    // activation while the user is editing the appearance fields below.
    SendMessageW(g_controls.appRules, CB_RESETCONTENT, 0, 0);
    for (const auto& rule : g_config.appRules) {
        // A rule may carry no process name at all, so the selector shows every
        // matcher the entry holds instead of leaving it blank.
        std::wstring label = rule.process.empty() ? std::wstring(L"*") : rule.process;
        if (!rule.className.empty()) label += L" +class=" + rule.className;
        if (!rule.title.empty()) label += L" +title=" + rule.title;
        if (!rule.aumid.empty()) label += L" +aumid=" + rule.aumid;
        SendMessageW(g_controls.appRules, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (g_selectedAppRule >= static_cast<int>(g_config.appRules.size())) g_selectedAppRule = -1;
    if (g_selectedAppRule >= 0) {
        SendMessageW(g_controls.appRules, CB_SETCURSEL, g_selectedAppRule, 0);
        PutAppRule(g_config.appRules[static_cast<size_t>(g_selectedAppRule)]);
    } else {
        SendMessageW(g_controls.appRules, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    EnableAppRuleControls(g_selectedAppRule >= 0);
}

bool ReadAppRuleControls(AppRule& rule, bool showErrors) {
    const auto read = [&](Appearance& appearance, HWND target, HWND glass, HWND color,
                          HWND glassOpacity, HWND tint, HWND animation, HWND fullscreen) {
        appearance.targetOpacity = ParseDouble(ControlText(target), appearance.targetOpacity, 0.05, 1.0);
        appearance.glass = SendMessageW(glass, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!ParseColor(ControlText(color), appearance.color)) {
            if (showErrors) {
                MessageBoxW(g_window, L"\x989c\x8272\x5fc5\x987b\x4f7f\x7528 #RRGGBB \x683c\x5f0f\x3002", L"\x989c\x8272\x65e0\x6548", MB_ICONWARNING);
                SetFocus(color);
            }
            return false;
        }
        appearance.glassOpacity = ParseDouble(ControlText(glassOpacity), appearance.glassOpacity, 0.05, 1.0);
        appearance.tintOpacity = ParseDouble(ControlText(tint), appearance.tintOpacity, 0.0, 1.0);
        appearance.animationMs = ParseInt(ControlText(animation), appearance.animationMs, 0, 5000);
        appearance.excludeFullscreen = SendMessageW(fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
        return true;
    };
    // The editor keeps the user's own spelling, so these values are trimmed but
    // never lower-cased; the resident process normalises them for matching.
    rule.className = Trim(ControlText(g_controls.appMatchClass));
    rule.title = Trim(ControlText(g_controls.appMatchTitle));
    rule.aumid = Trim(ControlText(g_controls.appMatchAumid));
    return read(rule.focused, g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor,
                g_controls.appFGlassOpacity, g_controls.appFTint, g_controls.appFAnimation, g_controls.appFFullscreen) &&
           read(rule.unfocused, g_controls.appUTarget, g_controls.appUGlass, g_controls.appUColor,
                g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation, g_controls.appUFullscreen);
}

void CaptureSelectedAppRule(bool showErrors) {
    if (g_selectedAppRule < 0 || g_selectedAppRule >= static_cast<int>(g_config.appRules.size())) return;
    ReadAppRuleControls(g_config.appRules[static_cast<size_t>(g_selectedAppRule)], showErrors);
}

void EnumerateProcessCandidates() {
    g_processCandidates.clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                AddProcessCandidate(entry.szExeFile, entry.th32ProcessID,
                                    ProcessPathFromId(entry.th32ProcessID));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    // Some managed/elevated desktop environments deny the system-wide
    // Toolhelp snapshot. Visible top-level windows remain enough for the
    // intended Cheat-Engine-style workflow, so use them as a safe fallback.
    if (g_processCandidates.empty()) EnumWindows(&VisibleWindowProcessProc, 0);
    std::sort(g_processCandidates.begin(), g_processCandidates.end(), [](const ProcessCandidate& left, const ProcessCandidate& right) {
        return left.display < right.display;
    });
}

int SystemIconIndex(const ProcessCandidate& candidate) {
    SHFILEINFOW info{};
    const bool hasPath = !candidate.path.empty();
    const wchar_t* source = hasPath ? candidate.path.c_str() : L".exe";
    const UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON |
                       (hasPath ? 0 : SHGFI_USEFILEATTRIBUTES);
    const auto imageList = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(source, hasPath ? 0 : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags));
    if (imageList) {
        if (!g_pickerImages) {
            // The Shell owns this cache. We only attach it to ListView and
            // must never destroy it when the picker closes.
            g_pickerImages = imageList;
            ListView_SetImageList(g_pickerList, g_pickerImages, LVSIL_SMALL);
        }
        return info.iIcon;
    }
    return 0;
}

BOOL CALLBACK VisibleApplicationPickerProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return TRUE;

    // The application page deliberately collapses several top-level windows
    // from one program into one selectable permanent-rule candidate.
    for (const auto& candidate : g_pickerCandidates) {
        if (candidate.processId == processId) return TRUE;
    }
    const auto found = std::find_if(g_processCandidates.begin(), g_processCandidates.end(),
        [processId](const ProcessCandidate& candidate) { return candidate.processId == processId; });
    if (found != g_processCandidates.end()) g_pickerCandidates.push_back(*found);
    return TRUE;
}

BOOL CALLBACK VisibleWindowPickerProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return TRUE;

    const auto found = std::find_if(g_processCandidates.begin(), g_processCandidates.end(),
        [processId](const ProcessCandidate& candidate) { return candidate.processId == processId; });
    if (found == g_processCandidates.end()) return TRUE;

    ProcessCandidate candidate = *found;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    const std::wstring windowName = title[0] ? std::wstring(title) : candidate.process;
    // A window handle distinguishes two documents/dialogs belonging to the
    // same process, while selection still writes the stable executable name.
    candidate.display = HexIdentifier(reinterpret_cast<UINT_PTR>(hwnd)) + L" - " + windowName;
    g_pickerCandidates.push_back(std::move(candidate));
    return TRUE;
}

// Store and MSIX applications are launched by package identity rather than by
// executable name: to the process snapshot every one of them is the shared
// ApplicationFrameHost.exe. The shell's AppsFolder namespace is the list of
// launchable entries, and every item there carries both the name a person
// recognises and the identity the resident process compares against.
// PKEY_AppUserModel_ID, spelled out here to keep the editor free of a propsys
// dependency that its one property read does not justify.
const PROPERTYKEY kPackageIdentityKey = {
    { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

// Set by the --list-packages diagnostic, which reports how far the shell
// enumeration got when it finds nothing.
bool g_reportPackageSteps = false;

void ReportPackageStep(const wchar_t* step, HRESULT result) {
    if (g_reportPackageSteps) std::wprintf(L"  %ls: hr=0x%08lX\n", step, static_cast<unsigned long>(result));
}

void EnumeratePackageCandidates() {
    PIDLIST_ABSOLUTE folder = nullptr;
    const HRESULT folderResult = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &folder);
    ReportPackageStep(L"SHGetKnownFolderIDList", folderResult);
    if (FAILED(folderResult) || !folder) return;
    // SHBindToParent hands back the parent folder (the desktop) together with
    // the AppsFolder pidl, so the namespace itself still has to be bound to:
    // enumerating the parent would list desktop icons instead.
    IShellFolder* desktop = nullptr;
    PCUITEMID_CHILD child = nullptr;
    const HRESULT bindResult = SHBindToParent(folder, __uuidof(IShellFolder), reinterpret_cast<void**>(&desktop), &child);
    ReportPackageStep(L"SHBindToParent", bindResult);
    if (FAILED(bindResult) || !desktop) {
        CoTaskMemFree(folder);
        return;
    }
    IShellFolder* apps = nullptr;
    const HRESULT folderBindResult = desktop->BindToObject(child, nullptr, __uuidof(IShellFolder),
                                                           reinterpret_cast<void**>(&apps));
    ReportPackageStep(L"BindToObject", folderBindResult);
    desktop->Release();
    if (FAILED(folderBindResult) || !apps) {
        CoTaskMemFree(folder);
        return;
    }
    IEnumIDList* ids = nullptr;
    // Every entry in this namespace is a folder as far as the shell is
    // concerned, so asking for files alone returns nothing at all.
    const HRESULT enumResult = apps->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &ids);
    ReportPackageStep(L"EnumObjects", enumResult);
    size_t seen = 0;
    size_t identified = 0;
    if (SUCCEEDED(enumResult) && ids) {
        LPITEMIDLIST childPidl = nullptr;
        while (ids->Next(1, &childPidl, nullptr) == S_OK) {
            ++seen;
            IShellItem2* item = nullptr;
            if (SUCCEEDED(SHCreateItemWithParent(folder, apps, childPidl, __uuidof(IShellItem2),
                                                 reinterpret_cast<void**>(&item))) && item) {
                PWSTR identity = nullptr;
                if (SUCCEEDED(item->GetString(kPackageIdentityKey, &identity)) && identity) {
                    ++identified;
                    const std::wstring aumid(identity);
                    CoTaskMemFree(identity);
                    // A package identity qualifies its application after the
                    // "!" separator; the plain desktop entries that share this
                    // namespace already appear on the process pages.
                    if (aumid.find(L'!') != std::wstring::npos) {
                        PWSTR name = nullptr;
                        item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
                        ProcessCandidate candidate{};
                        candidate.aumid = aumid;
                        candidate.display = name ? std::wstring(name) + L"   " + aumid : aumid;
                        if (name) CoTaskMemFree(name);
                        g_pickerCandidates.push_back(std::move(candidate));
                    }
                }
                item->Release();
            }
            CoTaskMemFree(childPidl);
        }
        ids->Release();
    }
    if (g_reportPackageSteps) {
        std::wprintf(L"  items=%zu with_identity=%zu packaged=%zu\n", seen, identified, g_pickerCandidates.size());
    }
    apps->Release();
    CoTaskMemFree(folder);
}

void BuildPickerCandidates() {
    g_pickerCandidates.clear();
    switch (g_pickerPage) {
    case PickerPage::Applications:
        EnumWindows(&VisibleApplicationPickerProc, 0);
        break;
    case PickerPage::Processes:
        g_pickerCandidates = g_processCandidates;
        break;
    case PickerPage::Windows:
        EnumWindows(&VisibleWindowPickerProc, 0);
        break;
    case PickerPage::Packages:
        EnumeratePackageCandidates();
        break;
    }
    std::sort(g_pickerCandidates.begin(), g_pickerCandidates.end(), [](const ProcessCandidate& left, const ProcessCandidate& right) {
        return left.display < right.display;
    });
}

void UpdatePickerOpenButton() {
    if (!g_pickerOpen || !g_pickerList) return;
    EnableWindow(g_pickerOpen, ListView_GetNextItem(g_pickerList, -1, LVNI_SELECTED) >= 0);
}

void PopulatePickerList() {
    if (!g_pickerList) return;
    ListView_DeleteAllItems(g_pickerList);
    ListView_SetImageList(g_pickerList, nullptr, LVSIL_SMALL);
    g_pickerImages = nullptr;
    for (size_t i = 0; i < g_pickerCandidates.size(); ++i) {
        const ProcessCandidate& candidate = g_pickerCandidates[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(candidate.display.c_str());
        item.iImage = SystemIconIndex(candidate);
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(g_pickerList, &item);
    }
    UpdatePickerOpenButton();
}

void RebuildPickerList() {
    BuildPickerCandidates();
    PopulatePickerList();
}

bool ConfirmPickerSelection() {
    const int selected = ListView_GetNextItem(g_pickerList, -1, LVNI_SELECTED);
    if (selected < 0) return false;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(g_pickerList, &item) || item.lParam < 0 ||
        item.lParam >= static_cast<LPARAM>(g_pickerCandidates.size())) return false;
    const auto& candidate = g_pickerCandidates[static_cast<size_t>(item.lParam)];
    if (!candidate.aumid.empty()) {
        // A Store application has no process name to offer, so its package
        // identity is the only stable handle to hand back.
        g_pickerChosenProcess.clear();
        g_pickerChosenAumid = candidate.aumid;
        g_pickerChosenDisplay = candidate.display;
    } else {
        g_pickerChosenAumid.clear();
        g_pickerChosenProcess = candidate.process;
        g_pickerChosenDisplay = candidate.process + L"  [PID " + std::to_wstring(candidate.processId) + L"]";
    }
    DestroyWindow(g_pickerWindow);
    return true;
}

void SetSelectedProcess(const std::wstring& process, const std::wstring& display, const std::wstring& aumid = L"") {
    g_selectedProcess = process;
    g_selectedAumid = aumid;
    g_selectedProcessDisplay = display;
    const bool hasSelection = !g_selectedProcess.empty() || !g_selectedAumid.empty();
    if (g_controls.selectedProcess) {
        SetControlText(g_controls.selectedProcess, hasSelection ? g_selectedProcessDisplay : L"\x5c1a\x672a\x9009\x62e9\x8fdb\x7a0b");
    }
    // The blacklist matches on process and window class, and a package identity
    // has neither, so only the per-application rule path can use it.
    if (g_controls.addProcessBlacklist) EnableWindow(g_controls.addProcessBlacklist, !g_selectedProcess.empty());
    if (g_controls.addAppRule) EnableWindow(g_controls.addAppRule, hasSelection);
}

LRESULT CALLBACK ProcessPickerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_pickerWindow = hwnd;
        // Use a real menu instead of a decorative label so the familiar
        // "File" affordance remains useful: it refreshes a stale snapshot.
        HMENU fileMenu = CreatePopupMenu();
        AppendMenuW(fileMenu, MF_STRING, IDC_PICKER_REFRESH, L"\x5237\x65b0\x5217\x8868");
        AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fileMenu, MF_STRING, IDC_PICKER_CANCEL, L"\x5173\x95ed");
        HMENU menuBar = CreateMenu();
        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"\x6587\x4ef6");
        SetMenu(hwnd, menuBar);

        g_pickerTabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            10, 8, 580, 29, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICKER_TABS)), g_instance, nullptr);
        if (g_pickerTabs && g_font) SendMessageW(g_pickerTabs, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        const wchar_t* pages[] = { L"\x5e94\x7528\x7a0b\x5e8f", L"\x8fdb\x7a0b", L"\x7a97\x53e3", L"\x5546\x5e97\x5e94\x7528" };
        for (int index = 0; index < static_cast<int>(std::size(pages)); ++index) {
            TCITEMW page{};
            page.mask = TCIF_TEXT;
            page.pszText = const_cast<wchar_t*>(pages[index]);
            TabCtrl_InsertItem(g_pickerTabs, index, &page);
        }
        g_pickerList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, 42, 580, 455, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICKER_LIST)), g_instance, nullptr);
        if (g_pickerList && g_font) SendMessageW(g_pickerList, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        ListView_SetExtendedListViewStyle(g_pickerList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L""); column.cx = 555; ListView_InsertColumn(g_pickerList, 0, &column);
        g_pickerOpen = CreateWindowExW(0, L"BUTTON", L"\x6253\x5f00", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            330, 512, 95, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICKER_OPEN)), g_instance, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"\x53d6\x6d88", WS_CHILD | WS_VISIBLE,
            440, 512, 95, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICKER_CANCEL)), g_instance, nullptr);
        if (g_pickerOpen && g_font) SendMessageW(g_pickerOpen, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        if (cancel && g_font) SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        RebuildPickerList();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PICKER_REFRESH) {
            EnumerateProcessCandidates();
            RebuildPickerList();
            return 0;
        }
        if (LOWORD(wParam) == IDC_PICKER_OPEN) { ConfirmPickerSelection(); return 0; }
        if (LOWORD(wParam) == IDC_PICKER_CANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_NOTIFY: {
        const auto* notify = reinterpret_cast<const NMHDR*>(lParam);
        if (notify && notify->idFrom == IDC_PICKER_TABS && notify->code == TCN_SELCHANGE) {
            const int selectedPage = TabCtrl_GetCurSel(g_pickerTabs);
            g_pickerPage = selectedPage == 1 ? PickerPage::Processes :
                           selectedPage == 2 ? PickerPage::Windows :
                           selectedPage == 3 ? PickerPage::Packages : PickerPage::Applications;
            RebuildPickerList();
            return 0;
        }
        if (notify && notify->idFrom == IDC_PICKER_LIST && notify->code == LVN_ITEMCHANGED) {
            UpdatePickerOpenButton();
            return 0;
        }
        if (notify && notify->idFrom == IDC_PICKER_LIST && notify->code == NM_DBLCLK) {
            ConfirmPickerSelection();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_pickerImages = nullptr;
        g_pickerList = nullptr;
        g_pickerTabs = nullptr;
        g_pickerOpen = nullptr;
        g_pickerWindow = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureProcessPickerClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = ProcessPickerProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"WinGlassProcessPicker";
    windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

void OpenProcessPicker() {
    if (!EnsureProcessPickerClass()) return;
    EnumerateProcessCandidates();
    g_pickerChosenProcess.clear();
    g_pickerChosenDisplay.clear();
    g_pickerChosenAumid.clear();
    HWND picker = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WinGlassProcessPicker", L"\x9009\x62e9\x8fd0\x884c\x8fdb\x7a0b",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 610, 605,
        g_window, nullptr, g_instance, nullptr);
    if (!picker) return;
    EnableWindow(g_window, FALSE);
    ShowWindow(picker, SW_SHOWNORMAL);
    MSG message{};
    while (IsWindow(picker) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(picker, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(g_window, TRUE);
    SetForegroundWindow(g_window);
    if (!g_pickerChosenProcess.empty() || !g_pickerChosenAumid.empty()) {
        SetSelectedProcess(g_pickerChosenProcess, g_pickerChosenDisplay, g_pickerChosenAumid);
        SetStatus(L"\x5df2\x9009\x62e9 " + g_pickerChosenDisplay + L"\x3002");
    }
}

void LoadToControls() {
    SendMessageW(g_controls.enabled, BM_SETCHECK, g_config.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    PutAppearance(g_config.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                  g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation, g_controls.fFullscreen);
    PutAppearance(g_config.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                  g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation, g_controls.uFullscreen);
    SetControlText(g_controls.blacklistProcesses, JoinLines(g_config.blacklistProcesses));
    SetControlText(g_controls.blacklistClasses, JoinLines(g_config.blacklistClasses));
    g_selectedAppRule = g_config.appRules.empty() ? -1 : 0;
    PopulateAppRuleList();
}

std::vector<std::wstring> ReadLines(HWND control) {
    std::vector<std::wstring> result;
    std::wistringstream input(ControlText(control));
    std::wstring line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (!line.empty()) AddUnique(result, line);
    }
    return result;
}

bool ReadAppearance(Appearance& appearance, HWND target, HWND glass, HWND color,
                    HWND glassOpacity, HWND tint, HWND animation, HWND fullscreen) {
    appearance.targetOpacity = ParseDouble(ControlText(target), appearance.targetOpacity, 0.05, 1.0);
    appearance.glass = SendMessageW(glass, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!ParseColor(ControlText(color), appearance.color)) {
        const int page = PageOfControl(color);
        if (page >= 0) ActivatePage(page);
        MessageBoxW(g_window, L"\x989c\x8272\x5fc5\x987b\x4f7f\x7528 #RRGGBB \x683c\x5f0f\x3002", L"\x989c\x8272\x65e0\x6548", MB_ICONWARNING);
        SetFocus(color);
        return false;
    }
    appearance.glassOpacity = ParseDouble(ControlText(glassOpacity), appearance.glassOpacity, 0.05, 1.0);
    appearance.tintOpacity = ParseDouble(ControlText(tint), appearance.tintOpacity, 0.0, 1.0);
    appearance.animationMs = ParseInt(ControlText(animation), appearance.animationMs, 0, 5000);
    appearance.excludeFullscreen = SendMessageW(fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return true;
}

void SetStatus(const std::wstring& text) {
    SetControlText(g_controls.status, text);
}

// ---------------------------------------------------------------------------
// Experimental clipboard palette
//
// The user takes a screenshot of the bare desktop with any screenshot tool and
// copies it to the clipboard. This dialog decodes that image, extracts the most
// frequent colours and can write one of them into the tint fields. It never
// talks to the resident process and never writes config.yaml by itself: the
// regular "save and apply" button stays in charge of persisting anything.
// ---------------------------------------------------------------------------

constexpr int kPaletteSwatchCount = 10;

// The editor buttons and the dialog buttons live in different windows, so the
// numeric ranges only have to stay readable rather than globally unique.
enum PaletteControlId {
    IDC_PALETTE_OPEN = 200,
    IDC_PALETTE_INFO = 400,
    IDC_PALETTE_READ = 401,
    IDC_PALETTE_APPLY_GLOBAL = 402,
    IDC_PALETTE_APPLY_ALL = 403,
    IDC_PALETTE_CLOSE = 404,
    IDC_PALETTE_STATUS = 405,
    IDC_PALETTE_SWATCH_BASE = 420
};

constexpr int kPaletteClientWidth = 660;
constexpr int kPaletteClientHeight = 460;
constexpr int kPalettePreviewLeft = 24;
constexpr int kPalettePreviewTop = 46;
constexpr int kPalettePreviewWidth = 288;
constexpr int kPalettePreviewHeight = 162;
constexpr DWORD kDibAlphaBitfields = 6;  // BI_ALPHABITFIELDS

struct ClipboardBitmap {
    std::vector<uint8_t> pixels;  // BGRA8, top-down, row-major
    int width = 0;
    int height = 0;
};

struct PaletteEntry {
    Color color{};
    double ratio = 0.0;
};

HWND g_paletteWindow = nullptr;
HWND g_paletteInfo = nullptr;
HWND g_paletteStatus = nullptr;
HWND g_paletteSwatches[kPaletteSwatchCount]{};
ClipboardBitmap g_paletteBitmap;
std::vector<PaletteEntry> g_paletteEntries;
int g_paletteSelected = -1;
uint64_t g_paletteSampleCount = 0;
std::wstring g_paletteSourceDescription;

// --- clipboard access ------------------------------------------------------

bool OpenClipboardWithRetry(HWND owner) {
    // A screenshot tool may still hold the clipboard for a few milliseconds
    // after copying, so retry briefly instead of reporting a hard failure.
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(30);
    }
    return false;
}

bool CopyClipboardFormat(HWND owner, UINT format, std::vector<uint8_t>& bytes) {
    if (!format || !IsClipboardFormatAvailable(format)) return false;
    if (!OpenClipboardWithRetry(owner)) return false;
    bool copied = false;
    if (HANDLE handle = GetClipboardData(format)) {
        if (const void* data = GlobalLock(handle)) {
            const SIZE_T size = GlobalSize(handle);
            if (size > 0) {
                const auto* begin = static_cast<const uint8_t*>(data);
                bytes.assign(begin, begin + size);
                copied = true;
            }
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return copied;
}

uint32_t ReadUint32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint8_t ExtractMaskedChannel(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;
    int shift = 0;
    while (shift < 32 && ((mask >> shift) & 1u) == 0) ++shift;
    const uint32_t ceiling = mask >> shift;
    if (ceiling == 0) return 0;
    const uint64_t value = (pixel & mask) >> shift;
    return static_cast<uint8_t>((value * 255u + ceiling / 2u) / ceiling);
}

// Decodes a clipboard device independent bitmap (CF_DIB / CF_DIBV5) into a
// BGRA8 buffer. Screenshot tools overwhelmingly use 32 bpp BI_RGB, but 24 bpp,
// 16 bpp and 8 bpp palettes are handled as well so older tools keep working.
bool DecodeDib(const uint8_t* data, size_t size, ClipboardBitmap& out, std::wstring& error) {
    const wchar_t* kInvalid = L"\x526a\x8d34\x677f\x56fe\x7247\x6570\x636e\x4e0d\x5b8c\x6574\x6216\x5df2\x635f\x574f\x3002";
    const wchar_t* kUnsupported = L"\x526a\x8d34\x677f\x56fe\x7247\x7684\x4f4d\x6df1\x4e0d\x53d7\x652f\x6301\x3002";
    if (size < sizeof(BITMAPINFOHEADER)) { error = kInvalid; return false; }
    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(data);
    if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > size) { error = kInvalid; return false; }
    if (header->biWidth <= 0 || header->biHeight == 0 || header->biPlanes != 1) { error = kInvalid; return false; }

    const int width = static_cast<int>(header->biWidth);
    const int height = std::abs(static_cast<int>(header->biHeight));
    const bool topDown = header->biHeight < 0;
    const WORD bitCount = header->biBitCount;
    const DWORD compression = header->biCompression;

    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;
    size_t pixelOffset = header->biSize;

    if (header->biSize >= sizeof(BITMAPV4HEADER)) {
        // BITMAPV4HEADER and BITMAPV5HEADER carry the channel masks inline.
        const auto* v4 = reinterpret_cast<const BITMAPV4HEADER*>(data);
        redMask = v4->bV4RedMask;
        greenMask = v4->bV4GreenMask;
        blueMask = v4->bV4BlueMask;
        alphaMask = v4->bV4AlphaMask;
    } else if (compression == BI_BITFIELDS || compression == kDibAlphaBitfields) {
        const size_t maskBytes = compression == kDibAlphaBitfields ? 16 : 12;
        if (header->biSize + maskBytes > size) { error = kInvalid; return false; }
        redMask = ReadUint32(data + header->biSize);
        greenMask = ReadUint32(data + header->biSize + 4);
        blueMask = ReadUint32(data + header->biSize + 8);
        if (maskBytes == 16) alphaMask = ReadUint32(data + header->biSize + 12);
        pixelOffset = header->biSize + maskBytes;
    }

    if (redMask == 0 && greenMask == 0 && blueMask == 0) {
        if (bitCount == 32 || bitCount == 24) {
            redMask = 0x00ff0000u; greenMask = 0x0000ff00u; blueMask = 0x000000ffu;
        } else if (bitCount == 16) {
            redMask = 0x7c00u; greenMask = 0x03e0u; blueMask = 0x001fu;
        }
    }

    size_t paletteEntries = 0;
    const uint8_t* palette = nullptr;
    if (bitCount <= 8) {
        paletteEntries = header->biClrUsed ? header->biClrUsed : (size_t{1} << bitCount);
        if (pixelOffset + paletteEntries * 4 > size) { error = kInvalid; return false; }
        palette = data + pixelOffset;
        pixelOffset += paletteEntries * 4;
    }

    if (bitCount != 32 && bitCount != 24 && bitCount != 16 && bitCount != 8) { error = kUnsupported; return false; }

    const size_t stride = ((static_cast<size_t>(width) * bitCount + 31) / 32) * 4;
    const size_t pixelsSize = stride * static_cast<size_t>(height);

    // BITMAPV4/V5 headers keep the channel masks inline, but the .NET bitmap
    // encoder (and a few other producers) writes the same masks a second time
    // directly after the header while still declaring BI_BITFIELDS. When those
    // bytes match the header masks exactly, skip them; otherwise they would be
    // decoded as the first three pixels and shift the whole image.
    if (header->biSize >= sizeof(BITMAPV4HEADER) &&
        (compression == BI_BITFIELDS || compression == kDibAlphaBitfields)) {
        const size_t extra = compression == kDibAlphaBitfields ? 16 : 12;
        if (pixelOffset + extra + pixelsSize <= size &&
            ReadUint32(data + pixelOffset) == redMask &&
            ReadUint32(data + pixelOffset + 4) == greenMask &&
            ReadUint32(data + pixelOffset + 8) == blueMask) {
            pixelOffset += extra;
        }
    }

    if (pixelOffset + pixelsSize > size) { error = kInvalid; return false; }

    // Screenshot tools routinely leave an unused alpha channel at zero. Only
    // trust alpha when the header declares a real alpha mask, otherwise the
    // whole image would be treated as fully transparent.
    const bool honouredAlpha = alphaMask != 0;
    out.width = width;
    out.height = height;
    out.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    for (int y = 0; y < height; ++y) {
        const int sourceRow = topDown ? y : height - 1 - y;
        const uint8_t* row = data + pixelOffset + stride * static_cast<size_t>(sourceRow);
        uint8_t* destination = out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            uint8_t blue = 0, green = 0, red = 0, alpha = 255;
            if (bitCount == 24) {
                blue = row[x * 3];
                green = row[x * 3 + 1];
                red = row[x * 3 + 2];
            } else if (bitCount == 8) {
                const uint8_t index = row[x];
                if (index < paletteEntries) {
                    blue = palette[index * 4];
                    green = palette[index * 4 + 1];
                    red = palette[index * 4 + 2];
                }
            } else if (bitCount == 16) {
                uint16_t value = 0;
                std::memcpy(&value, row + static_cast<size_t>(x) * 2, sizeof(value));
                red = ExtractMaskedChannel(value, redMask);
                green = ExtractMaskedChannel(value, greenMask);
                blue = ExtractMaskedChannel(value, blueMask);
            } else {
                const uint32_t value = ReadUint32(row + static_cast<size_t>(x) * 4);
                red = ExtractMaskedChannel(value, redMask);
                green = ExtractMaskedChannel(value, greenMask);
                blue = ExtractMaskedChannel(value, blueMask);
                if (honouredAlpha) alpha = ExtractMaskedChannel(value, alphaMask);
            }
            uint8_t* pixel = destination + static_cast<size_t>(x) * 4;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = alpha;
        }
    }
    return true;
}

// Screenshot tools that only publish the PNG clipboard format (registered as
// "PNG") need the Windows Imaging Component to be decoded.
bool DecodePng(const uint8_t* data, size_t size, ClipboardBitmap& out, std::wstring& error) {
    IWICImagingFactory* factory = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory));
    if (FAILED(result) || !factory) {
        error = L"\x65e0\x6cd5\x521d\x59cb\x5316\x56fe\x7247\x89e3\x7801\x5668\xff0cPNG \x683c\x5f0f\x4e0d\x53ef\x7528\x3002";
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool decoded = false;
    do {
        if (FAILED(factory->CreateStream(&stream)) || !stream) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size)))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) break;
        if (FAILED(decoder->GetFrame(0, &frame)) || !frame) break;
        if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                        nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) break;
        out.width = static_cast<int>(width);
        out.height = static_cast<int>(height);
        out.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.pixels.size()), out.pixels.data()))) {
            out.pixels.clear();
            break;
        }
        decoded = true;
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    if (!decoded && error.empty()) error = L"\x526a\x8d34\x677f\x56fe\x7247\x89e3\x7801\x5931\x8d25\x3002";
    return decoded;
}

bool ReadClipboardImage(HWND owner, ClipboardBitmap& out, std::wstring& error) {
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    const bool dibAvailable = IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB);
    const bool pngAvailable = pngFormat && IsClipboardFormatAvailable(pngFormat);
    std::vector<uint8_t> bytes;
    std::wstring decodeError;
    // Kept for diagnostics: knowing which clipboard flavour was decoded makes
    // odd palettes much easier to explain.
    const auto describe = [](const wchar_t* format, const std::vector<uint8_t>& buffer) {
        if (buffer.size() < sizeof(BITMAPINFOHEADER)) return std::wstring(format);
        const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(buffer.data());
        wchar_t text[160]{};
        swprintf_s(text, L"%s header=%u bits=%u compression=%u clrUsed=%u", format,
                   header->biSize, header->biBitCount, header->biCompression, header->biClrUsed);
        return std::wstring(text);
    };
    if (dibAvailable && CopyClipboardFormat(owner, CF_DIBV5, bytes)) {
        g_paletteSourceDescription = describe(L"CF_DIBV5", bytes);
        if (DecodeDib(bytes.data(), bytes.size(), out, decodeError)) return true;
    }
    if (dibAvailable && CopyClipboardFormat(owner, CF_DIB, bytes)) {
        g_paletteSourceDescription = describe(L"CF_DIB", bytes);
        if (DecodeDib(bytes.data(), bytes.size(), out, decodeError)) return true;
    }
    if (pngAvailable && CopyClipboardFormat(owner, pngFormat, bytes)) {
        g_paletteSourceDescription = L"PNG";
        if (DecodePng(bytes.data(), bytes.size(), out, decodeError)) return true;
    }
    if (!dibAvailable && !pngAvailable) {
        error = L"\x526a\x8d34\x677f\x91cc\x6ca1\x6709\x56fe\x7247\x3002\x8bf7\x5148\x622a\x53d6\x65e0\x56fe\x6807\x684c\x9762\x5e76\x590d\x5236\x5230\x526a\x8d34\x677f\x3002";
        return false;
    }
    error = decodeError.empty() ? L"\x526a\x8d34\x677f\x56fe\x7247\x89e3\x7801\x5931\x8d25\x3002" : decodeError;
    return false;
}

// --- colour analysis -------------------------------------------------------

// Reduces the bitmap to the most frequent colours. A coarse 5 bit histogram is
// built first, then neighbouring buckets are merged into clusters so gradients
// produce distinct colours instead of ten near-identical ones.
std::vector<PaletteEntry> ExtractPalette(const ClipboardBitmap& bitmap, int wanted, uint64_t& sampled) {
    std::vector<PaletteEntry> entries;
    sampled = 0;
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty()) return entries;

    const int stepX = std::max(1, bitmap.width / 240);
    const int stepY = std::max(1, bitmap.height / 240);
    struct Bucket {
        uint64_t count = 0;
        uint64_t red = 0;
        uint64_t green = 0;
        uint64_t blue = 0;
    };
    std::vector<Bucket> buckets(32768);
    for (int y = 0; y < bitmap.height; y += stepY) {
        const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(bitmap.width) * 4;
        for (int x = 0; x < bitmap.width; x += stepX) {
            const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
            if (pixel[3] < 16) continue;  // fully transparent pixels are not wallpaper
            Bucket& bucket = buckets[((pixel[2] >> 3) << 10) | ((pixel[1] >> 3) << 5) | (pixel[0] >> 3)];
            ++bucket.count;
            bucket.red += pixel[2];
            bucket.green += pixel[1];
            bucket.blue += pixel[0];
            ++sampled;
        }
    }
    if (sampled == 0) return entries;

    std::vector<int> order;
    order.reserve(buckets.size());
    for (size_t index = 0; index < buckets.size(); ++index) {
        if (buckets[index].count) order.push_back(static_cast<int>(index));
    }
    std::sort(order.begin(), order.end(), [&buckets](int left, int right) {
        return buckets[static_cast<size_t>(left)].count > buckets[static_cast<size_t>(right)].count;
    });

    struct Cluster {
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
        uint64_t count = 0;
    };
    const double mergeDistance = 48.0;
    const auto distance = [](const Cluster& cluster, double red, double green, double blue) {
        const double dr = cluster.red - red;
        const double dg = cluster.green - green;
        const double db = cluster.blue - blue;
        return std::sqrt(dr * dr + dg * dg + db * db);
    };

    std::vector<Cluster> clusters;
    const size_t considered = std::min<size_t>(order.size(), 256);
    const size_t clusterLimit = static_cast<size_t>(wanted) * 6;
    for (size_t index = 0; index < considered; ++index) {
        const Bucket& bucket = buckets[static_cast<size_t>(order[index])];
        const double red = static_cast<double>(bucket.red) / static_cast<double>(bucket.count);
        const double green = static_cast<double>(bucket.green) / static_cast<double>(bucket.count);
        const double blue = static_cast<double>(bucket.blue) / static_cast<double>(bucket.count);
        size_t best = clusters.size();
        double bestDistance = mergeDistance;
        for (size_t i = 0; i < clusters.size(); ++i) {
            const double candidate = distance(clusters[i], red, green, blue);
            if (candidate <= bestDistance) {
                bestDistance = candidate;
                best = i;
            }
        }
        if (best == clusters.size()) {
            if (clusters.size() >= clusterLimit) continue;
            clusters.push_back(Cluster{red, green, blue, bucket.count});
            continue;
        }
        Cluster& cluster = clusters[best];
        const uint64_t combined = cluster.count + bucket.count;
        cluster.red = (cluster.red * static_cast<double>(cluster.count) + red * static_cast<double>(bucket.count)) / static_cast<double>(combined);
        cluster.green = (cluster.green * static_cast<double>(cluster.count) + green * static_cast<double>(bucket.count)) / static_cast<double>(combined);
        cluster.blue = (cluster.blue * static_cast<double>(cluster.count) + blue * static_cast<double>(bucket.count)) / static_cast<double>(combined);
        cluster.count = combined;
    }

    // Second pass: collapse clusters that ended up close to each other.
    const double finalMerge = mergeDistance * 0.8;
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < clusters.size() && !merged; ++i) {
            for (size_t j = i + 1; j < clusters.size(); ++j) {
                if (distance(clusters[i], clusters[j].red, clusters[j].green, clusters[j].blue) > finalMerge) continue;
                const Cluster source = clusters[j];
                Cluster& target = clusters[i];
                const uint64_t combined = target.count + source.count;
                target.red = (target.red * static_cast<double>(target.count) + source.red * static_cast<double>(source.count)) / static_cast<double>(combined);
                target.green = (target.green * static_cast<double>(target.count) + source.green * static_cast<double>(source.count)) / static_cast<double>(combined);
                target.blue = (target.blue * static_cast<double>(target.count) + source.blue * static_cast<double>(source.count)) / static_cast<double>(combined);
                target.count = combined;
                clusters.erase(clusters.begin() + static_cast<ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }

    std::sort(clusters.begin(), clusters.end(), [](const Cluster& left, const Cluster& right) {
        return left.count > right.count;
    });
    if (clusters.size() > static_cast<size_t>(wanted)) clusters.resize(static_cast<size_t>(wanted));
    entries.reserve(clusters.size());
    for (const Cluster& cluster : clusters) {
        PaletteEntry entry;
        entry.color.r = static_cast<int>(std::lround(cluster.red));
        entry.color.g = static_cast<int>(std::lround(cluster.green));
        entry.color.b = static_cast<int>(std::lround(cluster.blue));
        entry.ratio = static_cast<double>(cluster.count) / static_cast<double>(sampled);
        entries.push_back(entry);
    }
    return entries;
}

// --- result dialog ---------------------------------------------------------

HWND MakePaletteControl(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style,
                        int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    if (control && g_font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

std::wstring PaletteSwatchLabel(const PaletteEntry& entry) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%s  %.1f%%", ColorText(entry.color).c_str(), entry.ratio * 100.0);
    return buffer;
}

COLORREF PaletteTextColour(const Color& color) {
    // Rec. 601 luma keeps the label readable on both bright and dark swatches.
    const int luma = (color.r * 299 + color.g * 587 + color.b * 114) / 1000;
    return luma >= 140 ? RGB(16, 16, 16) : RGB(245, 245, 245);
}

void PaintPaletteSwatch(const DRAWITEMSTRUCT& item, const PaletteEntry& entry, bool selected) {
    const RECT bounds = item.rcItem;
    HBRUSH fill = CreateSolidBrush(RGB(entry.color.r, entry.color.g, entry.color.b));
    FillRect(item.hDC, &bounds, fill);
    DeleteObject(fill);

    HPEN border = CreatePen(PS_SOLID, selected ? 3 : 1, selected ? RGB(255, 152, 0) : RGB(96, 96, 96));
    HGDIOBJ previousPen = SelectObject(item.hDC, border);
    HGDIOBJ previousBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SelectObject(item.hDC, previousPen);
    SelectObject(item.hDC, previousBrush);
    DeleteObject(border);

    RECT text = bounds;
    text.left += 6;
    text.right -= 6;
    const std::wstring label = PaletteSwatchLabel(entry);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, PaletteTextColour(entry.color));
    DrawTextW(item.hDC, label.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &bounds);
}

void PaintPalettePreview(HDC dc) {
    const RECT frame{kPalettePreviewLeft, kPalettePreviewTop,
                     kPalettePreviewLeft + kPalettePreviewWidth,
                     kPalettePreviewTop + kPalettePreviewHeight};
    HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
    FillRect(dc, &frame, background);
    DeleteObject(background);
    if (!g_paletteBitmap.pixels.empty() && g_paletteBitmap.width > 0 && g_paletteBitmap.height > 0) {
        const double horizontal = static_cast<double>(kPalettePreviewWidth) / static_cast<double>(g_paletteBitmap.width);
        const double vertical = static_cast<double>(kPalettePreviewHeight) / static_cast<double>(g_paletteBitmap.height);
        const double scale = std::min(horizontal, vertical);
        const int drawWidth = std::max(1, static_cast<int>(static_cast<double>(g_paletteBitmap.width) * scale));
        const int drawHeight = std::max(1, static_cast<int>(static_cast<double>(g_paletteBitmap.height) * scale));
        const int drawLeft = frame.left + (kPalettePreviewWidth - drawWidth) / 2;
        const int drawTop = frame.top + (kPalettePreviewHeight - drawHeight) / 2;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = g_paletteBitmap.width;
        info.bmiHeader.biHeight = -g_paletteBitmap.height;  // top-down, like our buffer
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, drawLeft, drawTop, drawWidth, drawHeight, 0, 0,
                      g_paletteBitmap.width, g_paletteBitmap.height,
                      g_paletteBitmap.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    FrameRect(dc, &frame, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

void EnablePaletteActions(HWND hwnd) {
    const bool ready = g_paletteSelected >= 0 && g_paletteSelected < static_cast<int>(g_paletteEntries.size());
    if (HWND apply = GetDlgItem(hwnd, IDC_PALETTE_APPLY_GLOBAL)) EnableWindow(apply, ready);
    if (HWND apply = GetDlgItem(hwnd, IDC_PALETTE_APPLY_ALL)) EnableWindow(apply, ready);
}

void UpdatePaletteSwatches(HWND hwnd) {
    for (int i = 0; i < kPaletteSwatchCount; ++i) {
        HWND swatch = g_paletteSwatches[i];
        if (!swatch) continue;
        ShowWindow(swatch, i < static_cast<int>(g_paletteEntries.size()) ? SW_SHOW : SW_HIDE);
        InvalidateRect(swatch, nullptr, TRUE);
    }
    EnablePaletteActions(hwnd);
}

void SelectPaletteEntry(HWND hwnd, int index) {
    if (index < 0 || index >= static_cast<int>(g_paletteEntries.size())) return;
    g_paletteSelected = index;
    for (HWND swatch : g_paletteSwatches) {
        if (swatch) InvalidateRect(swatch, nullptr, TRUE);
    }
    EnablePaletteActions(hwnd);
}

void RefreshPaletteFromClipboard(HWND hwnd) {
    ClipboardBitmap bitmap;
    std::wstring error;
    g_paletteBitmap = ClipboardBitmap{};
    g_paletteEntries.clear();
    g_paletteSelected = -1;
    g_paletteSampleCount = 0;

    std::wstring info;
    std::wstring status;
    if (ReadClipboardImage(hwnd, bitmap, error)) {
        g_paletteBitmap = std::move(bitmap);
        g_paletteEntries = ExtractPalette(g_paletteBitmap, kPaletteSwatchCount, g_paletteSampleCount);
        if (g_paletteEntries.empty()) {
            info = L"\x526a\x8d34\x677f\x56fe\x7247\x89e3\x7801\x5931\x8d25\x3002";
            status = info;
        } else {
            const int colours = static_cast<int>(g_paletteEntries.size());
            const int samples = static_cast<int>(std::min<uint64_t>(g_paletteSampleCount, 2000000000ull));
            wchar_t buffer[256]{};
            swprintf_s(buffer,
                       L"\x56fe\x7247 %d x %d\xff0c\x91c7\x6837 %d \x50cf\x7d20\xff0c\x5171\x63d0\x53d6 %d \x4e2a\x989c\x8272\x3002",
                       g_paletteBitmap.width, g_paletteBitmap.height, samples, colours);
            info = buffer;
            status = L"\x8bf7\x5148\x5728\x4e0a\x65b9\x9009\x62e9\x4e00\x4e2a\x989c\x8272\x3002";
        }
    } else {
        info = error;
        status = error;
    }
    SetControlText(g_paletteInfo, info);
    SetControlText(g_paletteStatus, status);
    UpdatePaletteSwatches(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
}

void ApplyPaletteColour(HWND hwnd, bool toAllRules) {
    if (g_paletteSelected < 0 || g_paletteSelected >= static_cast<int>(g_paletteEntries.size())) {
        SetControlText(g_paletteStatus, L"\x8bf7\x5148\x5728\x4e0a\x65b9\x9009\x62e9\x4e00\x4e2a\x989c\x8272\x3002");
        return;
    }
    const Color color = g_paletteEntries[static_cast<size_t>(g_paletteSelected)].color;
    const std::wstring text = ColorText(color);
    const int ruleCount = static_cast<int>(g_config.appRules.size());

    if (toAllRules && ruleCount > 0) {
        wchar_t question[512]{};
        swprintf_s(question,
                   L"\x5c06\x628a %s \x5e94\x7528\x5230\x5168\x90e8 %d \x6761\x4e13\x5c5e\x89c4\x5219\xff0c\x8986\x76d6\x6bcf\x6761\x89c4\x5219\x7684\x805a\x7126\x8272\x548c\x5931\x7126\x8272\x3002\x662f\x5426\x7ee7\x7eed\xff1f",
                   text.c_str(), ruleCount);
        if (MessageBoxW(hwnd, question, L"\x786e\x8ba4\x8986\x76d6", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    }

    // Unsaved edits in the rule form must survive, otherwise writing the colour
    // below would silently discard them.
    CaptureSelectedAppRule(false);

    g_config.focused.color = color;
    g_config.unfocused.color = color;
    SetControlText(g_controls.fColor, text);
    SetControlText(g_controls.uColor, text);

    wchar_t buffer[512]{};
    if (toAllRules) {
        for (auto& rule : g_config.appRules) {
            rule.focused.color = color;
            rule.unfocused.color = color;
        }
        if (g_selectedAppRule >= 0 && g_selectedAppRule < ruleCount) {
            PutAppRule(g_config.appRules[static_cast<size_t>(g_selectedAppRule)]);
        }
        swprintf_s(buffer,
                   L"\x5df2\x628a %s \x5e94\x7528\x5230\x5168\x5c40\x548c %d \x6761\x4e13\x5c5e\x89c4\x5219\x3002\x70b9\x201c\x4fdd\x5b58\x5e76\x5e94\x7528\x201d\x5199\x5165 config.yaml\x3002",
                   text.c_str(), ruleCount);
    } else {
        swprintf_s(buffer,
                   L"\x5df2\x628a %s \x5e94\x7528\x5230\x5168\x5c40\x7684\x805a\x7126\x8272\x548c\x5931\x7126\x8272\x3002\x70b9\x201c\x4fdd\x5b58\x5e76\x5e94\x7528\x201d\x5199\x5165 config.yaml\x3002",
                   text.c_str());
    }
    const std::wstring message = buffer;
    SetControlText(g_paletteStatus, message);
    SetStatus(message);
}

void CreatePaletteControls(HWND hwnd) {
    MakePaletteControl(hwnd, L"STATIC",
                       L"\x5148\x7528\x622a\x56fe\x5de5\x5177\x622a\x53d6\x65e0\x56fe\x6807\x684c\x9762\x5e76\x590d\x5236\x5230\x526a\x8d34\x677f\xff0c\x518d\x70b9\x201c\x91cd\x65b0\x8bfb\x53d6\x526a\x8d34\x677f\x201d",
                       0, 0, 24, 14, 612, 22);
    g_paletteInfo = MakePaletteControl(hwnd, L"STATIC", L"", SS_LEFT, IDC_PALETTE_INFO, 328, 48, 308, 150);
    MakePaletteControl(hwnd, L"STATIC",
                       L"\x51fa\x73b0\x9891\x7387\x6700\x9ad8\x7684\x989c\x8272\xff08\x70b9\x51fb\x9009\x62e9\xff09",
                       0, 0, 24, 218, 612, 20);
    for (int i = 0; i < kPaletteSwatchCount; ++i) {
        const int column = i % 5;
        const int row = i / 5;
        g_paletteSwatches[i] = MakePaletteControl(hwnd, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW,
                                                  IDC_PALETTE_SWATCH_BASE + i,
                                                  24 + column * 124, 242 + row * 72, 116, 64);
    }
    g_paletteStatus = MakePaletteControl(hwnd, L"STATIC", L"", SS_LEFT, IDC_PALETTE_STATUS, 24, 386, 612, 20);
    MakePaletteControl(hwnd, L"BUTTON", L"\x5e94\x7528\x5230\x5168\x5c40", WS_TABSTOP,
                       IDC_PALETTE_APPLY_GLOBAL, 24, 412, 150, 34);
    MakePaletteControl(hwnd, L"BUTTON", L"\x5e94\x7528\x5230\x5168\x90e8\x89c4\x5219\xff08\x542b\x4e13\x5c5e\xff09", WS_TABSTOP,
                       IDC_PALETTE_APPLY_ALL, 184, 412, 220, 34);
    MakePaletteControl(hwnd, L"BUTTON", L"\x91cd\x65b0\x8bfb\x53d6\x526a\x8d34\x677f", WS_TABSTOP,
                       IDC_PALETTE_READ, 414, 412, 130, 34);
    MakePaletteControl(hwnd, L"BUTTON", L"\x5173\x95ed", WS_TABSTOP,
                       IDC_PALETTE_CLOSE, 554, 412, 82, 34);
}

LRESULT CALLBACK PaletteProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_paletteWindow = hwnd;
        CreatePaletteControls(hwnd);
        RefreshPaletteFromClipboard(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        // Labels must blend with the dialog background instead of the default
        // white static background.
        SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_BUTTON) {
            const int index = static_cast<int>(item->CtlID) - IDC_PALETTE_SWATCH_BASE;
            if (index >= 0 && index < static_cast<int>(g_paletteEntries.size())) {
                PaintPaletteSwatch(*item, g_paletteEntries[static_cast<size_t>(index)], index == g_paletteSelected);
                return TRUE;
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintPalettePreview(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id >= IDC_PALETTE_SWATCH_BASE && id < IDC_PALETTE_SWATCH_BASE + kPaletteSwatchCount) {
            SelectPaletteEntry(hwnd, id - IDC_PALETTE_SWATCH_BASE);
            return 0;
        }
        switch (id) {
        case IDC_PALETTE_READ: RefreshPaletteFromClipboard(hwnd); return 0;
        case IDC_PALETTE_APPLY_GLOBAL: ApplyPaletteColour(hwnd, false); return 0;
        case IDC_PALETTE_APPLY_ALL: ApplyPaletteColour(hwnd, true); return 0;
        case IDC_PALETTE_CLOSE: DestroyWindow(hwnd); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_paletteWindow = nullptr;
        g_paletteInfo = nullptr;
        g_paletteStatus = nullptr;
        for (HWND& swatch : g_paletteSwatches) swatch = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsurePaletteClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = PaletteProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"WinGlassPaletteDialog";
    windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

void OpenPaletteDialog() {
    if (g_paletteWindow) {
        SetForegroundWindow(g_paletteWindow);
        return;
    }
    if (!EnsurePaletteClass()) return;
    RECT rect{0, 0, kPaletteClientWidth, kPaletteClientHeight};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WinGlassPaletteDialog",
                                  L"\x4ece\x526a\x8d34\x677f\x63d0\x53d6\x914d\x8272", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top,
                                  g_window, nullptr, g_instance, nullptr);
    if (!dialog) return;
    EnableWindow(g_window, FALSE);
    ShowWindow(dialog, SW_SHOWNORMAL);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(g_window, TRUE);
    SetForegroundWindow(g_window);
}

bool SaveFromControls() {
    EditorConfig updated = g_config;
    updated.enabled = SendMessageW(g_controls.enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!ReadAppearance(updated.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                        g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation, g_controls.fFullscreen) ||
        !ReadAppearance(updated.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                        g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation, g_controls.uFullscreen)) {
        return false;
    }
    updated.blacklistProcesses = ReadLines(g_controls.blacklistProcesses);
    updated.blacklistClasses = ReadLines(g_controls.blacklistClasses);
    if (g_selectedAppRule >= 0 && g_selectedAppRule < static_cast<int>(updated.appRules.size())) {
        AppRule& rule = updated.appRules[static_cast<size_t>(g_selectedAppRule)];
        if (!ReadAppRuleControls(rule, true)) return false;
        // The resident process drops a rule that matches nothing, so writing one
        // would look saved and quietly do nothing.
        if (!rule.AnyMatcher()) {
            ActivatePage(static_cast<int>(EditorPage::Applications));
            MessageBoxW(g_window, L"\x6bcf\x6761\x4e13\x5c5e\x89c4\x5219\x81f3\x5c11\x9700\x8981\x4e00\x4e2a\x5339\x914d\x6761\x4ef6\x3002", L"\x5339\x914d\x6761\x4ef6\x7f3a\x5931", MB_ICONWARNING);
            return false;
        }
    }
    if (!SaveConfigFile(updated, g_configPath)) {
        MessageBoxW(g_window, L"Could not write config.yaml.", L"Save failed", MB_ICONERROR);
        return false;
    }
    g_config = std::move(updated);
    SetStatus(L"\x5df2\x4fdd\x5b58\x3002\x8fd0\x884c\x4e2d\x7684 WinGlass \x4f1a\x81ea\x52a8\x91cd\x65b0\x52a0\x8f7d\x3002");
    return true;
}

void AddSelectedProcessToBlacklist() {
    const std::wstring process = g_selectedProcess;
    if (process.empty()) {
        // A package identity cannot join the name blacklist: those windows
        // belong to the shared ApplicationFrameHost.exe host, so the only way
        // to exempt one is a rule that matches its package identity.
        SetStatus(g_selectedAumid.empty() ? L"\x8bf7\x5148\x9009\x62e9\x4e00\x4e2a\x8fd0\x884c\x8fdb\x7a0b\x3002"
                                          : L"\x5546\x5e97\x5e94\x7528\x8bf7\x7528\x4e13\x5c5e\x89c4\x5219\x6309\x5305\x6807\x8bc6\x5339\x914d\x3002");
        return;
    }
    // Incorporate any manual edits in the blacklist box before adding the
    // process selected from the live list, then persist immediately.
    g_config.blacklistProcesses = ReadLines(g_controls.blacklistProcesses);
    AddUnique(g_config.blacklistProcesses, process);
    SetControlText(g_controls.blacklistProcesses, JoinLines(g_config.blacklistProcesses));
    if (SaveFromControls()) SetStatus(process + L" \x5df2\x52a0\x5165\x9ed1\x540d\x5355\x5e76\x6c38\x4e45\x4fdd\x5b58\x3002");
}

void AddSelectedProcessRule() {
    const std::wstring process = g_selectedProcess;
    const std::wstring aumid = g_selectedAumid;
    if (process.empty() && aumid.empty()) {
        SetStatus(L"\x8bf7\x5148\x9009\x62e9\x4e00\x4e2a\x8fd0\x884c\x8fdb\x7a0b\x3002");
        return;
    }
    // A Store application is identified by its package identity; a desktop
    // program by its executable name. Whichever was picked names the rule in
    // the status line.
    const std::wstring identity = process.empty() ? aumid : process;
    CaptureSelectedAppRule(false);
    for (size_t i = 0; i < g_config.appRules.size(); ++i) {
        const AppRule& existing = g_config.appRules[i];
        if (_wcsicmp(existing.process.c_str(), process.c_str()) == 0 &&
            _wcsicmp(existing.aumid.c_str(), aumid.c_str()) == 0) {
            g_selectedAppRule = static_cast<int>(i);
            PopulateAppRuleList();
            SetStatus(identity + L" \x5df2\x5b58\x5728\x4e13\x5c5e\x89c4\x5219\x3002");
            return;
        }
    }
    // Copy the values currently shown in the global controls so the quick
    // rule feels like a focused override rather than a blank form.
    AppRule rule{};
    rule.process = process;
    rule.aumid = aumid;
    rule.focused = g_config.focused;
    rule.unfocused = g_config.unfocused;
    ReadAppearance(rule.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                   g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation, g_controls.fFullscreen);
    ReadAppearance(rule.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                   g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation, g_controls.uFullscreen);
    g_config.appRules.push_back(std::move(rule));
    g_selectedAppRule = static_cast<int>(g_config.appRules.size() - 1);
    PopulateAppRuleList();
    if (SaveFromControls()) SetStatus(identity + L" \x5df2\x521b\x5efa\x4e13\x5c5e\x89c4\x5219\x5e76\x6c38\x4e45\x4fdd\x5b58\x3002");
}

void RemoveSelectedAppRule() {
    CaptureSelectedAppRule(false);
    if (g_selectedAppRule < 0 || g_selectedAppRule >= static_cast<int>(g_config.appRules.size())) return;
    const std::wstring process = g_config.appRules[static_cast<size_t>(g_selectedAppRule)].process;
    g_config.appRules.erase(g_config.appRules.begin() + g_selectedAppRule);
    if (g_selectedAppRule >= static_cast<int>(g_config.appRules.size())) g_selectedAppRule = static_cast<int>(g_config.appRules.size()) - 1;
    PopulateAppRuleList();
    if (SaveFromControls()) SetStatus(process + L" \x7684\x4e13\x5c5e\x89c4\x5219\x5df2\x5220\x9664\x5e76\x4fdd\x5b58\x3002");
}

void SelectAppRule(int selected) {
    CaptureSelectedAppRule(false);
    g_selectedAppRule = selected;
    if (g_selectedAppRule >= 0 && g_selectedAppRule < static_cast<int>(g_config.appRules.size())) {
        PutAppRule(g_config.appRules[static_cast<size_t>(g_selectedAppRule)]);
    }
    EnableAppRuleControls(g_selectedAppRule >= 0);
}

void ReloadFromDisk() {
    EditorConfig loaded;
    if (!LoadConfigFile(loaded, g_configPath)) {
        MessageBoxW(g_window, L"\x65e0\x6cd5\x8bfb\x53d6 config.yaml \x6216 config.ini\x3002", L"\x52a0\x8f7d\x5931\x8d25", MB_ICONERROR);
        return;
    }
    g_config = std::move(loaded);
    g_originalText = ReadAll(g_configPath);
    LoadToControls();
    SetStatus(L"\x914d\x7f6e\x5df2\x91cd\x65b0\x52a0\x8f7d\x3002");
}

void OpenConfigFolder() {
    const size_t slash = g_configPath.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? L"." : g_configPath.substr(0, slash);
    ShellExecuteW(g_window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CreateControls() {
    // The tab strip belongs to no page, and neither do the note, the status
    // line and the action buttons: those stay visible whichever page shows.
    g_registerPageControls = false;
    g_controls.tabs = MakeControl(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, IDC_TABS, 20, 12, 767, 30);
    if (g_controls.tabs) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const wchar_t* titles[kEditorPageCount] = {
            L"\x5168\x5c40",              // Global
            L"\x4e13\x5c5e\x89c4\x5219",  // Applications
            L"\x9ed1\x540d\x5355"         // Blacklist
        };
        for (int i = 0; i < kEditorPageCount; ++i) {
            item.pszText = const_cast<wchar_t*>(titles[i]);
            TabCtrl_InsertItem(g_controls.tabs, i, &item);
        }
    }
    g_registerPageControls = true;

    g_activePage = static_cast<int>(EditorPage::Global);
    g_controls.enabled = MakeControl(L"BUTTON", L"\x5168\x5c40\x542f\x7528 WinGlass", BS_AUTOCHECKBOX,
                                     IDC_ENABLED, 24, 60, 300, 24);
    // Experimental wallpaper helper: the palette dialog reads a screenshot
    // from the clipboard, so nothing here depends on where the wallpaper comes
    // from (plain image, live wallpaper, slideshow, ...).
    MakeControl(L"BUTTON", L"\x4ece\x526a\x8d34\x677f\x63d0\x53d6\x914d\x8272\xff08\x5b9e\x9a8c\xff09", 0,
                IDC_PALETTE_OPEN, 24, 96, 220, 30);
    MakeLabel(L"\x622a\x56fe\x540e\x590d\x5236\x5230\x526a\x8d34\x677f\xff0c\x518d\x70b9\x6b64\x5206\x6790", 258, 102, 500);
    MakeAppearanceGroup(L"\x805a\x7126\x7a97\x53e3", IDC_F_TARGET, 20, 145);
    MakeAppearanceGroup(L"\x5931\x6d3b\x7a97\x53e3", IDC_U_TARGET, 405, 145);

    g_activePage = static_cast<int>(EditorPage::Applications);
    MakeControl(L"BUTTON", L"\x8fd0\x884c\x8fdb\x7a0b\x4e0e\x4e13\x5c5e\x89c4\x5219", BS_GROUPBOX, 0, 20, 55, 750, 120);
    MakeLabel(L"\x5df2\x9009\x62e9\x8fdb\x7a0b\xff1a", 35, 85, 90);
    g_controls.selectedProcess = MakeLabel(L"\x5c1a\x672a\x9009\x62e9\x8fdb\x7a0b", 125, 85, 420);
    g_controls.pickProcess = MakeControl(L"BUTTON", L"\x9009\x62e9\x8fd0\x884c\x8fdb\x7a0b...", 0,
                                         IDC_PICK_PROCESS, 565, 80, 165, 28);
    g_controls.addProcessBlacklist = MakeControl(L"BUTTON", L"\x52a0\x5165\x9ed1\x540d\x5355\x5e76\x4fdd\x5b58", 0,
                                                  IDC_ADD_PROCESS_BLACKLIST, 350, 115, 170, 25);
    g_controls.addAppRule = MakeControl(L"BUTTON", L"\x521b\x5efa\x4e13\x5c5e\x89c4\x5219", 0,
                                         IDC_ADD_APP_RULE, 530, 115, 130, 25);
    MakeLabel(L"\x5df2\x6709\x4e13\x5c5e\x89c4\x5219", 35, 148, 100);
    g_controls.appRules = MakeControl(L"COMBOBOX", L"", WS_BORDER | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      IDC_APP_RULES, 125, 145, 360, 220);
    g_controls.removeAppRule = MakeControl(L"BUTTON", L"\x5220\x9664\x5f53\x524d\x89c4\x5219", 0,
                                            IDC_REMOVE_APP_RULE, 500, 145, 120, 25);
    SetSelectedProcess(L"", L"");

    // Match conditions. Any field left empty simply does not take part, so a
    // rule can be as broad as one title fragment or as narrow as a package
    // identity plus a window class.
    MakeControl(L"BUTTON", L"\x5339\x914d\x6761\x4ef6\xff08\x7a7a\x5219\x4e0d\x53c2\x4e0e\x5339\x914d\xff09",
                BS_GROUPBOX, 0, 20, 190, 750, 150);
    MakeLabel(L"\x7a97\x53e3\x7c7b\x540d", 35, 226, 120);
    g_controls.appMatchClass = MakeEdit(IDC_APP_MATCH_CLASS, 165, 222, 240);
    MakeLabel(L"\x6807\x9898\x5305\x542b", 35, 258, 120);
    g_controls.appMatchTitle = MakeEdit(IDC_APP_MATCH_TITLE, 165, 254, 460);
    MakeLabel(L"\x5e94\x7528\x5305\x6807\x8bc6\xff08" L"AUMID\xff09", 35, 290, 120);
    g_controls.appMatchAumid = MakeEdit(IDC_APP_MATCH_AUMID, 165, 286, 560);
    MakeLabel(L"\x5546\x5e97\x5e94\x7528\x7684\x5305\x6807\x8bc6\xff0c\x4f8b\xff1a Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", 35, 316, 700);

    MakeAppAppearanceGroup(L"\x4e13\x5c5e\x89c4\x5219\xff1a\x805a\x7126\x7a97\x53e3", IDC_APP_F_TARGET, 20, 355, true);
    MakeAppAppearanceGroup(L"\x4e13\x5c5e\x89c4\x5219\xff1a\x5931\x6d3b\x7a97\x53e3", IDC_APP_U_TARGET, 405, 355, false);

    g_activePage = static_cast<int>(EditorPage::Blacklist);
    MakeControl(L"BUTTON", L"\x9ed1\x540d\x5355\x8fdb\x7a0b\xff08\x6bcf\x884c\x4e00\x4e2a\xff09", BS_GROUPBOX, 0, 20, 55, 365, 240);
    g_controls.blacklistProcesses = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                                  IDC_BLACK_PROCESSES, 35, 90, 335, 185);
    MakeControl(L"BUTTON", L"\x9ed1\x540d\x5355\x7a97\x53e3\x7c7b\x540d\xff08\x6bcf\x884c\x4e00\x4e2a\xff09", BS_GROUPBOX, 0, 405, 55, 365, 240);
    g_controls.blacklistClasses = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                               IDC_BLACK_CLASSES, 420, 90, 335, 185);
    MakeLabel(L"\x547d\x4e2d\x9ed1\x540d\x5355\x7684\x7a97\x53e3\x88ab\x5b8c\x5168\x8df3\x8fc7\xff0c\x4e0d\x6e32\x67d3\x4e5f\x4e0d\x4fee\x6539\x6837\x5f0f\x3002", 24, 315, 740);
    SendMessageW(g_controls.blacklistProcesses, EM_SETLIMITTEXT, 8192, 0);
    SendMessageW(g_controls.blacklistClasses, EM_SETLIMITTEXT, 8192, 0);

    g_registerPageControls = false;
    MakeLabel(L"\x9ed1\x540d\x5355\x4e0e\x4e13\x5c5e\x89c4\x5219\x4f1a\x5199\x5165 config.yaml\xff0c\x8fd0\x884c\x4e2d\x7684 WinGlass \x4f1a\x81ea\x52a8\x5e94\x7528\x3002", 24, 858, 740);
    g_controls.status = MakeLabel(L"", 24, 882, 740);
    MakeControl(L"BUTTON", L"\x4fdd\x5b58\x5e76\x5e94\x7528", BS_DEFPUSHBUTTON, IDC_SAVE, 410, 913, 150, 30);
    MakeControl(L"BUTTON", L"\x91cd\x65b0\x52a0\x8f7d", 0, IDC_RELOAD, 570, 913, 90, 30);
    MakeControl(L"BUTTON", L"\x6253\x5f00\x6587\x4ef6\x5939", 0, IDC_OPEN_FOLDER, 670, 913, 100, 30);
    MakeControl(L"BUTTON", L"\x5173\x95ed", 0, IDC_EXIT, 300, 913, 90, 30);
    g_registerPageControls = true;

    ActivatePage(static_cast<int>(EditorPage::Global));
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        // CreateWindowExW does not return its HWND until after WM_CREATE.
        // Bind it here so every child control receives a valid parent instead
        // of silently failing with a null WS_CHILD parent.
        g_window = hwnd;
        CreateControls();
        ReloadFromDisk();
        return 0;
    case WM_NOTIFY:
        if (lParam) {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == IDC_TABS && header->code == TCN_SELCHANGE) {
                ActivatePage(TabCtrl_GetCurSel(g_controls.tabs));
                return 0;
            }
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SAVE: SaveFromControls(); return 0;
        case IDC_RELOAD: ReloadFromDisk(); return 0;
        case IDC_PICK_PROCESS: OpenProcessPicker(); return 0;
        case IDC_ADD_PROCESS_BLACKLIST: AddSelectedProcessToBlacklist(); return 0;
        case IDC_ADD_APP_RULE: AddSelectedProcessRule(); return 0;
        case IDC_REMOVE_APP_RULE: RemoveSelectedAppRule(); return 0;
        case IDC_APP_RULES:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int selected = static_cast<int>(SendMessageW(g_controls.appRules, CB_GETCURSEL, 0, 0));
                SelectAppRule(selected == CB_ERR ? -1 : selected);
            }
            return 0;
        case IDC_OPEN_FOLDER: OpenConfigFolder(); return 0;
        case IDC_PALETTE_OPEN: OpenPaletteDialog(); return 0;
        case IDC_EXIT: DestroyWindow(hwnd); return 0;
        default: break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    g_instance = instance;
    // The experimental clipboard palette decodes PNG through the Windows
    // Imaging Component, which needs COM on the UI thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Diagnostic mode: read whatever image is currently on the clipboard, print
    // the extracted palette and exit. It runs the exact code path the dialog
    // uses, which makes "my screenshot produced odd colours" easy to check.
    if (commandLine && wcsstr(commandLine, L"--palette-self-test")) {
        ClipboardBitmap bitmap;
        std::wstring error;
        if (!ReadClipboardImage(nullptr, bitmap, error)) {
            std::wprintf(L"clipboard image unavailable: %ls\n", error.c_str());
            CoUninitialize();
            return 4;
        }
        uint64_t sampled = 0;
        const std::vector<PaletteEntry> palette = ExtractPalette(bitmap, kPaletteSwatchCount, sampled);
        std::wprintf(L"source=%ls\nimage %dx%d sampled=%llu colours=%zu\n", g_paletteSourceDescription.c_str(),
                     bitmap.width, bitmap.height, static_cast<unsigned long long>(sampled), palette.size());
        for (const PaletteEntry& entry : palette) {
            std::wprintf(L"#%02X%02X%02X %.2f%%\n", entry.color.r, entry.color.g, entry.color.b,
                         entry.ratio * 100.0);
        }
        CoUninitialize();
        return palette.empty() ? 5 : 0;
    }
    // Package diagnostic: print every Store application identity the picker
    // can offer, so the enumeration can be checked without opening the dialog.
    if (commandLine && wcsstr(commandLine, L"--list-packages")) {
        g_pickerCandidates.clear();
        g_reportPackageSteps = true;
        EnumeratePackageCandidates();
        std::sort(g_pickerCandidates.begin(), g_pickerCandidates.end(),
                  [](const ProcessCandidate& left, const ProcessCandidate& right) { return left.display < right.display; });
        for (const auto& package : g_pickerCandidates) std::wprintf(L"%ls\n", package.display.c_str());
        std::wprintf(L"packages=%zu\n", g_pickerCandidates.size());
        const bool found = !g_pickerCandidates.empty();
        CoUninitialize();
        return found ? 0 : 6;
    }
    // Configuration round trip: load config.yaml, write the parsed model back
    // out through the same writer the Save button uses, and exit. It makes
    // "did my rule survive a save?" answerable without clicking anything.
    if (commandLine && wcsstr(commandLine, L"--config-round-trip")) {
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::wstring base = module;
        const size_t slash = base.find_last_of(L"\\/");
        base = slash == std::wstring::npos ? std::wstring() : base.substr(0, slash + 1);
        const std::wstring source = base + L"config.yaml";
        EditorConfig loaded;
        if (!LoadConfigFile(loaded, source)) {
            std::wprintf(L"round trip failed: cannot load %ls\n", source.c_str());
            CoUninitialize();
            return 2;
        }
        const std::wstring target = base + L"config.roundtrip.yaml";
        if (!SaveConfigFile(loaded, target)) {
            std::wprintf(L"round trip failed: cannot write %ls\n", target.c_str());
            CoUninitialize();
            return 3;
        }
        std::wprintf(L"round trip ok: %ls -> %ls rules=%zu blacklist=%zu classes=%zu\n",
                     source.c_str(), target.c_str(), loaded.appRules.size(),
                     loaded.blacklistProcesses.size(), loaded.blacklistClasses.size());
        CoUninitialize();
        return 0;
    }
    // ListView itself can be registered by another application component, but
    // its image-list support is only dependable after explicit initialization.
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&commonControls);
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    g_configPath = module;
    const size_t slash = g_configPath.find_last_of(L"\\/");
    g_configPath = (slash == std::wstring::npos ? L"" : g_configPath.substr(0, slash + 1)) + L"config.yaml";

    g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = EditorProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"WinGlassConfigEditor";
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_WINGLASS_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!RegisterClassExW(&windowClass)) return 2;

    g_window = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName,
                               L"WinGlass \x914d\x7f6e\x7f16\x8f91\x5668", WS_OVERLAPPED | WS_CAPTION |
                               WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 810, 1000,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 3;
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
