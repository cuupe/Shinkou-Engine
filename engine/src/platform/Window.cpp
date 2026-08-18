#include "shinkou/platform/Window.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>

namespace {
LRESULT CALLBACK ShinkouWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_COMMAND && HIWORD(wParam) == 0) {
        auto* owner = reinterpret_cast<shinkou::platform::Window*>(GetWindowLongPtrA(window, GWLP_USERDATA));
        if (owner) owner->dispatch_menu_command(static_cast<std::uint32_t>(LOWORD(wParam)));
        return 0;
    }
    if (message == WM_CLOSE || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}
}
#endif

namespace shinkou::platform {
bool Window::create(const WindowConfig& config) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    static const char* className = "ShinkouEngineWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSA windowClass{};
        windowClass.lpfnWndProc = ShinkouWindowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height)};
    AdjustWindowRect(&rect, style, FALSE);
    HWND window = CreateWindowExA(0, className, config.title.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), this);
    if (!window) return false;
    nativeHandle_ = window;
    open_ = true;
    if (config.visible) ShowWindow(window, SW_SHOW);
    return true;
#else
    (void)config;
    nativeHandle_ = nullptr;
    open_ = true;
    return true;
#endif
}

void Window::process_events() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) open_ = false;
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
#endif
}

void Window::destroy() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (nativeHandle_) DestroyWindow(static_cast<HWND>(nativeHandle_));
#endif
    nativeHandle_ = nullptr;
    open_ = false;
}
}
