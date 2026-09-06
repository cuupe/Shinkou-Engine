#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

struct WindowSearch {
    DWORD processId{0};
    HWND window{nullptr};
};

struct Size {
    int width{0};
    int height{0};
};

BOOL CALLBACK find_process_window(HWND window, LPARAM rawContext) {
    auto& context = *reinterpret_cast<WindowSearch*>(rawContext);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != context.processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    context.window = window;
    return FALSE;
}

HWND wait_for_process_window(DWORD processId, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    WindowSearch context{processId, nullptr};
    while (GetTickCount() - start < timeoutMs) {
        context.window = nullptr;
        EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&context));
        if (context.window) return context.window;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

bool write_bitmap(const std::filesystem::path& output, const BITMAPINFO& info, const void* pixels,
                  std::size_t pixelBytes) {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = static_cast<DWORD>(header.bfOffBits + pixelBytes);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(pixelBytes));
    return file.good();
}

bool has_visible_pixels(const std::uint8_t* pixels, std::size_t pixelBytes) {
    if (!pixels || pixelBytes < 16) return false;
    const std::size_t pixelCount = pixelBytes / 4;
    const std::uint8_t* first = pixels;
    std::size_t different = 0;
    for (std::size_t index = 1; index < pixelCount; index += std::max<std::size_t>(1, pixelCount / 4096)) {
        const auto* pixel = pixels + index * 4;
        if (pixel[0] != first[0] || pixel[1] != first[1] || pixel[2] != first[2]) ++different;
    }
    return different >= 4;
}

bool read_bitmap_dimensions(const std::filesystem::path& output, int& width, int& height) {
    std::ifstream file(output, std::ios::binary);
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    file.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    file.read(reinterpret_cast<char*>(&infoHeader), sizeof(infoHeader));
    if (!file || fileHeader.bfType != 0x4D42 || infoHeader.biWidth <= 0 || infoHeader.biHeight == 0 ||
        infoHeader.biBitCount != 32 || infoHeader.biCompression != BI_RGB) return false;
    width = infoHeader.biWidth;
    height = infoHeader.biHeight < 0 ? -infoHeader.biHeight : infoHeader.biHeight;
    if (width <= 0 || height <= 0) return false;
    // The render thread writes the header before the pixels. A valid header
    // alone is not completion evidence and can produce truncated snapshots.
    file.seekg(0, std::ios::end);
    const auto bytes = file.tellg();
    const auto expected = static_cast<std::uint64_t>(fileHeader.bfOffBits) + static_cast<std::uint64_t>(width) * height * 4;
    return bytes >= 0 && static_cast<std::uint64_t>(bytes) >= expected;
}

bool parse_size(const std::wstring& value, Size& size) {
    const auto split = value.find_first_of(L"xX");
    if (split == std::wstring::npos) return false;
    try {
        size.width = std::stoi(value.substr(0, split));
        size.height = std::stoi(value.substr(split + 1));
    } catch (...) {
        return false;
    }
    return size.width > 0 && size.height > 0;
}

bool resize_client(HWND window, Size requested) {
    if (requested.width <= 0 || requested.height <= 0) return false;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT frame{0, 0, requested.width, requested.height};
    if (!AdjustWindowRectEx(&frame, style, GetMenu(window) != nullptr, exStyle)) return false;
    if (!SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)) return false;

    // AdjustWindowRectEx is evaluated in the caller's DPI context while the
    // target HWND may be per-monitor aware. Correct the result from the
    // target's real client rectangle so --client-size means the size the
    // editor actually receives, including the native menu bar.
    for (int attempt = 0; attempt < 4; ++attempt) {
        RECT client{};
        RECT bounds{};
        if (!GetClientRect(window, &client) || !GetWindowRect(window, &bounds)) return false;
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const int deltaWidth = requested.width - clientWidth;
        const int deltaHeight = requested.height - clientHeight;
        if (deltaWidth >= -1 && deltaWidth <= 1 && deltaHeight >= -1 && deltaHeight <= 1) return true;
        const int outerWidth = bounds.right - bounds.left + deltaWidth;
        const int outerHeight = bounds.bottom - bounds.top + deltaHeight;
        if (outerWidth <= 0 || outerHeight <= 0 ||
            !SetWindowPos(window, nullptr, 0, 0, outerWidth, outerHeight,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    RECT client{};
    return GetClientRect(window, &client) &&
        client.right - client.left == requested.width && client.bottom - client.top == requested.height;
}

bool capture_window(HWND window, const std::filesystem::path& output, std::string& mode,
                    int& width, int& height, int& clientWidth, int& clientHeight, UINT& dpi) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    width = std::max(1L, bounds.right - bounds.left);
    height = std::max(1L, bounds.bottom - bounds.top);
    RECT client{};
    if (GetClientRect(window, &client)) {
        clientWidth = std::max(0L, client.right - client.left);
        clientHeight = std::max(0L, client.bottom - client.top);
    }
    dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC windowDc = GetWindowDC(window);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    void* pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ previous = bitmap && memory ? SelectObject(memory, bitmap) : nullptr;

    bool copied = false;
    if (memory && bitmap && windowDc) {
        copied = BitBlt(memory, 0, 0, width, height, windowDc, 0, 0, SRCCOPY | CAPTUREBLT) != FALSE;
        if (copied && has_visible_pixels(static_cast<const std::uint8_t*>(pixels),
                                         static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u)) {
            mode = "WindowDC";
        } else {
            copied = false;
        }
    }
    if (!copied && memory && bitmap && screen) {
        copied = BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT) != FALSE;
    }
    if (!copied || !has_visible_pixels(static_cast<const std::uint8_t*>(pixels),
                                       static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u)) {
        // GPU-backed windows can be blank to GDI when the desktop compositor
        // is not exposing the surface. PrintWindow is the supported fallback
        // for a native Windows editor and also works while it is occluded.
        copied = memory && bitmap && PrintWindow(window, memory, PW_RENDERFULLCONTENT) != FALSE;
        mode = "PrintWindow";
    } else if (mode != "WindowDC") {
        mode = "BitBlt";
    }

    const std::size_t pixelBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const bool written = copied && write_bitmap(output, info, pixels, pixelBytes);
    if (previous && memory) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (windowDc) ReleaseDC(window, windowDc);
    if (screen) ReleaseDC(nullptr, screen);
    return written;
}

