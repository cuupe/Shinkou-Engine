#include "shinkou/platform/Window.h"

#include <algorithm>
#include <cstring>

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
    if (message == WM_SIZE) {
        auto* owner = reinterpret_cast<shinkou::platform::Window*>(GetWindowLongPtrA(window, GWLP_USERDATA));
        if (owner) owner->set_client_size(static_cast<std::uint32_t>(LOWORD(lParam)), static_cast<std::uint32_t>(HIWORD(lParam)));
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_DPICHANGED) {
        const auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOACTIVATE|SWP_NOZORDER);
        return 0;
    }
    if (message == WM_CLOSE) {
        auto* owner = reinterpret_cast<shinkou::platform::Window*>(GetWindowLongPtrA(window,GWLP_USERDATA));
        if (owner && !owner->request_close()) return 0;
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
    // Opt into the native per-monitor coordinate space before creating the
    // first HWND. Without this, Windows virtualizes GetClientRect/WM_SIZE on
    // a 125%/150% display and the editor's logical layout, swapchain and
    // RenderView seam disagree by the DPI factor.
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    if (const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                            "SetProcessDpiAwarenessContext")) {
        SetProcessDpiAwarenessContextFn setAwareness = nullptr;
        static_assert(sizeof(setAwareness) == sizeof(address));
        std::memcpy(&setAwareness, &address, sizeof(setAwareness));
        if (setAwareness) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is -4 on Windows 10.
            (void)setAwareness(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4)));
        }
    }
    static const char* className = "ShinkouEngineWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSA windowClass{};
        windowClass.lpfnWndProc = ShinkouWindowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
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
    width_ = config.width;
    height_ = config.height;
    if (config.visible) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    RECT client{};
    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
        // The renderer consumes framebuffer pixels, not the logical outer
        // window size. Resolve the actual client extent once so the first
        // editor frame does not immediately recreate the swapchain.
        width_ = static_cast<std::uint32_t>(client.right - client.left);
        height_ = static_cast<std::uint32_t>(client.bottom - client.top);
    }
    return true;
#else
    (void)config;
    nativeHandle_ = nullptr;
    width_ = 0;
    height_ = 0;
    open_ = true;
    width_ = config.width;
    height_ = config.height;
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
    width_ = 0;
    height_ = 0;
    open_ = false;
}

float Window::dpi_scale() const noexcept {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (nativeHandle_) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static const GetDpiForWindowFn getDpi = [] {
            FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
            GetDpiForWindowFn function = nullptr;
            static_assert(sizeof(function) == sizeof(address));
            std::memcpy(&function, &address, sizeof(function));
            return function;
        }();
        if (getDpi) return std::max(0.25f, static_cast<float>(getDpi(static_cast<HWND>(nativeHandle_))) / 96.0f);
    }
#endif
    return 1.0f;
}
}
