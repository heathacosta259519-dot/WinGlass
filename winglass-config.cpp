#define _UNICODE
#define UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <fstream>
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
};

struct AppRule {
    // WinGlass matches permanent rules by executable name, not the ephemeral
    // PID displayed by the process picker.
    std::wstring process;
    Appearance focused{};
    Appearance unfocused{};
};

struct ProcessCandidate {
    std::wstring display;
    std::wstring process;
    std::wstring path;
    DWORD processId = 0;
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
    IDC_F_TARGET, IDC_F_GLASS, IDC_F_COLOR, IDC_F_GLASS_OPACITY, IDC_F_TINT, IDC_F_ANIMATION,
    IDC_U_TARGET, IDC_U_GLASS, IDC_U_COLOR, IDC_U_GLASS_OPACITY, IDC_U_TINT, IDC_U_ANIMATION,
    IDC_BLACK_PROCESSES, IDC_BLACK_CLASSES,
    IDC_PICK_PROCESS, IDC_SELECTED_PROCESS, IDC_ADD_PROCESS_BLACKLIST, IDC_ADD_APP_RULE,
    IDC_APP_RULES, IDC_REMOVE_APP_RULE,
    IDC_APP_F_TARGET, IDC_APP_F_GLASS, IDC_APP_F_COLOR, IDC_APP_F_GLASS_OPACITY, IDC_APP_F_TINT, IDC_APP_F_ANIMATION,
    IDC_APP_U_TARGET, IDC_APP_U_GLASS, IDC_APP_U_COLOR, IDC_APP_U_GLASS_OPACITY, IDC_APP_U_TINT, IDC_APP_U_ANIMATION,
    IDC_SAVE, IDC_RELOAD, IDC_OPEN_FOLDER, IDC_EXIT, IDC_STATUS
};

struct Controls {
    HWND enabled = nullptr;
    HWND fTarget = nullptr;
    HWND fGlass = nullptr;
    HWND fColor = nullptr;
    HWND fGlassOpacity = nullptr;
    HWND fTint = nullptr;
    HWND fAnimation = nullptr;
    HWND uTarget = nullptr;
    HWND uGlass = nullptr;
    HWND uColor = nullptr;
    HWND uGlassOpacity = nullptr;
    HWND uTint = nullptr;
    HWND uAnimation = nullptr;
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
    HWND appUTarget = nullptr;
    HWND appUGlass = nullptr;
    HWND appUColor = nullptr;
    HWND appUGlassOpacity = nullptr;
    HWND appUTint = nullptr;
    HWND appUAnimation = nullptr;
    HWND status = nullptr;
} g_controls;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HFONT g_font = nullptr;
std::wstring g_configPath;
std::wstring g_originalText;
EditorConfig g_config;
std::vector<ProcessCandidate> g_processCandidates;
int g_selectedAppRule = -1;
std::wstring g_selectedProcess;
std::wstring g_selectedProcessDisplay;
std::wstring g_pickerChosenProcess;
std::wstring g_pickerChosenDisplay;

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

// The three pages mirror familiar process explorers: applications are the
// visible desktop programs, processes are the complete process snapshot, and
// windows exposes each individual visible top-level window.
enum class PickerPage { Applications, Processes, Windows };
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

std::wstring ReadAll(const std::wstring& path) {
    std::wifstream input(path);
    if (!input) return L"";
    std::wstringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
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
}