std::wstring quote_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') result += L'\\';
        result += character;
    }
    result += L"\"";
    return result;
}

#include "InteractionScript.h"

} // namespace

int run_capture(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::wcerr << L"Usage: shinkou_ui_capture.exe <editor.exe> <output.bmp> [wait-ms] "
                      L"[--client-size WxH] [--force-gdi] [--require-gpu] [editor args...]\n";
        return 2;
    }

    const std::filesystem::path executable(argv[1]);
    const std::filesystem::path output(argv[2]);
    DWORD waitMs = 1500;
    int firstArgument = 3;
    if (argc > 3) {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(argv[3], &end, 10);
        if (end != argv[3] && *end == L'\0') {
            waitMs = static_cast<DWORD>(std::min<unsigned long>(parsed, 30000));
            firstArgument = 4;
        }
    }

    std::filesystem::path scriptPath;
    const auto snapshotPath = std::filesystem::path(output).concat(".state.txt");
    const auto metricsPath = std::filesystem::path(output).concat(".timings.csv");
    std::optional<Size> requestedClientSize;
    bool forceGdi = false;
    bool requireGpu = false;
    std::vector<std::wstring> childArguments;
    for (int index = firstArgument; index < argc; ++index) {
        if (std::wstring(argv[index]) == L"--script" && index + 1 < argc) {
            scriptPath = argv[++index];
        } else if (std::wstring(argv[index]) == L"--force-gdi") {
            forceGdi = true;
        } else if (std::wstring(argv[index]) == L"--require-gpu") {
            requireGpu = true;
        } else if (std::wstring(argv[index]) == L"--client-size" && index + 1 < argc) {
            Size parsed{};
            if (!parse_size(argv[++index], parsed)) {
                std::wcerr << L"Invalid --client-size; expected WIDTHxHEIGHT\n";
                return 2;
            }
            requestedClientSize = parsed;
        } else {
            childArguments.emplace_back(argv[index]);
        }
    }

    std::wstring commandLine = quote_argument(executable.wstring());
    for (const auto& argument : childArguments) {
        commandLine += L' ';
        commandLine += quote_argument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    // Shinkou's D3D11 backend can read the presented client surface directly.
    // Prefer that ground-truth image over GDI, which cannot reliably sample a
    // covered or GPU-composited window. Other Windows applications simply
    // ignore this private environment variable and use the fallback below.
    DeleteFileW(output.wstring().c_str());
    const auto kindPath = std::filesystem::path(output).concat(".kind");
    DeleteFileW(kindPath.wstring().c_str());
    if (!forceGdi) SetEnvironmentVariableW(L"SHINKOU_UI_CAPTURE_PATH", output.wstring().c_str());

    if (!scriptPath.empty()) SetEnvironmentVariableW(L"SHINKOU_UI_QA_SNAPSHOT", snapshotPath.wstring().c_str());
    SetEnvironmentVariableW(L"SHINKOU_UI_METRICS_PATH",metricsPath.wstring().c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto workingDirectory = executable.parent_path().empty() ? std::filesystem::current_path() : executable.parent_path();
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workingDirectory.wstring().c_str(), &startup, &process)) {
        if (!forceGdi) SetEnvironmentVariableW(L"SHINKOU_UI_CAPTURE_PATH", nullptr);
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\n";
        return 3;
    }
    if (!forceGdi) SetEnvironmentVariableW(L"SHINKOU_UI_CAPTURE_PATH", nullptr);

    SetEnvironmentVariableW(L"SHINKOU_UI_QA_SNAPSHOT",nullptr);
    SetEnvironmentVariableW(L"SHINKOU_UI_METRICS_PATH",nullptr);
    HWND window = wait_for_process_window(process.dwProcessId, waitMs);
    if (!window) {
        std::wcerr << L"No visible top-level window found for process " << process.dwProcessId << L"\n";
        TerminateProcess(process.hProcess, 4);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 4;
    }

    if (requestedClientSize) {
        if (!resize_client(window, *requestedClientSize)) {
            std::wcerr << L"Unable to resize target client area\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Give DWM a real presented frame before using a window-surface fallback.
    // This is especially important when the capture process itself was the
    // foreground application that launched the editor.
    ShowWindow(window, SW_RESTORE);
    AllowSetForegroundWindow(process.dwProcessId);
    SetForegroundWindow(window);
    BringWindowToTop(window);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // The engine shows its HWND before device creation finishes. Wait for the
    // first GPU readback to appear; a fixed sleep is racy on shader-compiling
    // or WARP machines.
    bool engineCapture = false;
    std::string engineCaptureKind = "EngineGpuReadback";
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    const DWORD captureStart = GetTickCount();
    while (!forceGdi && GetTickCount() - captureStart < std::max<DWORD>(750, waitMs)) {
        std::error_code fileError;
        engineCapture = std::filesystem::exists(output, fileError) &&
            std::filesystem::file_size(output, fileError) > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        if (engineCapture && read_bitmap_dimensions(output, surfaceWidth, surfaceHeight)) break;
        engineCapture = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Re-arm after resize so the image is the final physical client surface.
    if (engineCapture) {
        DeleteFileW(output.wstring().c_str()); PostMessageW(window,WM_COMMAND,49001,0);
        engineCapture = false;
        for (int i=0;i<150;++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (read_bitmap_dimensions(output,surfaceWidth,surfaceHeight)) {engineCapture=true;break;}
        }
    }
    bool interactionsPassed = scriptPath.empty() || run_interaction_script(window,scriptPath,output,snapshotPath);
    if (!scriptPath.empty()) read_bitmap_dimensions(output,surfaceWidth,surfaceHeight);
    DwmFlush();
    std::string mode;
    int width = 0;
    int height = 0;
    int clientWidth = 0;
    int clientHeight = 0;
    UINT dpi = 96;
    const bool captured = engineCapture ? true : capture_window(window, output, mode, width, height,
                                                                clientWidth, clientHeight, dpi);
    if (engineCapture) {
        std::ifstream kindFile(std::filesystem::path(output).concat(".kind"));
        if (kindFile) std::getline(kindFile, engineCaptureKind);
        mode = engineCaptureKind.empty() ? "EngineGpuReadback" : engineCaptureKind;
        RECT bounds{};
        if (GetWindowRect(window, &bounds)) {
            width = std::max(1L, bounds.right - bounds.left);
            height = std::max(1L, bounds.bottom - bounds.top);
        }
        RECT client{};
        if (GetClientRect(window, &client)) {
            clientWidth = std::max(0L, client.right - client.left);
            clientHeight = std::max(0L, client.bottom - client.top);
        }
        dpi = GetDpiForWindow(window);
        if (dpi == 0) dpi = 96;
    } else {
        surfaceWidth = width;
        surfaceHeight = height;
    }
    const std::wstring wideMode(mode.begin(), mode.end());
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    std::wcout << L"window=" << reinterpret_cast<std::uintptr_t>(window)
               << L" title=\"" << title << L"\""
               << L" size=" << width << L"x" << height
               << L" client=" << clientWidth << L"x" << clientHeight
               << L" surface=" << (engineCapture ? surfaceWidth : width) << L"x"
               << (engineCapture ? surfaceHeight : height)
               << L" dpi=" << dpi
               << L" mode=" << wideMode
               << L" surface-kind=" << (engineCapture
                   ? (mode == "EngineUiDib" ? L"UiLayerSurface" : L"GpuClientSurface")
                   : (captured ? L"GdiWindowSurface" : L"None"))
               << L" output=" << output.wstring()
               << L" captured=" << (captured ? L"1" : L"0") << L"\n";

    PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!interactionsPassed) return 8;
    if (requireGpu && (!engineCapture || (mode != "EngineGpuReadback" && mode != "EngineGdiCopy"))) return 6;
    if (requestedClientSize && engineCapture &&
        (clientWidth != requestedClientSize->width || clientHeight != requestedClientSize->height)) return 7;
    return captured ? 0 : 5;
}

int main() {
    using SetAwareness = BOOL(WINAPI*)(HANDLE);
    if (auto proc = GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext")) {
        auto set = reinterpret_cast<SetAwareness>(proc); set(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4)));
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    const int result = run_capture(argc, argv);
    LocalFree(argv);
    return result;
}
