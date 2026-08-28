#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "shinkou/uikit/ShinkouUI.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

using namespace shinkou::uikit;

namespace {

constexpr int kThemeButton = 1001;
constexpr int kApplyButton = 1002;
constexpr int kSaveButton = 1003;
constexpr int kLoadButton = 1004;
constexpr int kAccentEdit = 1005;
constexpr int kRadiusTrack = 1006;
constexpr int kFontTrack = 1007;

HWND g_window = nullptr;
HWND g_themeButton = nullptr;
HWND g_accentEdit = nullptr;
HWND g_radiusTrack = nullptr;
HWND g_fontTrack = nullptr;
StyleSheet g_style = StyleSheet::make_windows11_light();
float g_radius = 4.0f;
float g_fontSize = 14.0f;

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, length)), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
COLORREF colorref(Color color) { return RGB(static_cast<BYTE>(clamp01(color.r) * 255), static_cast<BYTE>(clamp01(color.g) * 255), static_cast<BYTE>(clamp01(color.b) * 255)); }
std::wstring hex(Color color) {
    std::wstringstream stream;
    stream << L'#' << std::hex << std::uppercase << std::setw(6) << std::setfill(L'0') << (color.to_rgba8() >> 8);
    return stream.str();
}
void fill(HDC dc, RECT rect, COLORREF color) { HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush); }

void draw_list(HDC dc, const RenderList& list) {
    SetBkMode(dc, TRANSPARENT);
    for (const DrawCommand& command : list.commands()) {
        const RECT rect{static_cast<int>(command.rect.x), static_cast<int>(command.rect.y), static_cast<int>(command.rect.x + command.rect.width), static_cast<int>(command.rect.y + command.rect.height)};
        switch (command.type) {
        case DrawCommandType::Rect: {
            HBRUSH brush = CreateSolidBrush(colorref(command.color));
            const int radius = static_cast<int>(std::max({command.radii.left, command.radii.top, command.radii.right, command.radii.bottom}));
            if (radius > 0) { HGDIOBJ old = SelectObject(dc, brush); RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2); SelectObject(dc, old); }
            else FillRect(dc, &rect, brush);
            DeleteObject(brush);
            break;
        }
        case DrawCommandType::Border: {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(command.thickness)), colorref(command.color));
            HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
            break;
        }
        case DrawCommandType::Line: {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(command.thickness)), colorref(command.color));
            HGDIOBJ old = SelectObject(dc, pen); MoveToEx(dc, static_cast<int>(command.from.x), static_cast<int>(command.from.y), nullptr); LineTo(dc, static_cast<int>(command.to.x), static_cast<int>(command.to.y)); SelectObject(dc, old); DeleteObject(pen);
            break;
        }
        case DrawCommandType::Text: {
            HFONT font = CreateFontW(-static_cast<int>(command.fontSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, wide(command.fontFamily).c_str());
            HGDIOBJ old = SelectObject(dc, font); SetTextColor(dc, colorref(command.color)); RECT textRect = rect; textRect.left += 12; DrawTextW(dc, wide(command.text).c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); SelectObject(dc, old); DeleteObject(font);
            break;
        }
        case DrawCommandType::Gradient:
            fill(dc, rect, colorref(command.color));
            break;
        case DrawCommandType::BeginClip:
        case DrawCommandType::EndClip:
            break;
        case DrawCommandType::Image:
            fill(dc, rect, RGB(226, 232, 240));
            break;
        }
    }
}

void add_text(RenderList& list, float x, float y, float width, float height, const std::string& value, Color color, float size = -1.0f) { list.text({x, y, width, height}, value, color, g_style.font.family, size > 0 ? size : g_fontSize); }

