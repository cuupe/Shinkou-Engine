#include "shinkou/render/RenderBackend.h"

namespace shinkou::render {
std::unique_ptr<IRenderBackend> create_null_backend(BackendApi api);
#if defined(SHINKOU_PLATFORM_WINDOWS)
std::unique_ptr<IRenderBackend> create_directx11_backend();
std::unique_ptr<IRenderBackend> create_directx12_backend();
#endif
#if defined(SHINKOU_WITH_VULKAN)
std::unique_ptr<IRenderBackend> create_vulkan_backend();
#endif

std::unique_ptr<IRenderBackend> create_backend(BackendApi api) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (api == BackendApi::DirectX11) return create_directx11_backend();
    if (api == BackendApi::DirectX12) return create_directx12_backend();
#endif
#if defined(SHINKOU_WITH_VULKAN)
    if (api == BackendApi::Vulkan) return create_vulkan_backend();
#endif
    return create_null_backend(api);
}
}