bool LoadYaml(EditorConfig& result, const std::wstring& text) {
    result = {};
    enum class Section { None, Global, Focused, Unfocused, Blacklist, Applications };
    Section section = Section::None;
    AppRule currentRule{};
    bool appOpen = false;
    bool appFocused = true;
    const auto commitRule = [&]() {
        if (appOpen && !currentRule.process.empty()) result.appRules.push_back(std::move(currentRule));
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
        if (!item.empty() && item.front() == L'-') item = Trim(item.substr(1));
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
            if (key == L"match") {
                commitRule();
                appOpen = true;
                currentRule.focused = result.focused;
                currentRule.unfocused = result.unfocused;
                continue;
            }
            if (key == L"process" && !value.empty()) {
                if (!appOpen) {
                    appOpen = true;
                    currentRule.focused = result.focused;
                    currentRule.unfocused = result.unfocused;
                }
                currentRule.process = Unquote(value);
                continue;
            }
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
    for (const auto& rule : config.appRules) if (!safeYamlName(rule.process)) return false;

    // Write beside the destination and atomically replace it only after the
    // stream is known-good. A crash or full disk can no longer truncate the
    // user's last valid config.yaml halfway through a save.
    const std::wstring tempPath = path + L".tmp";
    std::wofstream output(tempPath, std::ios::trunc);
    if (!output) return false;
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
            output << L"  - match:\n"
                   << L"      process: \"" << rule.process << L"\"\n"
                   << L"    rules:\n"
                   << L"      - type: focused\n"
                   << L"        config:\n"
                   << AppearanceFieldsYaml(rule.focused, 10)
                   << L"      - type: unfocused\n"
                   << L"        config:\n"
                   << AppearanceFieldsYaml(rule.unfocused, 10);
        }
    }
    output.flush();
    const bool writeSucceeded = output.good();
    output.close();
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

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                 int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    if (control && g_font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

HWND MakeLabel(const wchar_t* text, int x, int y, int width = 150) {
    return MakeControl(L"STATIC", text, 0, 0, x, y, width, 22);
}

HWND MakeEdit(int id, int x, int y, int width = 100) {
    return MakeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id, x, y, width, 24);
}

void MakeAppearanceGroup(const wchar_t* title, int baseId, int x, int y) {
    MakeControl(L"BUTTON", title, BS_GROUPBOX, 0, x, y, 365, 220);
    MakeLabel(L"\x76ee\x6807\x900f\x660e\x5ea6 (0.05 - 1.0)", x + 16, y + 32);
    MakeLabel(L"\x542f\x7528\x6bdb\x73bb\x7483", x + 16, y + 62);
    MakeLabel(L"\x73bb\x7483/\x67d3\x8272\x989c\x8272 (#RRGGBB)", x + 16, y + 92);
    MakeLabel(L"\x73bb\x7483\x900f\x660e\x5ea6 (0 - 1)", x + 16, y + 122);
    MakeLabel(L"\x67d3\x8272\x5f3a\x5ea6 (0 - 1)", x + 16, y + 152);
    MakeLabel(L"\x52a8\x753b\x65f6\x957f (\x6beb\x79d2)", x + 16, y + 182);

    HWND target = MakeEdit(baseId, x + 205, y + 29);
    HWND glass = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 1, x + 205, y + 59, 22, 22);
    HWND color = MakeEdit(baseId + 2, x + 205, y + 89, 125);
    HWND glassOpacity = MakeEdit(baseId + 3, x + 205, y + 119);
    HWND tint = MakeEdit(baseId + 4, x + 205, y + 149);
    HWND animation = MakeEdit(baseId + 5, x + 205, y + 179, 125);
    if (baseId == IDC_F_TARGET) {
        g_controls.fTarget = target; g_controls.fGlass = glass; g_controls.fColor = color;
        g_controls.fGlassOpacity = glassOpacity; g_controls.fTint = tint; g_controls.fAnimation = animation;
    } else {
        g_controls.uTarget = target; g_controls.uGlass = glass; g_controls.uColor = color;
        g_controls.uGlassOpacity = glassOpacity; g_controls.uTint = tint; g_controls.uAnimation = animation;
    }
}