void build_preview(RenderList& list, int width, int height) {
    const Color window = g_style.color("window", g_style.windowBackground);
    const Color surface = g_style.color("surface", g_style.surface);
    const Color elevated = g_style.color("surface-elevated", g_style.surfaceElevated);
    const Color text = g_style.color("text", Color::from_hex("#1f2937"));
    const Color secondary = g_style.color("text-secondary", Color::from_hex("#64748b"));
    const Color accent = g_style.color("accent", Color::from_hex("#2563eb"));
    const ControlStyle button = g_style.control("button");
    const ControlStyle input = g_style.control("input");
    const float left = 212.0f;
    const float right = std::max(left + 560.0f, static_cast<float>(width - 360));
    list.rect({0, 0, static_cast<float>(width), static_cast<float>(height)}, window);
    list.rect({0, 0, 188, static_cast<float>(height)}, surface);
    list.border({188, 0, 1, static_cast<float>(height)}, g_style.color("border"), 1);
    add_text(list, 24, 24, 150, 32, "SHINKOU UI", text, 16);
    add_text(list, 24, 55, 150, 24, "Style Studio", secondary, 12);
    list.rect({16, 112, 156, 38}, Color::from_hex("#dbeafe"), {6, 6, 6, 6});
    add_text(list, 30, 112, 130, 38, "样式预览", accent, 13);
    add_text(list, 30, 162, 130, 32, "控件", secondary, 13);
    add_text(list, 30, 202, 130, 32, "字体", secondary, 13);
    add_text(list, 30, 242, 130, 32, "动画", secondary, 13);
    list.rect({left, 24, right - left, 56}, elevated, {8, 8, 8, 8});
    add_text(list, left + 20, 24, 340, 56, "实时预览 / Windows 11", text, 16);
    add_text(list, right - 180, 24, 150, 56, g_style.activeTheme, secondary, 12);
    const float contentY = 104;
    list.rect({left, contentY, right - left, height - contentY - 24.0f}, surface, {8, 8, 8, 8});
    add_text(list, left + 28, contentY + 24, 400, 30, "控件状态", text, 16);
    add_text(list, left + 28, contentY + 64, 400, 24, "悬停、按下、输入和进度控件的统一视觉预览", secondary, 12);
    const float cardY = contentY + 112;
    list.rect({left + 28, cardY, 220, 180}, window, {6, 6, 6, 6});
    add_text(list, left + 48, cardY + 18, 170, 28, "按钮", secondary, 12);
    list.rect({left + 48, cardY + 58, 170, 38}, button.background, {g_radius, g_radius, g_radius, g_radius});
    add_text(list, left + 48, cardY + 58, 170, 38, "普通按钮", button.text, g_fontSize);
    list.rect({left + 48, cardY + 110, 170, 38}, accent, {g_radius, g_radius, g_radius, g_radius});
    add_text(list, left + 48, cardY + 110, 170, 38, "主要操作", Color::from_hex("#ffffff"), g_fontSize);
    list.rect({left + 276, cardY, right - left - 304, 180}, window, {6, 6, 6, 6});
    add_text(list, left + 296, cardY + 18, 240, 28, "输入与滑块", secondary, 12);
    list.rect({left + 296, cardY + 58, right - left - 344, 36}, input.background, {3, 3, 3, 3});
    list.border({left + 296, cardY + 58, right - left - 344, 36}, input.border, input.borderWidth);
    add_text(list, left + 296, cardY + 58, right - left - 344, 36, "Microsoft YaHei / 可编辑文本", input.text, g_fontSize);
    const float lineY = cardY + 132;
    list.line({left + 296, lineY}, {right - 48, lineY}, input.border, 3);
    list.line({left + 296, lineY}, {left + 430, lineY}, accent, 3);
    list.rect({left + 424, lineY - 7, 14, 14}, accent, {7, 7, 7, 7});
    list.rect({left + 28, cardY + 206, right - left - 56, 76}, elevated, {0, 0, 0, 0});
    add_text(list, left + 48, cardY + 218, 360, 24, "扁平化层级", text, 14);
    add_text(list, left + 48, cardY + 248, right - left - 96, 22, "面板保持方正，交互控件使用有限圆角，减少视觉噪声。", secondary, 12);
}

void layout_controls(int width, int height) {
    const int x = std::max(980, width - 332);
    MoveWindow(g_themeButton, x, 32, 280, 32, TRUE);
    MoveWindow(g_accentEdit, x, 150, 280, 28, TRUE);
    MoveWindow(g_radiusTrack, x, 238, 280, 32, TRUE);
    MoveWindow(g_fontTrack, x, 334, 280, 32, TRUE);
    MoveWindow(GetDlgItem(g_window, kApplyButton), x, 420, 132, 34, TRUE);
    MoveWindow(GetDlgItem(g_window, kSaveButton), x + 148, 420, 132, 34, TRUE);
    MoveWindow(GetDlgItem(g_window, kLoadButton), x, 464, 280, 34, TRUE);
    (void)height;
}

void update_controls() {
    SetWindowTextW(g_themeButton, g_style.activeTheme.find("dark") != std::string::npos ? L"切换到浅色主题" : L"切换到深色主题");
    SetWindowTextW(g_accentEdit, hex(g_style.color("accent" )).c_str());
    SendMessageW(g_radiusTrack, TBM_SETPOS, TRUE, static_cast<LPARAM>(g_radius));
    SendMessageW(g_fontTrack, TBM_SETPOS, TRUE, static_cast<LPARAM>(g_fontSize));
}

