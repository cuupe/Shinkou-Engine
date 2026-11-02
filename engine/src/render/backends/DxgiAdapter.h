#pragma once
#include "BackendCommon.h"
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <dxgi1_4.h>
#include <windows.h>
namespace shinkou::render {
inline std::wstring wide_name(std::string_view name) {
    if (name.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), result.data(),
                        length);
    return result;
}

inline const std::wstring &platform_default_ui_font() {
    static const std::wstring value = [] {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE &&
            metrics.lfMessageFont.lfFaceName[0] != L'\0') {
            return std::wstring(metrics.lfMessageFont.lfFaceName);
        }
        // This is only a last-resort Windows adapter fallback. The portable
        // UI never depends on this family name.
        return std::wstring(L"Segoe UI");
    }();
    return value;
}

inline const std::wstring &platform_default_ui_locale() {
    static const std::wstring value = [] {
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
            return std::wstring(locale);
        return std::wstring(L"en-US");
    }();
    return value;
}

inline IDXGIAdapter1 *select_high_performance_adapter() {
    IDXGIFactory4 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
        return nullptr;
    const char *requestedAdapter = std::getenv("SHINKOU_GPU_ADAPTER");
    const bool preferIntegrated =
        requestedAdapter &&
        (_stricmp(requestedAdapter, "integrated") == 0 || _stricmp(requestedAdapter, "igpu") == 0);
    IDXGIAdapter1 *best = nullptr;
    std::uint64_t bestScore = 0;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            continue;
        }
        // DXGI does not expose a single universal "integrated" bit. Intel
        // adapters and hardware with little/no dedicated VRAM are reliable
        // indicators, while shared system memory is the primary IGPU pool.
        const bool integrated =
            description.VendorId == 0x8086u || description.DedicatedVideoMemory <= (1ull << 30);
        const auto score =
            preferIntegrated
                ? (integrated ? (1ull << 63) : 0ull) +
                      static_cast<std::uint64_t>(description.SharedSystemMemory) / 16ull
                : static_cast<std::uint64_t>(description.DedicatedVideoMemory) +
                      static_cast<std::uint64_t>(description.SharedSystemMemory) / 16ull;
        if (!best || score > bestScore) {
            if (best)
                best->Release();
            best = adapter;
            bestScore = score;
        } else {
            adapter->Release();
        }
    }
    factory->Release();
    return best;
}

} // namespace shinkou::render
#endif