void MakeAppAppearanceGroup(const wchar_t* title, int baseId, int x, int y, bool focused) {
    MakeControl(L"BUTTON", title, BS_GROUPBOX, 0, x, y, 365, 220);
    MakeLabel(L"\x76ee\x6807\x900f\x660e\x5ea6 (0.05 - 1.0)", x + 16, y + 32);
    MakeLabel(L"\x542f\x7528\x6bdb\x73bb\x7483", x + 16, y + 62);
    MakeLabel(L"\x73bb\x7483/\x67d3\x8272\x989c\x8272 (#RRGGBB)", x + 16, y + 92);
    MakeLabel(L"\x73bb\x7483\x900f\x660e\x5ea6 (0 - 1)", x + 16, y + 122);
    MakeLabel(L"\x67d3\x8272\x5f3a\x5ea6 (0 - 1)", x + 16, y + 152);
    MakeLabel(L"\x52a8\x753b\x65f6\x957f (\x6beb\x79d2)", x + 16, y + 182);

    HWND target = MakeEdit(baseId, x + 205, y + 29);
    HWND glass = MakeControl(L"BUTTON", L"", BS_AUTOCHECKBOX, baseId + 1, x + 205, y + 59, 22, 22);
    HWND color = MakeEdit(baseId + 2, x + 205, y + 89, 125);
    HWND glassOpacity = MakeEdit(baseId + 3, x + 205, y + 119);
    HWND tint = MakeEdit(baseId + 4, x + 205, y + 149);
    HWND animation = MakeEdit(baseId + 5, x + 205, y + 179, 125);
    if (focused) {
        g_controls.appFTarget = target; g_controls.appFGlass = glass; g_controls.appFColor = color;
        g_controls.appFGlassOpacity = glassOpacity; g_controls.appFTint = tint; g_controls.appFAnimation = animation;
    } else {
        g_controls.appUTarget = target; g_controls.appUGlass = glass; g_controls.appUColor = color;
        g_controls.appUGlassOpacity = glassOpacity; g_controls.appUTint = tint; g_controls.appUAnimation = animation;
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
                   HWND glassOpacity, HWND tint, HWND animation) {
    SetControlText(target, std::to_wstring(appearance.targetOpacity));
    SendMessageW(glass, BM_SETCHECK, appearance.glass ? BST_CHECKED : BST_UNCHECKED, 0);
    SetControlText(color, ColorText(appearance.color));
    SetControlText(glassOpacity, std::to_wstring(appearance.glassOpacity));
    SetControlText(tint, std::to_wstring(appearance.tintOpacity));
    SetControlText(animation, std::to_wstring(appearance.animationMs));
}

void EnableAppRuleControls(bool enabled) {
    const HWND controls[] = {
        g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor, g_controls.appFGlassOpacity,
        g_controls.appFTint, g_controls.appFAnimation, g_controls.appUTarget, g_controls.appUGlass,
        g_controls.appUColor, g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation,
        g_controls.removeAppRule
    };
    for (HWND control : controls) if (control) EnableWindow(control, enabled);
}

void PutAppRule(const AppRule& rule) {
    PutAppearance(rule.focused, g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor,
                  g_controls.appFGlassOpacity, g_controls.appFTint, g_controls.appFAnimation);
    PutAppearance(rule.unfocused, g_controls.appUTarget, g_controls.appUGlass, g_controls.appUColor,
                  g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation);
}

void PopulateAppRuleList() {
    SendMessageW(g_controls.appRules, LB_RESETCONTENT, 0, 0);
    for (const auto& rule : g_config.appRules) SendMessageW(g_controls.appRules, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rule.process.c_str()));
    if (g_selectedAppRule >= static_cast<int>(g_config.appRules.size())) g_selectedAppRule = -1;
    if (g_selectedAppRule >= 0) {
        SendMessageW(g_controls.appRules, LB_SETCURSEL, g_selectedAppRule, 0);
        PutAppRule(g_config.appRules[static_cast<size_t>(g_selectedAppRule)]);
    }
    EnableAppRuleControls(g_selectedAppRule >= 0);
}