void apply_values() {
    wchar_t value[64]{};
    GetWindowTextW(g_accentEdit, value, 64);
    const Color accent = Color::from_hex(utf8(value));
    if (accent.a > 0.0f || utf8(value) == "#00000000") {
        g_style.palette["accent"] = accent;
        g_style.controls["primary-button"].background = accent;
        g_style.controls["primary-button"].border = accent;
    }
    g_radius = static_cast<float>(SendMessageW(g_radiusTrack, TBM_GETPOS, 0, 0));
    g_fontSize = static_cast<float>(SendMessageW(g_fontTrack, TBM_GETPOS, 0, 0));
    g_style.font.size = g_fontSize;
    g_style.controls["button"].radii = {g_radius, g_radius, g_radius, g_radius};
    g_style.controls["primary-button"].radii = {g_radius, g_radius, g_radius, g_radius};
    InvalidateRect(g_window, nullptr, FALSE);
}

void save_style() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = g_window; dialog.lpstrFilter = L"Shinkou UI XML (*.xml)\0*.xml\0All files (*.*)\0*.*\0"; dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH; dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; dialog.lpstrDefExt = L"xml";
    if (GetSaveFileNameW(&dialog)) { std::ofstream output(utf8(path), std::ios::binary); output << g_style.to_xml_string(); }
}
void load_style() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = g_window; dialog.lpstrFilter = L"Shinkou UI XML (*.xml)\0*.xml\0All files (*.*)\0*.*\0"; dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH; dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;
    std::ifstream input(utf8(path), std::ios::binary); std::stringstream buffer; buffer << input.rdbuf(); std::string error;
    StyleSheet loaded = StyleSheet::from_xml(buffer.str(), &error);
    if (!loaded.valid()) { MessageBoxW(g_window, wide("样式 XML 加载失败：" + error).c_str(), L"Shinkou UI Studio", MB_ICONERROR); return; }
    g_style = std::move(loaded); g_radius = g_style.control("button").radii.topLeft; g_fontSize = g_style.font.size; update_controls(); InvalidateRect(g_window, nullptr, FALSE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        g_themeButton = CreateWindowW(L"BUTTON", L"切换到深色主题", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kThemeButton), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"强调色（HEX）", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        g_accentEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"#2563EB", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kAccentEdit), nullptr, nullptr);
        g_radiusTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kRadiusTrack), nullptr, nullptr);
        g_fontTrack = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kFontTrack), nullptr, nullptr);
        SendMessageW(g_radiusTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 16)); SendMessageW(g_fontTrack, TBM_SETRANGE, TRUE, MAKELONG(10, 24));
        CreateWindowW(L"BUTTON", L"应用修改", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kApplyButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"保存 XML", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kSaveButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"加载 XML", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(kLoadButton), nullptr, nullptr);
        RECT client{};
        GetClientRect(window, &client);
        layout_controls(static_cast<int>(client.right), static_cast<int>(client.bottom));
        update_controls();
        return 0;
    }
    case WM_SIZE: layout_controls(LOWORD(lParam), HIWORD(lParam)); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kThemeButton) { g_style = g_style.activeTheme.find("dark") != std::string::npos ? StyleSheet::make_windows11_light() : StyleSheet::make_windows11_dark(); g_radius = g_style.control("button").radii.topLeft; g_fontSize = g_style.font.size; update_controls(); InvalidateRect(window, nullptr, FALSE); }
        else if (LOWORD(wParam) == kApplyButton) apply_values();
        else if (LOWORD(wParam) == kSaveButton) save_style();
        else if (LOWORD(wParam) == kLoadButton) load_style();
        return 0;
    case WM_HSCROLL: if (reinterpret_cast<HWND>(lParam) == g_radiusTrack || reinterpret_cast<HWND>(lParam) == g_fontTrack) apply_values(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT client{}; GetClientRect(window, &client); HBRUSH brush = CreateSolidBrush(colorref(g_style.color("window", g_style.windowBackground))); FillRect(dc, &client, brush); DeleteObject(brush);
        RenderList list; build_preview(list, client.right, client.bottom); draw_list(dc, list);
        SetTextColor(dc, colorref(g_style.color("text-secondary"))); SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei"); HGDIOBJ old = SelectObject(dc, font);
        const int x = std::max(980, static_cast<int>(client.right) - 332); TextOutW(dc, x, 108, L"属性", 2); TextOutW(dc, x, 128, L"强调色（HEX）", 12); TextOutW(dc, x, 208, L"控件圆角", 8); TextOutW(dc, x, 304, L"字体大小", 8); TextOutW(dc, x, 386, L"修改会立即影响中央预览", 20); SelectObject(dc, old); DeleteObject(font);
        EndPaint(window, &paint); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    SetProcessDPIAware();
    const wchar_t className[] = L"ShinkouUIStyleStudio";
    WNDCLASSW windowClass{}; windowClass.hInstance = instance; windowClass.lpfnWndProc = window_proc; windowClass.lpszClassName = className; windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, className, L"ShinkouUI Style Studio", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_MAXIMIZE); UpdateWindow(window);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
