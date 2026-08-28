#include "shinkou/uikit/Win32Platform.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <string>
#endif

namespace shinkou::uikit {
namespace {
#if defined(_WIN32)
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string utf8(const wchar_t* value) {
    if (!value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, size - 1)), '\0');
    if (!result.empty()) WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring dialog_filter(const std::string& value) {
    std::wstring result = wide(value.empty() ? "All files (*.*)|*.*" : value);
    for (wchar_t& character : result) if (character == L'|') character = L'\0';
    result.push_back(L'\0');
    return result;
}
#endif
}

bool Win32PlatformAdapter::initialize(void* nativeWindow) { m_nativeWindow = nativeWindow; return true; }
void Win32PlatformAdapter::shutdown() { m_nativeWindow = nullptr; }
void Win32PlatformAdapter::poll_events(UiContext&) {}
float Win32PlatformAdapter::dpi_scale(void* nativeWindow) const {
#if defined(_WIN32)
    const HWND window = static_cast<HWND>(nativeWindow ? nativeWindow : m_nativeWindow);
    if (window) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static const GetDpiForWindowFn getDpi = [] {
            FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
            GetDpiForWindowFn function = nullptr;
            static_assert(sizeof(function) == sizeof(address));
            std::memcpy(&function, &address, sizeof(function));
            return function;
        }();
        if (getDpi) return static_cast<float>(getDpi(window)) / 96.0f;
    }
#else
    (void)nativeWindow;
#endif
    return 1.0f;
}
std::string Win32PlatformAdapter::font_directory() const {
#if defined(_WIN32)
    return "C:/Windows/Fonts";
#else
    return {};
#endif
}
std::string Win32PlatformAdapter::open_file_dialog(const std::string& title, const std::string& filter) {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const std::wstring wideTitle = wide(title);
    const std::wstring wideFilter = dialog_filter(filter);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(m_nativeWindow);
    dialog.lpstrTitle = wideTitle.c_str();
    dialog.lpstrFilter = wideFilter.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? utf8(buffer) : std::string{};
#else
    (void)title; (void)filter; return {};
#endif
}
std::string Win32PlatformAdapter::save_file_dialog(const std::string& title, const std::string& filter) {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const std::wstring wideTitle = wide(title);
    const std::wstring wideFilter = dialog_filter(filter);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(m_nativeWindow);
    dialog.lpstrTitle = wideTitle.c_str();
    dialog.lpstrFilter = wideFilter.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? utf8(buffer) : std::string{};
#else
    (void)title; (void)filter; return {};
#endif
}
bool Win32PlatformAdapter::read_clipboard(std::string& value) const {
#if defined(_WIN32)
    const HWND window = static_cast<HWND>(m_nativeWindow);
    if (!OpenClipboard(window)) return false;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    value = text ? utf8(text) : std::string{};
    if (text) GlobalUnlock(handle);
    CloseClipboard();
    return handle != nullptr;
#else
    (void)value; return false;
#endif
}
bool Win32PlatformAdapter::write_clipboard(const std::string& value) const {
#if defined(_WIN32)
    const HWND window = static_cast<HWND>(m_nativeWindow);
    if (!OpenClipboard(window)) return false;
    EmptyClipboard();
    const std::wstring text = wide(value);
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { CloseClipboard(); return false; }
    void* target = GlobalLock(memory);
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    const bool success = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!success) GlobalFree(memory);
    CloseClipboard();
    return success;
#else
    (void)value; return false;
#endif
}
PlatformCapabilities Win32PlatformAdapter::capabilities() const {
    return {true, true, true, true};
}

} // namespace shinkou::uikit