bool ReadAppRuleControls(AppRule& rule, bool showErrors) {
    const auto read = [&](Appearance& appearance, HWND target, HWND glass, HWND color,
                          HWND glassOpacity, HWND tint, HWND animation) {
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
        return true;
    };
    return read(rule.focused, g_controls.appFTarget, g_controls.appFGlass, g_controls.appFColor,
                g_controls.appFGlassOpacity, g_controls.appFTint, g_controls.appFAnimation) &&
           read(rule.unfocused, g_controls.appUTarget, g_controls.appUGlass, g_controls.appUColor,
                g_controls.appUGlassOpacity, g_controls.appUTint, g_controls.appUAnimation);
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
    g_pickerChosenProcess = candidate.process;
    g_pickerChosenDisplay = candidate.process + L"  [PID " + std::to_wstring(candidate.processId) + L"]";
    DestroyWindow(g_pickerWindow);
    return true;
}

void SetSelectedProcess(const std::wstring& process, const std::wstring& display) {
    g_selectedProcess = process;
    g_selectedProcessDisplay = display;
    const bool hasSelection = !g_selectedProcess.empty();
    if (g_controls.selectedProcess) {
        SetControlText(g_controls.selectedProcess, hasSelection ? g_selectedProcessDisplay : L"\x5c1a\x672a\x9009\x62e9\x8fdb\x7a0b");
    }
    if (g_controls.addProcessBlacklist) EnableWindow(g_controls.addProcessBlacklist, hasSelection);
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
        const wchar_t* pages[] = { L"\x5e94\x7528\x7a0b\x5e8f", L"\x8fdb\x7a0b", L"\x7a97\x53e3" };
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
                           selectedPage == 2 ? PickerPage::Windows : PickerPage::Applications;
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
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ProcessPickerProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"WinGlassProcessPicker";
    registered = RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

void OpenProcessPicker() {
    if (!EnsureProcessPickerClass()) return;
    EnumerateProcessCandidates();
    g_pickerChosenProcess.clear();
    g_pickerChosenDisplay.clear();
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
    if (!g_pickerChosenProcess.empty()) {
        SetSelectedProcess(g_pickerChosenProcess, g_pickerChosenDisplay);
        SetStatus(L"\x5df2\x9009\x62e9 " + g_pickerChosenDisplay + L"\x3002");
    }
}

void LoadToControls() {
    SendMessageW(g_controls.enabled, BM_SETCHECK, g_config.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    PutAppearance(g_config.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                  g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation);
    PutAppearance(g_config.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                  g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation);
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
                    HWND glassOpacity, HWND tint, HWND animation) {
    appearance.targetOpacity = ParseDouble(ControlText(target), appearance.targetOpacity, 0.05, 1.0);
    appearance.glass = SendMessageW(glass, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!ParseColor(ControlText(color), appearance.color)) {
        MessageBoxW(g_window, L"\x989c\x8272\x5fc5\x987b\x4f7f\x7528 #RRGGBB \x683c\x5f0f\x3002", L"\x989c\x8272\x65e0\x6548", MB_ICONWARNING);
        SetFocus(color);
        return false;
    }
    appearance.glassOpacity = ParseDouble(ControlText(glassOpacity), appearance.glassOpacity, 0.05, 1.0);
    appearance.tintOpacity = ParseDouble(ControlText(tint), appearance.tintOpacity, 0.0, 1.0);
    appearance.animationMs = ParseInt(ControlText(animation), appearance.animationMs, 0, 5000);
    return true;
}

void SetStatus(const std::wstring& text) {
    SetControlText(g_controls.status, text);
}

bool SaveFromControls() {
    EditorConfig updated = g_config;
    updated.enabled = SendMessageW(g_controls.enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!ReadAppearance(updated.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                        g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation) ||
        !ReadAppearance(updated.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                        g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation)) {
        return false;
    }
    updated.blacklistProcesses = ReadLines(g_controls.blacklistProcesses);
    updated.blacklistClasses = ReadLines(g_controls.blacklistClasses);
    if (g_selectedAppRule >= 0 && g_selectedAppRule < static_cast<int>(updated.appRules.size()) &&
        !ReadAppRuleControls(updated.appRules[static_cast<size_t>(g_selectedAppRule)], true)) {
        return false;
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
        SetStatus(L"\x8bf7\x5148\x9009\x62e9\x4e00\x4e2a\x8fd0\x884c\x8fdb\x7a0b\x3002");
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
    if (process.empty()) {
        SetStatus(L"\x8bf7\x5148\x9009\x62e9\x4e00\x4e2a\x8fd0\x884c\x8fdb\x7a0b\x3002");
        return;
    }
    CaptureSelectedAppRule(false);
    for (size_t i = 0; i < g_config.appRules.size(); ++i) {
        if (_wcsicmp(g_config.appRules[i].process.c_str(), process.c_str()) == 0) {
            g_selectedAppRule = static_cast<int>(i);
            PopulateAppRuleList();
            SetStatus(process + L" \x5df2\x5b58\x5728\x4e13\x5c5e\x89c4\x5219\x3002");
            return;
        }
    }
    // Copy the values currently shown in the global controls so the quick
    // rule feels like a focused override rather than a blank form.
    AppRule rule{};
    rule.process = process;
    rule.focused = g_config.focused;
    rule.unfocused = g_config.unfocused;
    ReadAppearance(rule.focused, g_controls.fTarget, g_controls.fGlass, g_controls.fColor,
                   g_controls.fGlassOpacity, g_controls.fTint, g_controls.fAnimation);
    ReadAppearance(rule.unfocused, g_controls.uTarget, g_controls.uGlass, g_controls.uColor,
                   g_controls.uGlassOpacity, g_controls.uTint, g_controls.uAnimation);
    g_config.appRules.push_back(std::move(rule));
    g_selectedAppRule = static_cast<int>(g_config.appRules.size() - 1);
    PopulateAppRuleList();
    if (SaveFromControls()) SetStatus(process + L" \x5df2\x521b\x5efa\x4e13\x5c5e\x89c4\x5219\x5e76\x6c38\x4e45\x4fdd\x5b58\x3002");
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
    g_controls.enabled = MakeControl(L"BUTTON", L"\x5168\x5c40\x542f\x7528 WinGlass", BS_AUTOCHECKBOX,
                                     IDC_ENABLED, 24, 18, 260, 24);
    MakeAppearanceGroup(L"\x805a\x7126\x7a97\x53e3", IDC_F_TARGET, 20, 55);
    MakeAppearanceGroup(L"\x5931\x6d3b\x7a97\x53e3", IDC_U_TARGET, 405, 55);

    MakeControl(L"BUTTON", L"\x8fd0\x884c\x8fdb\x7a0b\x4e0e\x4e13\x5c5e\x89c4\x5219", BS_GROUPBOX, 0, 20, 290, 750, 115);
    MakeLabel(L"\x5df2\x9009\x62e9\x8fdb\x7a0b\xff1a", 35, 318, 90);
    g_controls.selectedProcess = MakeLabel(L"\x5c1a\x672a\x9009\x62e9\x8fdb\x7a0b", 125, 318, 420);
    g_controls.pickProcess = MakeControl(L"BUTTON", L"\x9009\x62e9\x8fd0\x884c\x8fdb\x7a0b...", 0,
                                         IDC_PICK_PROCESS, 565, 313, 165, 28);
    g_controls.addProcessBlacklist = MakeControl(L"BUTTON", L"\x52a0\x5165\x9ed1\x540d\x5355\x5e76\x4fdd\x5b58", 0,
                                                   IDC_ADD_PROCESS_BLACKLIST, 350, 343, 170, 25);
    g_controls.addAppRule = MakeControl(L"BUTTON", L"\x521b\x5efa\x4e13\x5c5e\x89c4\x5219", 0,
                                         IDC_ADD_APP_RULE, 530, 343, 130, 25);
    MakeLabel(L"\x5df2\x6709\x4e13\x5c5e\x89c4\x5219", 35, 374, 100);
    g_controls.appRules = MakeControl(L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                      IDC_APP_RULES, 125, 371, 360, 25);
    g_controls.removeAppRule = MakeControl(L"BUTTON", L"\x5220\x9664\x5f53\x524d\x89c4\x5219", 0, IDC_REMOVE_APP_RULE, 500, 371, 120, 25);
    SetSelectedProcess(L"", L"");

    MakeAppAppearanceGroup(L"\x4e13\x5c5e\x89c4\x5219\xff1a\x805a\x7126\x7a97\x53e3", IDC_APP_F_TARGET, 20, 420, true);
    MakeAppAppearanceGroup(L"\x4e13\x5c5e\x89c4\x5219\xff1a\x5931\x6d3b\x7a97\x53e3", IDC_APP_U_TARGET, 405, 420, false);

    MakeControl(L"BUTTON", L"\x9ed1\x540d\x5355\x8fdb\x7a0b\xff08\x6bcf\x884c\x4e00\x4e2a\xff09", BS_GROUPBOX, 0, 20, 665, 365, 150);
    MakeControl(L"BUTTON", L"\x9ed1\x540d\x5355\x7a97\x53e3\x7c7b\x540d\xff08\x6bcf\x884c\x4e00\x4e2a\xff09", BS_GROUPBOX, 0, 405, 665, 365, 150);
    g_controls.blacklistProcesses = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                                  IDC_BLACK_PROCESSES, 35, 700, 335, 95);
    g_controls.blacklistClasses = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                               IDC_BLACK_CLASSES, 420, 700, 335, 95);
    SendMessageW(g_controls.blacklistProcesses, EM_SETLIMITTEXT, 8192, 0);
    SendMessageW(g_controls.blacklistClasses, EM_SETLIMITTEXT, 8192, 0);

    MakeLabel(L"\x9ed1\x540d\x5355\x4e0e\x4e13\x5c5e\x89c4\x5219\x4f1a\x5199\x5165 config.yaml\xff0c\x8fd0\x884c\x4e2d\x7684 WinGlass \x4f1a\x81ea\x52a8\x5e94\x7528\x3002", 24, 830, 740);
    g_controls.status = MakeLabel(L"", 24, 854, 740);
    MakeControl(L"BUTTON", L"\x4fdd\x5b58\x5e76\x5e94\x7528", BS_DEFPUSHBUTTON, IDC_SAVE, 410, 885, 150, 30);
    MakeControl(L"BUTTON", L"\x91cd\x65b0\x52a0\x8f7d", 0, IDC_RELOAD, 570, 885, 90, 30);
    MakeControl(L"BUTTON", L"\x6253\x5f00\x6587\x4ef6\x5939", 0, IDC_OPEN_FOLDER, 670, 885, 100, 30);
    MakeControl(L"BUTTON", L"\x5173\x95ed", 0, IDC_EXIT, 300, 885, 90, 30);
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
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SAVE: SaveFromControls(); return 0;
        case IDC_RELOAD: ReloadFromDisk(); return 0;
        case IDC_PICK_PROCESS: OpenProcessPicker(); return 0;
        case IDC_ADD_PROCESS_BLACKLIST: AddSelectedProcessToBlacklist(); return 0;
        case IDC_ADD_APP_RULE: AddSelectedProcessRule(); return 0;
        case IDC_REMOVE_APP_RULE: RemoveSelectedAppRule(); return 0;
        case IDC_APP_RULES:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                const int selected = static_cast<int>(SendMessageW(g_controls.appRules, LB_GETCURSEL, 0, 0));
                SelectAppRule(selected == LB_ERR ? -1 : selected);
            }
            return 0;
        case IDC_OPEN_FOLDER: OpenConfigFolder(); return 0;
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    g_instance = instance;
    // ListView itself can be registered by another application component, but
    // its image-list support is only dependable after explicit initialization.
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&commonControls);
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    g_configPath = module;
    const size_t slash = g_configPath.find_last_of(L"\\/");
    g_configPath = (slash == std::wstring::npos ? L"" : g_configPath.substr(0, slash + 1)) + L"config.yaml";

    g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = EditorProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"WinGlassConfigEditor";
    if (!RegisterClassW(&windowClass)) return 2;

    g_window = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName,
                               L"WinGlass \x914d\x7f6e\x7f16\x8f91\x5668", WS_OVERLAPPED | WS_CAPTION |
                               WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 810, 970,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 3;
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
