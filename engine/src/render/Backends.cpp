#include "shinkou/render/RenderBackend.h"
#include "shinkou/render/GpuMemoryAllocator.h"
#include "shinkou/render/QueueSync.h"
#include "shinkou/render/ShaderCompiler.h"
#include "shinkou/render/TextureBackendMapping.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <variant>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#endif

#if defined(SHINKOU_WITH_VULKAN)
#include <vulkan/vulkan.h>
#endif

#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <backends/imgui_impl_dx11.h>
#endif
#if defined(SHINKOU_WITH_VULKAN)
#include <backends/imgui_impl_vulkan.h>
#endif
#endif

namespace shinkou::render {
namespace {
#if defined(SHINKOU_PLATFORM_WINDOWS)
std::wstring wide_name(std::string_view name) {
    if (name.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), result.data(), length);
    return result;
}
#endif
const char* descriptor_type_name(DescriptorType type) {
    switch (type) {
    case DescriptorType::Texture: return "texture";
    case DescriptorType::Sampler: return "sampler";
    case DescriptorType::UniformBuffer: return "uniform_buffer";
    case DescriptorType::StorageBuffer: return "storage_buffer";
    case DescriptorType::StorageTexture: return "storage_texture";
    case DescriptorType::StructuredBuffer: return "structured_buffer";
    }
    return "unknown";
}

bool shader_binding_matches(const ShaderBinding& reflected, const DescriptorBinding& requested) {
    return reflected.slot == requested.slot && reflected.space == requested.space &&
        reflected.type == descriptor_type_name(requested.type) && reflected.count == requested.count &&
        reflected.unbounded == requested.unbounded;
}

bool validate_backend_shader_layout(const std::vector<ShaderBinding>& bindings, std::string& error) {
    std::vector<ShaderBinding> merged;
    if (!merge_shader_bindings(bindings, merged, error)) return false;
    for (const auto& binding : merged) {
        if (binding.count == 0 && !binding.unbounded) {
            error = "shader descriptor has an invalid zero-sized array";
            return false;
        }
    }
    return true;
}

std::uint64_t d3d12_binding_key(std::uint32_t space, std::uint32_t slot, std::uint32_t type) {
    return (static_cast<std::uint64_t>(space) << 32u) |
        (static_cast<std::uint64_t>(slot) << 3u) | static_cast<std::uint64_t>(type);
}

bool is_color_attachment_usage(ResourceUsage usage) {
    return usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7;
}

bool is_supported_color_format(std::string_view format) {
    return format == "rgba8" || format == "bgra8" || format == "rgba16f" || format == "r32f";
}

bool is_supported_pipeline_depth_format(std::string_view format) {
    return format == "d24s8" || format == "none";
}

bool resource_description_matches_kind(ResourceKind kind, const ResourceDesc& description) {
    switch (kind) {
    case ResourceKind::Texture2D:
    case ResourceKind::DepthStencil: return std::holds_alternative<TextureDesc>(description);
    case ResourceKind::Buffer: return std::holds_alternative<BufferDesc>(description);
    case ResourceKind::Shader: return std::holds_alternative<ShaderDesc>(description);
    case ResourceKind::Pipeline: return std::holds_alternative<PipelineDesc>(description);
    case ResourceKind::Material: return std::holds_alternative<MaterialDesc>(description);
    case ResourceKind::Sampler: return std::holds_alternative<SamplerDesc>(description);
    }
    return false;
}

bool prepare_backend_texture(const TextureDesc& description, BackendApi backend,
                             TexturePhysicalImagePlan& plan, std::string& error) {
    const auto mapped = map_texture_to_backend(description, backend,
                                                TextureCapabilities::for_backend(backend));
    if (!mapped) {
        error = mapped.error_summary();
        if (error.empty()) error = "texture backend mapping failed";
        return false;
    }
    if (mapped.plan.logicalDimension == TextureDimension::Texture3D || mapped.plan.cubeCompatible) {
        error = "texture dimension is valid but this backend's existing view/update path only supports 2D and 2D arrays";
        return false;
    }
    if (mapped.plan.formatDowngraded) {
        error = "texture format downgrade is disabled for native resource creation";
        return false;
    }
    if (mapped.plan.sampleCount > 1 &&
        (!description.initialData.empty() || !description.initialSubresources.empty())) {
        error = "multisample textures cannot use the existing CPU upload path";
        return false;
    }
    if (texture_format_is_compressed(mapped.plan.format) &&
        (!description.initialData.empty() || !description.initialSubresources.empty())) {
        error = "compressed textures cannot use the existing uncompressed CPU upload path";
        return false;
    }
    plan = mapped.plan;
    return true;
}
}

class NullBackend final : public IRenderBackend {
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::unordered_map<std::uint32_t, MaterialDesc> materials_;
    std::unordered_map<std::uint32_t, ResourceUsage> resourceUsages_;
    std::unordered_map<std::uint32_t, ResourceKind> resourceKinds_;
    std::vector<bool> submittedQueueBatches_;
    std::string lastError_;
public:
    explicit NullBackend(BackendApi api) {
        capabilities_.api = api;
        capabilities_.deviceReady = api == BackendApi::Null;
        capabilities_.deviceState = api == BackendApi::Null ? RenderDeviceState::Ready : RenderDeviceState::Uninitialized;
        capabilities_.supportsCompute = api != BackendApi::Null;
        capabilities_.supportsMultiDrawIndirect = api == BackendApi::DirectX12 || api == BackendApi::Vulkan;
        capabilities_.supportsBindless = false;
        capabilities_.supportsDedicatedComputeQueue = false;
        capabilities_.supportsDedicatedCopyQueue = false;
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    bool initialize(const RenderBackendConfig&) override { return true; }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "Null backend resource handle and description kind do not match";
            return false;
        }
        ++stats_.resourceCreates;
        resourceUsages_[handle.id] = ResourceUsage::Unknown;
        resourceKinds_[handle.id] = handle.kind;
        if (const auto* material = std::get_if<MaterialDesc>(&description)) materials_[handle.id] = *material;
        return true;
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resourceKinds_.find(physical.id);
        if (!logical || !physical || source == resourceKinds_.end() || source->second != physical.kind ||
            source->second != logical.kind) {
            lastError_ = "Null backend alias handles or resource kinds are invalid";
            return false;
        }
        resourceKinds_[logical.id] = logical.kind;
        resourceUsages_[logical.id] = resourceUsages_[physical.id];
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        const auto kind = resourceKinds_.find(handle.id);
        if (kind == resourceKinds_.end() || kind->second != handle.kind) return;
        ++stats_.resourceDestroys;
        materials_.erase(handle.id);
        resourceUsages_.erase(handle.id);
        resourceKinds_.erase(handle.id);
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        if (handle.kind != ResourceKind::Material) {
            lastError_ = "Null backend material handle has the wrong resource kind";
            return false;
        }
        materials_[handle.id] = description;
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        if (!update.buffer || update.buffer.kind != ResourceKind::Buffer || update.data.empty()) {
            lastError_ = "Null backend buffer update handle or data is invalid";
            return false;
        }
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        if (!update.texture || update.texture.kind != ResourceKind::Texture2D || update.data.empty() ||
            update.width == 0 || update.height == 0) {
            lastError_ = "Null backend texture update handle or data is invalid";
            return false;
        }
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        if (!texture || texture.kind != ResourceKind::Texture2D) {
            lastError_ = "Null backend mip generation handle has the wrong resource kind";
            return false;
        }
        return true;
    }
    void begin_frame() override { ++stats_.frames; submittedQueueBatches_.clear(); }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        for (const auto waitBatch : waitBatches) {
            if (waitBatch >= submittedQueueBatches_.size() || !submittedQueueBatches_[waitBatch]) {
                lastError_ = "Null backend queue batch wait references an unsignaled batch";
                return false;
            }
        }
        if (submittedQueueBatches_.size() <= batchIndex) submittedQueueBatches_.resize(batchIndex + 1, false);
        submittedQueueBatches_[batchIndex] = true;
        ++stats_.queueSubmissions;
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        const auto it = resourceUsages_.find(handle.id);
        const auto kind = resourceKinds_.find(handle.id);
        if (it == resourceUsages_.end() || kind == resourceKinds_.end() || kind->second != handle.kind) {
            lastError_ = "Null backend resource handle or kind is invalid";
            return false;
        }
        if (it->second == usage) return true;
        it->second = usage;
        ++stats_.barriers;
        return true;
    }
    void begin_debug_label(std::string_view) override { ++stats_.debugMarkers; }
    void set_debug_name(ResourceHandle, std::string_view) override {}
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void bind_pipeline(std::string_view) override {}
    void bind_pipeline(ResourceHandle handle) override {
        if (handle && handle.kind != ResourceKind::Pipeline) lastError_ = "Null backend pipeline handle has the wrong resource kind";
    }
    void bind_material(ResourceHandle handle) override {
        if (handle.kind != ResourceKind::Material) {
            lastError_ = "Null backend material handle has the wrong resource kind";
            return;
        }
        const auto material = materials_.find(handle.id);
        if (material == materials_.end()) {
            lastError_ = "Null backend material handle is invalid";
            return;
        }
        ++stats_.materialBinds;
        for (const auto& binding : material->second.bindings) {
            const auto count = binding.resources.empty() ? (binding.resource ? 1u : 0u)
                : static_cast<std::uint32_t>(binding.resources.size());
            if (count != 0) ++stats_.descriptorBinds;
        }
    }
    void draw_sprite(const SpriteDraw&) override { ++stats_.drawCalls; ++stats_.spriteCalls; }
    void draw_mesh(const MeshDraw&) override { ++stats_.drawCalls; ++stats_.meshCalls; }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        if (!draw.argumentBuffer || !draw.vertexBuffer || !draw.indexBuffer || draw.maxDrawCount == 0) return;
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        ++stats_.indirectDrawCalls;
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) return false;
        ++stats_.dispatchCalls;
        return true;
    }
    void end_frame() override {}
    void discard_frame() override { ++stats_.discardedFrames; }
    void wait_idle() override {}
    void clear_error() override { lastError_.clear(); }
    std::string last_error() const override { return lastError_; }
    RenderStats stats() const noexcept override { return stats_; }
};

#if defined(SHINKOU_PLATFORM_WINDOWS)
IDXGIAdapter1* select_high_performance_adapter() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return nullptr;
    const char* requestedAdapter = std::getenv("SHINKOU_GPU_ADAPTER");
    const bool preferIntegrated = requestedAdapter &&
        (_stricmp(requestedAdapter, "integrated") == 0 || _stricmp(requestedAdapter, "igpu") == 0);
    IDXGIAdapter1* best = nullptr;
    std::uint64_t bestScore = 0;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            continue;
        }
        // DXGI does not expose a single universal "integrated" bit. Intel
        // adapters and hardware with little/no dedicated VRAM are reliable
        // indicators, while shared system memory is the primary IGPU pool.
        const bool integrated = description.VendorId == 0x8086u ||
            description.DedicatedVideoMemory <= (1ull << 30);
        const auto score = preferIntegrated
            ? (integrated ? (1ull << 63) : 0ull) +
                static_cast<std::uint64_t>(description.SharedSystemMemory) / 16ull
            : static_cast<std::uint64_t>(description.DedicatedVideoMemory) +
                static_cast<std::uint64_t>(description.SharedSystemMemory) / 16ull;
        if (!best || score > bestScore) {
            if (best) best->Release();
            best = adapter;
            bestScore = score;
        } else {
            adapter->Release();
        }
    }
    factory->Release();
    return best;
}

class DirectX11Backend final : public IRenderBackend {
    static D3D11_CULL_MODE cull_mode(std::string_view mode) {
        if (mode == "front") return D3D11_CULL_FRONT;
        if (mode == "none") return D3D11_CULL_NONE;
        return D3D11_CULL_BACK;
    }
    static D3D11_FILL_MODE fill_mode(std::string_view mode) {
        return mode == "wireframe" ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    }
    static D3D11_PRIMITIVE_TOPOLOGY topology(std::string_view mode) {
        if (mode == "line") return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        if (mode == "point") return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        ID3D11Resource* resource{nullptr};
        ID3D11ShaderResourceView* srv{nullptr};
        ID3D11UnorderedAccessView* uav{nullptr};
        ID3D11RenderTargetView* rtv{nullptr};
        ID3D11DepthStencilView* dsv{nullptr};
        ID3D11SamplerState* sampler{nullptr};
        ID3D11VertexShader* vertexShader{nullptr};
        ID3D11PixelShader* pixelShader{nullptr};
        ID3D11ComputeShader* computeShader{nullptr};
        ID3D11InputLayout* inputLayout{nullptr};
        std::vector<std::uint8_t> shaderBytecode;
        ID3D11BlendState* blendState{nullptr};
        ID3D11DepthStencilState* depthState{nullptr};
        ID3D11RasterizerState* rasterState{nullptr};
        UINT stride{0};
        bool indexBuffer{false};
        std::size_t size{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t mipLevels{1};
        std::uint32_t layers{1};
        bool generateMips{false};
        ResourceUsage usage{ResourceUsage::Unknown};
        bool alias{false};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<ShaderBinding> shaderBindings;
        ResourceRecord() = default;
        ResourceRecord(const ResourceRecord&) = delete;
        ResourceRecord& operator=(const ResourceRecord&) = delete;
        ResourceRecord(ResourceRecord&& other) noexcept
            : kind(other.kind), resource(other.resource), srv(other.srv), uav(other.uav), rtv(other.rtv), dsv(other.dsv), sampler(other.sampler),
              vertexShader(other.vertexShader), pixelShader(other.pixelShader), computeShader(other.computeShader),
              inputLayout(other.inputLayout), shaderBytecode(std::move(other.shaderBytecode)), blendState(other.blendState), depthState(other.depthState), rasterState(other.rasterState), stride(other.stride), indexBuffer(other.indexBuffer), size(other.size),
              format(other.format), width(other.width), height(other.height), mipLevels(other.mipLevels), layers(other.layers), generateMips(other.generateMips), usage(other.usage),
              pipeline(std::move(other.pipeline)), material(std::move(other.material)), shaderBindings(std::move(other.shaderBindings)) {
            other.resource = nullptr; other.srv = nullptr; other.uav = nullptr; other.rtv = nullptr; other.dsv = nullptr;
            other.sampler = nullptr;
            other.vertexShader = nullptr; other.pixelShader = nullptr; other.computeShader = nullptr;
            other.inputLayout = nullptr; other.blendState = nullptr; other.depthState = nullptr; other.rasterState = nullptr;
        }
        ~ResourceRecord() {
            if (computeShader) computeShader->Release();
            if (sampler) sampler->Release();
            if (depthState) depthState->Release();
            if (rasterState) rasterState->Release();
            if (inputLayout) inputLayout->Release();
            if (blendState) blendState->Release();
            if (pixelShader) pixelShader->Release();
            if (vertexShader) vertexShader->Release();
            if (rtv) rtv->Release();
            if (dsv) dsv->Release();
            if (srv) srv->Release();
            if (uav) uav->Release();
            if (resource) resource->Release();
        }
    };
    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    IDXGISwapChain* swapChain_{nullptr};
    ID3D11RenderTargetView* renderTarget_{nullptr};
    ID3D11Texture2D* depthBuffer_{nullptr};
    ID3D11DepthStencilView* depthView_{nullptr};
    bool vsync_{true};
    std::uint32_t width_{1280};
    std::uint32_t height_{720};
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    std::unordered_map<ID3D11Resource*, ResourceUsage> resourceUsages_;
    std::vector<bool> submittedQueueBatches_;
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord* boundPipeline_{nullptr};
    ResourceRecord* boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::string lastError_;
    std::unordered_map<std::uint32_t, std::string> debugNames_;
public:
    ~DirectX11Backend() override {
        resources_.clear();
        if (context_) context_->Release();
        if (depthView_) depthView_->Release();
        if (depthBuffer_) depthBuffer_->Release();
        if (renderTarget_) renderTarget_->Release();
        if (swapChain_) swapChain_->Release();
        if (device_) device_->Release();
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    std::string last_error() const override { return lastError_; }
    void clear_error() override { lastError_.clear(); }
#if defined(SHINKOU_WITH_IMGUI) && defined(SHINKOU_PLATFORM_WINDOWS)
    bool initialize_imgui() override {
        return device_ && context_ && ImGui::GetCurrentContext() && ImGui_ImplDX11_Init(device_, context_);
    }
    void shutdown_imgui() override {
        if (ImGui::GetCurrentContext()) ImGui_ImplDX11_Shutdown();
    }
    void render_imgui(ImDrawData* drawData) override {
        if (!drawData || !context_ || !renderTarget_) return;
        ImGui_ImplDX11_NewFrame();
        context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
        ImGui_ImplDX11_RenderDrawData(drawData);
    }
#endif
    bool initialize(const RenderBackendConfig& config) override {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = E_FAIL;
        IDXGIAdapter1* adapter = select_high_performance_adapter();
        if (config.nativeWindow) {
            DXGI_SWAP_CHAIN_DESC swapDesc{};
            swapDesc.BufferCount = 2;
            swapDesc.BufferDesc.Width = config.width;
            swapDesc.BufferDesc.Height = config.height;
            swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDesc.OutputWindow = static_cast<HWND>(config.nativeWindow);
            swapDesc.SampleDesc.Count = 1;
            swapDesc.Windowed = TRUE;
            result = D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                0, levels, 2, D3D11_SDK_VERSION, &swapDesc, &swapChain_, &device_, &selected, &context_);
            if (FAILED(result)) {
                if (context_) { context_->Release(); context_ = nullptr; }
                if (device_) { device_->Release(); device_ = nullptr; }
                if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    0, levels, 1, D3D11_SDK_VERSION, &device_, &selected, &context_);
            }
        } else {
            result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                0, levels, 2, D3D11_SDK_VERSION, &device_, &selected, &context_);
        }
        if (adapter) adapter->Release();
        if (FAILED(result)) return false;
        vsync_ = config.vsync;
        width_ = config.width;
        height_ = config.height;
        if (swapChain_) {
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
            result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
            backBuffer->Release();
            if (FAILED(result)) return false;
            if (!create_depth_target(config.width, config.height)) return false;
        }
        capabilities_.api = BackendApi::DirectX11;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
        capabilities_.supportsCompute = true;
        capabilities_.supportsMultiDrawIndirect = true;
        capabilities_.supportsDedicatedComputeQueue = false;
        capabilities_.supportsDedicatedCopyQueue = false;
        return true;
    }
    bool resize(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) {
            lastError_ = "D3D11 swapchain dimensions must be non-zero";
            return false;
        }
        if (!swapChain_ || !device_ || !context_) return true;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
        if (depthView_) { depthView_->Release(); depthView_ = nullptr; }
        if (depthBuffer_) { depthBuffer_->Release(); depthBuffer_ = nullptr; }
        const auto resizeResult = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeResult)) {
            lastError_ = "D3D11 swapchain resize failed hr=" + std::to_string(static_cast<long long>(resizeResult));
            capabilities_.deviceState = resizeResult == DXGI_ERROR_DEVICE_REMOVED || resizeResult == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return false;
        }
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        const auto result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
        if (SUCCEEDED(result)) {
            width_ = width;
            height_ = height;
        }
        const auto resized = SUCCEEDED(result) && create_depth_target(width, height);
        if (resized) capabilities_.deviceState = RenderDeviceState::Ready;
        return resized;
    }
    bool create_depth_target(std::uint32_t width, std::uint32_t height) {
        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = width;
        depth.Height = height;
        depth.MipLevels = 1;
        depth.ArraySize = 1;
        depth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth.SampleDesc.Count = 1;
        depth.Usage = D3D11_USAGE_DEFAULT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device_->CreateTexture2D(&depth, nullptr, &depthBuffer_))) return false;
        return SUCCEEDED(device_->CreateDepthStencilView(depthBuffer_, nullptr, &depthView_));
    }
    static DXGI_FORMAT format_of(std::string_view format) {
        if (format == "bgra8") return DXGI_FORMAT_B8G8R8A8_UNORM;
        if (format == "rgba16f") return DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (format == "r32f") return DXGI_FORMAT_R32_FLOAT;
        if (format == "d24s8") return DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (format == "d32f") return DXGI_FORMAT_D32_FLOAT;
        if (format == "bc1") return DXGI_FORMAT_BC1_UNORM;
        if (format == "bc3") return DXGI_FORMAT_BC3_UNORM;
        if (format == "bc5") return DXGI_FORMAT_BC5_UNORM;
        if (format == "bc6h") return DXGI_FORMAT_BC6H_UF16;
        if (format == "bc7") return DXGI_FORMAT_BC7_UNORM;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    static std::size_t bytes_per_pixel(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "D3D11 resource handle and description kind do not match";
            return false;
        }
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        bool pipelineValidationFailed = false;
        std::visit([&](const auto& desc) {
            using T = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<T, TextureDesc>) {
                TexturePhysicalImagePlan plan;
                if (!prepare_backend_texture(desc, BackendApi::DirectX11, plan, lastError_)) return;
                D3D11_TEXTURE2D_DESC texture{};
                texture.Width = plan.width;
                texture.Height = plan.height;
                texture.MipLevels = plan.mipLevels;
                texture.ArraySize = plan.physicalArrayLayers;
                texture.Format = format_of(texture_format_name(plan.format));
                texture.SampleDesc.Count = plan.sampleCount;
                texture.Usage = D3D11_USAGE_DEFAULT;
                const bool depth = handle.kind == ResourceKind::DepthStencil || texture_format_is_depth(plan.format);
                const bool generateMips = desc.generateMips && plan.mipLevels > 1 && !depth;
                if (generateMips) texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                texture.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_SHADER_RESOURCE;
                if (desc.storage && !depth) texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                if (desc.renderTarget) texture.BindFlags |= D3D11_BIND_RENDER_TARGET;
                if (generateMips) texture.BindFlags |= D3D11_BIND_RENDER_TARGET;
                record.generateMips = generateMips;
                D3D11_SUBRESOURCE_DATA data{};
                data.pSysMem = desc.initialData.empty() || generateMips ? nullptr : desc.initialData.data();
                data.SysMemPitch = static_cast<UINT>(plan.width * bytes_per_pixel(texture.Format));
                ID3D11Texture2D* native = nullptr;
                if (SUCCEEDED(device_->CreateTexture2D(&texture, data.pSysMem ? &data : nullptr, &native))) {
                    record.resource = native;
                    device_->CreateShaderResourceView(native, nullptr, &record.srv);
                    if (desc.storage) device_->CreateUnorderedAccessView(native, nullptr, &record.uav);
                    if (desc.renderTarget) device_->CreateRenderTargetView(native, nullptr, &record.rtv);
                    if (depth) device_->CreateDepthStencilView(native, nullptr, &record.dsv);
                }
            } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                D3D11_SAMPLER_DESC sampler{};
                sampler.Filter = desc.filter == "nearest" ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.AddressU = desc.addressU == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
                sampler.AddressV = desc.addressV == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
                sampler.AddressW = desc.addressW == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
                sampler.MaxAnisotropy = static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy));
                sampler.ComparisonFunc = desc.compareEnable ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
                sampler.MinLOD = 0.0f;
                sampler.MaxLOD = D3D11_FLOAT32_MAX;
                device_->CreateSamplerState(&sampler, &record.sampler);
            } else if constexpr (std::is_same_v<T, BufferDesc>) {
                record.stride = static_cast<UINT>(desc.stride);
                record.indexBuffer = desc.indexBuffer;
                record.size = desc.size;
                D3D11_BUFFER_DESC buffer{};
                buffer.ByteWidth = static_cast<UINT>(std::max<std::size_t>(desc.size, 1));
                buffer.Usage = D3D11_USAGE_DEFAULT;
                if (desc.vertexBuffer) buffer.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
                if (desc.indexBuffer) buffer.BindFlags |= D3D11_BIND_INDEX_BUFFER;
                if (desc.indirectBuffer) buffer.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                if (desc.structuredBuffer && desc.stride != 0) {
                    buffer.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                    buffer.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                    buffer.StructureByteStride = static_cast<UINT>(desc.stride);
                }
                if (desc.storageBuffer) buffer.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                D3D11_SUBRESOURCE_DATA data{};
                data.pSysMem = desc.initialData.empty() ? nullptr : desc.initialData.data();
                ID3D11Buffer* native = nullptr;
                if (SUCCEEDED(device_->CreateBuffer(&buffer, data.pSysMem ? &data : nullptr, &native)) && native) {
                    record.resource = native;
                    if (desc.structuredBuffer && desc.stride != 0) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                        view.Format = DXGI_FORMAT_UNKNOWN;
                        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                        view.Buffer.NumElements = static_cast<UINT>(desc.size / desc.stride);
                        device_->CreateShaderResourceView(native, &view, &record.srv);
                    }
                    if (desc.storageBuffer) device_->CreateUnorderedAccessView(native, nullptr, &record.uav);
                }
            } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                const auto compiled = shaderCompiler_->compile(desc, BackendApi::DirectX11);
                if (!compiled.valid) { lastError_ = compiled.diagnostics; return; }
                if (!validate_backend_shader_layout(compiled.bindings, lastError_)) return;
                record.shaderBindings = compiled.bindings;
                record.shaderBytecode = compiled.bytecode;
                if (desc.stage == ShaderStage::Vertex) {
                    device_->CreateVertexShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &record.vertexShader);
                } else if (desc.stage == ShaderStage::Fragment) {
                    device_->CreatePixelShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &record.pixelShader);
                } else {
                    device_->CreateComputeShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &record.computeShader);
                }
            } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                if (desc.sampleCount != 1 || desc.colorFormats.size() > 8 ||
                    !is_supported_color_format(desc.colorFormat) || !is_supported_pipeline_depth_format(desc.depthFormat) ||
                    std::any_of(desc.colorFormats.begin(), desc.colorFormats.end(),
                        [](const std::string& format) { return !is_supported_color_format(format); }) ||
                    (desc.fillMode != "solid" && desc.fillMode != "wireframe") ||
                    (desc.cullMode != "back" && desc.cullMode != "front" && desc.cullMode != "none") ||
                    (desc.topology != "triangle" && desc.topology != "line" && desc.topology != "point")) {
                    lastError_ = "D3D11 pipeline fixed-function state is unsupported";
                    pipelineValidationFailed = true;
                    return;
                }
                record.pipeline = desc;
                D3D11_RASTERIZER_DESC raster{};
                raster.FillMode = fill_mode(desc.fillMode);
                raster.CullMode = cull_mode(desc.cullMode);
                raster.DepthClipEnable = TRUE;
                device_->CreateRasterizerState(&raster, &record.rasterState);
                D3D11_BLEND_DESC blend{};
                const auto blendCount = std::max<std::size_t>(1, desc.colorFormats.size());
                for (std::size_t index = 0; index < blendCount; ++index) {
                    auto& target = blend.RenderTarget[index];
                    target.BlendEnable = desc.alphaBlend ? TRUE : FALSE;
                    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                    target.BlendOp = D3D11_BLEND_OP_ADD;
                    target.SrcBlendAlpha = D3D11_BLEND_ONE;
                    target.DestBlendAlpha = D3D11_BLEND_ZERO;
                    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                }
                device_->CreateBlendState(&blend, &record.blendState);
                D3D11_DEPTH_STENCIL_DESC depth{};
                depth.DepthEnable = desc.depthFormat == "none" || !desc.depthTest ? FALSE : TRUE;
                depth.DepthWriteMask = desc.depthFormat == "none" || !desc.depthWrite
                    ? D3D11_DEPTH_WRITE_MASK_ZERO : D3D11_DEPTH_WRITE_MASK_ALL;
                depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
                device_->CreateDepthStencilState(&depth, &record.depthState);
                if (desc.vertexInput) {
                    const auto vertex = resources_.find(desc.vertexShader);
                    if (vertex != resources_.end() && vertex->second.shaderBytecode.size() > 0) {
                        const D3D11_INPUT_ELEMENT_DESC input[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                            D3D11_INPUT_PER_VERTEX_DATA, 0}};
                        device_->CreateInputLayout(input, 1, vertex->second.shaderBytecode.data(), vertex->second.shaderBytecode.size(), &record.inputLayout);
                    }
                }
            } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                record.material = desc;
            }
        }, description);
        if (pipelineValidationFailed) return false;
        resources_.emplace(handle.id, std::move(record));
        ++stats_.resourceCreates;
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture && !texture->initialData.empty()) {
            const auto textureFormat = texture->formatKind == TextureFormat::Unknown
                ? texture_format_from_name(texture->format) : texture->formatKind;
            const auto uploaded = update_texture({handle, 0, 0, texture->width, texture->height,
                static_cast<std::size_t>(texture->width) * bytes_per_pixel(format_of(texture_format_name(textureFormat))), texture->initialData});
            if (!uploaded) lastError_ = "D3D11 initial texture upload failed";
            else if (texture->generateMips && (texture->mipLevels == 0 || texture->mipLevels > 1) && !generate_mips(handle)) {
                lastError_ = "D3D11 initial texture mip generation failed";
            }
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture) {
            for (const auto& subresource : texture->initialSubresources) {
                if (!update_texture({handle, subresource.mipLevel, subresource.layer, subresource.width,
                    subresource.height, subresource.rowPitch, subresource.data})) {
                    lastError_ = "D3D11 initial texture subresource upload failed";
                    return false;
                }
            }
        }
        const auto created = resources_.find(handle.id);
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil || handle.kind == ResourceKind::Buffer) &&
             !created->second.resource)) {
            if (lastError_.empty()) lastError_ = "D3D11 resource creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Sampler && !created->second.sampler) {
            if (lastError_.empty()) lastError_ = "D3D11 sampler creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Shader && !created->second.vertexShader &&
            !created->second.pixelShader && !created->second.computeShader) {
            if (lastError_.empty()) lastError_ = "D3D11 shader creation failed";
            return false;
        }
        if (created->second.resource) resourceUsages_[created->second.resource] = ResourceUsage::Unknown;
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) return;
        if (boundPipeline_ == &it->second) boundPipeline_ = nullptr;
        if (!it->second.alias && it->second.resource) resourceUsages_.erase(it->second.resource);
        resources_.erase(it);
        ++stats_.resourceDestroys;
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() || source->second.kind != physical.kind ||
            source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D && source->second.kind != ResourceKind::Buffer)) return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.resource = source->second.resource;
        alias.srv = source->second.srv;
        alias.uav = source->second.uav;
        alias.rtv = source->second.rtv;
        alias.dsv = source->second.dsv;
        alias.stride = source->second.stride;
        alias.indexBuffer = source->second.indexBuffer;
        alias.size = source->second.size;
        alias.format = source->second.format;
        alias.width = source->second.width;
        alias.height = source->second.height;
        alias.mipLevels = source->second.mipLevels;
        alias.layers = source->second.layers;
        alias.generateMips = source->second.generateMips;
        alias.usage = source->second.usage;
        alias.alias = true;
        if (alias.resource) alias.resource->AddRef();
        if (alias.srv) alias.srv->AddRef();
        if (alias.uav) alias.uav->AddRef();
        if (alias.rtv) alias.rtv->AddRef();
        if (alias.dsv) alias.dsv->AddRef();
        resources_.emplace(logical.id, std::move(alias));
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() || it->second.kind != ResourceKind::Buffer) {
            lastError_ = "D3D11 buffer handle or resource kind is invalid";
            return false;
        }
        if (!it->second.resource) { lastError_ = "D3D12 buffer has no native resource"; return false; }
        if (update.data.empty() || update.offset + update.data.size() > it->second.size) { lastError_ = "D3D12 buffer update exceeds resource size"; return false; }
        D3D11_BOX box{};
        box.left = static_cast<UINT>(update.offset);
        box.right = static_cast<UINT>(update.offset + update.data.size());
        box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        context_->UpdateSubresource(it->second.resource, 0, &box, update.data.data(), static_cast<UINT>(update.data.size()), 0);
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            !it->second.generateMips || !it->second.srv || !context_) {
            lastError_ = "D3D11 mip generation handle or resource is invalid";
            return false;
        }
        context_->GenerateMips(it->second.srv);
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            !it->second.resource || update.data.empty() || update.width == 0 || update.height == 0) {
            lastError_ = "D3D11 texture handle or update data is invalid";
            return false;
        }
        auto* texture = static_cast<ID3D11Texture2D*>(it->second.resource);
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (update.mipLevel >= description.MipLevels || update.layer >= description.ArraySize ||
            update.x > std::max(1u, description.Width >> update.mipLevel) ||
            update.y > std::max(1u, description.Height >> update.mipLevel) ||
            update.width > std::max(1u, description.Width >> update.mipLevel) - update.x ||
            update.height > std::max(1u, description.Height >> update.mipLevel) - update.y) return false;
        D3D11_BOX box{update.x, update.y, 0, update.x + update.width, update.y + update.height, 1};
        const auto pixelSize = bytes_per_pixel(description.Format);
        const auto pitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * pixelSize : update.rowPitch;
        if (pitch < static_cast<std::size_t>(update.width) * pixelSize || pitch * update.height > update.data.size()) return false;
        context_->UpdateSubresource(it->second.resource, D3D11CalcSubresource(update.mipLevel, update.layer, description.MipLevels),
            &box, update.data.data(), static_cast<UINT>(pitch), 0);
        return true;
    }
    bool resolve_texture(ResourceHandle destination, ResourceHandle source) override {
        const auto destinationIt = resources_.find(destination.id);
        const auto sourceIt = resources_.find(source.id);
        if (!context_ || destination.kind != ResourceKind::Texture2D || source.kind != ResourceKind::Texture2D ||
            destinationIt == resources_.end() || sourceIt == resources_.end() ||
            !destinationIt->second.resource || !sourceIt->second.resource || destination.id == source.id) {
            lastError_ = "D3D11 resolve requires two valid texture resources";
            return false;
        }
        auto* destinationTexture = static_cast<ID3D11Texture2D*>(destinationIt->second.resource);
        auto* sourceTexture = static_cast<ID3D11Texture2D*>(sourceIt->second.resource);
        D3D11_TEXTURE2D_DESC destinationDescription{};
        D3D11_TEXTURE2D_DESC sourceDescription{};
        destinationTexture->GetDesc(&destinationDescription);
        sourceTexture->GetDesc(&sourceDescription);
        if (sourceDescription.SampleDesc.Count <= 1 || destinationDescription.SampleDesc.Count != 1 ||
            sourceDescription.Width != destinationDescription.Width ||
            sourceDescription.Height != destinationDescription.Height ||
            sourceDescription.ArraySize != destinationDescription.ArraySize ||
            sourceDescription.MipLevels != destinationDescription.MipLevels ||
            sourceDescription.Format != destinationDescription.Format ||
            sourceDescription.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
            sourceDescription.Format == DXGI_FORMAT_D32_FLOAT) {
            lastError_ = "D3D11 resolve resources must match and use a color MSAA source";
            return false;
        }
        for (UINT layer = 0; layer < sourceDescription.ArraySize; ++layer) {
            for (UINT mip = 0; mip < sourceDescription.MipLevels; ++mip) {
                const auto subresource = D3D11CalcSubresource(mip, layer, sourceDescription.MipLevels);
                context_->ResolveSubresource(destinationIt->second.resource, subresource,
                                              sourceIt->second.resource, subresource, sourceDescription.Format);
            }
        }
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind || !context_ || !it->second.resource) {
            lastError_ = "D3D11 resource transition handle or device is invalid";
            return false;
        }
        const auto state = resourceUsages_.find(it->second.resource);
        const auto previous = state == resourceUsages_.end() ? ResourceUsage::Unknown : state->second;
        if (previous == usage) return true;
        const bool write = is_color_attachment_usage(usage) || usage == ResourceUsage::DepthStencil ||
            usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite ||
            usage == ResourceUsage::CopyDestination;
        ID3D11ShaderResourceView* nullSrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D11UnorderedAccessView* nullUavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        if (write) {
            context_->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSrvs);
            context_->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSrvs);
            context_->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSrvs);
            context_->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, nullUavs, nullptr);
        } else {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, nullUavs, nullptr);
        }
        resourceUsages_[it->second.resource] = usage;
        ++stats_.barriers;
        return true;
    }
    void set_render_target(ResourceHandle target) override {
        if (!context_) return;
        if (target) {
            const auto it = resources_.find(target.id);
            if (it != resources_.end() && it->second.rtv) {
                context_->OMSetRenderTargets(1, &it->second.rtv, nullptr);
                return;
            }
        }
        if (renderTarget_) context_->OMSetRenderTargets(1, &renderTarget_, depthView_);
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        if (!context_) return;
        ID3D11RenderTargetView* rtv = color ? renderTarget_ : nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (color && colorIt != resources_.end() && colorIt->second.rtv) rtv = colorIt->second.rtv;
        if (depth && depthIt != resources_.end() && depthIt->second.dsv) dsv = depthIt->second.dsv;
        context_->OMSetRenderTargets(rtv ? 1u : 0u, rtv ? &rtv : nullptr, dsv);
    }
    void set_render_targets(const std::vector<ResourceHandle>& colors, ResourceHandle depth, bool clearAttachments) override {
        if (!context_) return;
        if (colors.size() <= 1) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth);
            if (clearAttachments) {
                constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (!colors.empty()) {
                    const auto color = resources_.find(colors.front().id);
                    if (color != resources_.end() && color->second.rtv) context_->ClearRenderTargetView(color->second.rtv, clear);
                }
                if (depth) {
                    const auto depthResource = resources_.find(depth.id);
                    if (depthResource != resources_.end() && depthResource->second.dsv) {
                        context_->ClearDepthStencilView(depthResource->second.dsv,
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                    }
                }
            }
            return;
        }
        std::array<ID3D11RenderTargetView*, 8> rtvs{};
        for (std::size_t index = 0; index < colors.size() && index < rtvs.size(); ++index) {
            const auto it = resources_.find(colors[index].id);
            rtvs[index] = it != resources_.end() ? it->second.rtv : nullptr;
        }
        ID3D11DepthStencilView* dsv = nullptr;
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (it != resources_.end() && it->second.dsv) dsv = it->second.dsv;
        }
        context_->OMSetRenderTargets(static_cast<UINT>(std::min<std::size_t>(colors.size(), rtvs.size())),
            rtvs.data(), dsv);
        if (clearAttachments) {
            constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (const auto rtv : rtvs) if (rtv) context_->ClearRenderTargetView(rtv, clear);
            if (depth && dsv) context_->ClearDepthStencilView(dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
    }
    void set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth) override {
        if (!context_) return;
        D3D11_VIEWPORT viewport{x, y, width, height, minDepth, maxDepth};
        context_->RSSetViewports(1, &viewport);
    }
    void begin_frame() override {
        ++stats_.frames;
        submittedQueueBatches_.clear();
        if (device_ && !swapChain_) {
            const auto reason = device_->GetDeviceRemovedReason();
            if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET) {
                lastError_ = "D3D11 device lost before frame begin hr=" + std::to_string(static_cast<long long>(reason));
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
            }
        }
        if (context_ && renderTarget_) {
            D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
            context_->RSSetViewports(1, &viewport);
            constexpr float clear[4] = {0.035f, 0.045f, 0.065f, 1.0f};
            context_->OMSetRenderTargets(1, &renderTarget_, depthView_);
            context_->ClearRenderTargetView(renderTarget_, clear);
            if (depthView_) context_->ClearDepthStencilView(depthView_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
    }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        for (const auto waitBatch : waitBatches) {
            if (waitBatch >= submittedQueueBatches_.size() || !submittedQueueBatches_[waitBatch]) {
                lastError_ = "D3D11 queue batch wait references an unsignaled batch";
                return false;
            }
        }
        if (submittedQueueBatches_.size() <= batchIndex) submittedQueueBatches_.resize(batchIndex + 1, false);
        submittedQueueBatches_[batchIndex] = true;
        ++stats_.queueSubmissions;
        return context_ != nullptr;
    }
    void set_debug_name(ResourceHandle handle, std::string_view name) override {
        debugNames_[handle.id] = std::string(name);
    }
    void begin_debug_label(std::string_view) override { ++stats_.debugMarkers; }
    void end_debug_label() override {}
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void bind_pipeline(std::string_view name) override {
        for (auto& [id, resource] : resources_) {
            if (resource.kind == ResourceKind::Pipeline && resource.pipeline.name == name) {
                bind_pipeline(ResourceHandle{id, ResourceKind::Pipeline});
                return;
            }
        }
        boundPipeline_ = nullptr;
        lastError_ = "D3D11 pipeline name is invalid: " + std::string(name);
    }
    void bind_pipeline(ResourceHandle handle) override {
        const auto pipeline = resources_.find(handle.id);
        if (pipeline == resources_.end() || pipeline->second.kind != ResourceKind::Pipeline ||
            handle.kind != ResourceKind::Pipeline) {
            boundPipeline_ = nullptr;
            lastError_ = "D3D11 pipeline handle is invalid";
            return;
        }
        boundPipeline_ = &pipeline->second;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11ComputeShader* cs = nullptr;
        if (const auto shader = resources_.find(boundPipeline_->pipeline.vertexShader); shader != resources_.end()) vs = shader->second.vertexShader;
            if (const auto shader = resources_.find(boundPipeline_->pipeline.fragmentShader); shader != resources_.end()) ps = shader->second.pixelShader;
        if (const auto shader = resources_.find(boundPipeline_->pipeline.computeShader); shader != resources_.end()) cs = shader->second.computeShader;
        context_->VSSetShader(vs, nullptr, 0);
        context_->PSSetShader(ps, nullptr, 0);
        context_->CSSetShader(cs, nullptr, 0);
        constexpr float blendFactor[4] = {1, 1, 1, 1};
        context_->OMSetBlendState(boundPipeline_->blendState, blendFactor, 0xffffffffu);
        context_->OMSetDepthStencilState(boundPipeline_->depthState, 0);
        context_->RSSetState(boundPipeline_->rasterState);
        context_->IASetInputLayout(boundPipeline_->pipeline.vertexInput ? boundPipeline_->inputLayout : nullptr);
        context_->IASetPrimitiveTopology(topology(boundPipeline_->pipeline.topology));
        if (boundPipeline_->pipeline.vertexInput) {
            const auto vertex = resources_.find(boundPipeline_->pipeline.vertexShader);
            (void)vertex;
        }
    }
    void bind_material(ResourceHandle handle) override {
        const auto material = resources_.find(handle.id);
        if (material == resources_.end() || material->second.kind != ResourceKind::Material ||
            handle.kind != ResourceKind::Material) {
            lastError_ = "D3D11 material handle is invalid";
            return;
        }
        ++stats_.materialBinds;
        bind_pipeline(material->second.material.pipeline);
        const auto pipeline = resources_.find(material->second.material.pipeline.id);
        if (pipeline == resources_.end()) {
            lastError_ = "D3D11 material pipeline handle is invalid";
            return;
        }
        for (const auto& binding : material->second.material.bindings) {
            if (binding.space != 0) {
                lastError_ = "D3D11 does not support non-zero shader register spaces; binding '" + binding.name + "' requested space=" + std::to_string(binding.space);
                return;
            }
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId : {pipeline->second.pipeline.vertexShader, pipeline->second.pipeline.fragmentShader, pipeline->second.pipeline.computeShader}) {
                const auto shader = resources_.find(shaderId);
                if (shader == resources_.end() || shader->second.shaderBindings.empty()) continue;
                reflectionAvailable = true;
                found |= std::any_of(shader->second.shaderBindings.begin(), shader->second.shaderBindings.end(),
                    [&](const auto& reflected) { return shader_binding_matches(reflected, binding); });
            }
            if (reflectionAvailable && !found) {
                lastError_ = "D3D11 material '" + material->second.material.name + "' binding '" + binding.name +
                    "' does not match shader slot=" + std::to_string(binding.slot) +
                    " space=" + std::to_string(binding.space) + " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        for (const auto& binding : material->second.material.bindings) {
            const std::vector<ResourceHandle> handles = binding.resources.empty()
                ? std::vector<ResourceHandle>{binding.resource}
                : binding.resources;
            if (handles.empty() || (handles.size() == 1 && !handles.front())) continue;
            if (binding.type == DescriptorType::Texture) {
                std::vector<ID3D11ShaderResourceView*> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.srv) {
                        lastError_ = "D3D11 texture array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.srv);
                }
                context_->PSSetShaderResources(binding.slot, static_cast<UINT>(views.size()), views.data());
                context_->CSSetShaderResources(binding.slot, static_cast<UINT>(views.size()), views.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StorageTexture) {
                std::vector<ID3D11UnorderedAccessView*> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.uav) {
                        lastError_ = "D3D11 storage texture array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.uav);
                }
                context_->CSSetUnorderedAccessViews(binding.slot, static_cast<UINT>(views.size()), views.data(), nullptr);
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StorageBuffer) {
                std::vector<ID3D11UnorderedAccessView*> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.uav) {
                        lastError_ = "D3D11 storage buffer array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.uav);
                }
                context_->CSSetUnorderedAccessViews(binding.slot, static_cast<UINT>(views.size()), views.data(), nullptr);
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StructuredBuffer) {
                std::vector<ID3D11ShaderResourceView*> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.srv) {
                        lastError_ = "D3D11 structured buffer array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.srv);
                }
                context_->VSSetShaderResources(binding.slot, static_cast<UINT>(views.size()), views.data());
                context_->PSSetShaderResources(binding.slot, static_cast<UINT>(views.size()), views.data());
                context_->CSSetShaderResources(binding.slot, static_cast<UINT>(views.size()), views.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::UniformBuffer) {
                std::vector<ID3D11Buffer*> buffers;
                buffers.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    auto* buffer = resource == resources_.end() ? nullptr : static_cast<ID3D11Buffer*>(resource->second.resource);
                    if (!buffer) {
                        lastError_ = "D3D11 uniform buffer array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    buffers.push_back(buffer);
                }
                context_->VSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()), buffers.data());
                context_->PSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()), buffers.data());
                context_->CSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()), buffers.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::Sampler) {
                std::vector<ID3D11SamplerState*> samplers;
                samplers.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.sampler) {
                        lastError_ = "D3D11 sampler array binding '" + binding.name + "' contains an invalid resource";
                        return;
                    }
                    samplers.push_back(resource->second.sampler);
                }
                context_->PSSetSamplers(binding.slot, static_cast<UINT>(samplers.size()), samplers.data());
                context_->CSSetSamplers(binding.slot, static_cast<UINT>(samplers.size()), samplers.data());
                ++stats_.descriptorBinds;
            }
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() || material->second.kind != ResourceKind::Material) {
            lastError_ = "D3D11 material handle not found";
            return false;
        }
        material->second.material = description;
        if (boundPipeline_ == &material->second) boundPipeline_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot, std::uint32_t space) override {
        if (space != 0) {
            lastError_ = "D3D11 does not support non-zero shader register spaces";
            return;
        }
        if (!context_ || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
            lastError_ = "D3D11 uniform buffer binding has invalid command state or slot";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() || resource->second.kind != ResourceKind::Buffer ||
            !resource->second.resource) {
            lastError_ = "D3D11 uniform buffer handle is invalid";
            return;
        }
        auto* buffer = static_cast<ID3D11Buffer*>(resource->second.resource);
        context_->VSSetConstantBuffers(slot, 1, &buffer);
        context_->PSSetConstantBuffers(slot, 1, &buffer);
        context_->CSSetConstantBuffers(slot, 1, &buffer);
    }
    void draw_sprite(const SpriteDraw& draw) override {
        ++stats_.drawCalls; ++stats_.spriteCalls;
        if (!context_ || !boundPipeline_) {
            lastError_ = "D3D11 sprite draw has invalid command state";
            return;
        }
        if (draw.texture) {
            const auto texture = resources_.find(draw.texture.id);
            if (draw.texture.kind != ResourceKind::Texture2D || texture == resources_.end() || !texture->second.srv) {
                lastError_ = "D3D11 sprite draw texture is invalid";
                return;
            }
            auto* srv = texture->second.srv;
            context_->PSSetShaderResources(0, 1, &srv);
        }
        context_->Draw(6, 0);
    }
    void draw_mesh(const MeshDraw& draw) override {
        ++stats_.drawCalls; ++stats_.meshCalls;
        if (!context_ || !boundPipeline_) {
            lastError_ = "D3D11 mesh draw has invalid command state";
            return;
        }
        auto vertex = resources_.find(draw.vertexBuffer.id);
        auto index = resources_.find(draw.indexBuffer.id);
        if (vertex != resources_.end() && index != resources_.end() && vertex->second.resource && index->second.resource) {
            auto* vb = static_cast<ID3D11Buffer*>(vertex->second.resource);
            auto* ib = static_cast<ID3D11Buffer*>(index->second.resource);
            UINT offset = 0;
            if (vb) context_->IASetVertexBuffers(0, 1, &vb, &vertex->second.stride, &offset);
            if (ib) context_->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
            context_->DrawIndexed(draw.indexCount, 0, 0);
            return;
        }
        lastError_ = "D3D11 mesh draw buffers are invalid";
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (!context_ || dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) {
            lastError_ = "D3D11 dispatch has invalid command state or zero group count";
            return false;
        }
        if (!boundPipeline_ || !boundPipeline_->computeShader) {
            lastError_ = "D3D11 dispatch requires a compute shader pipeline";
            return false;
        }
        ++stats_.dispatchCalls;
        context_->Dispatch(dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
        return true;
    }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        if (!context_ || !boundPipeline_ || !draw.argumentBuffer || !draw.vertexBuffer || !draw.indexBuffer ||
            draw.maxDrawCount == 0 || draw.stride < sizeof(std::uint32_t) * 5) {
            lastError_ = "D3D11 indirect draw has invalid command state";
            return;
        }
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (vertex == resources_.end() || index == resources_.end() || arguments == resources_.end() ||
            !vertex->second.resource || !index->second.resource || !arguments->second.resource) {
            lastError_ = "D3D11 indirect draw references an invalid buffer";
            return;
        }
        auto* vb = static_cast<ID3D11Buffer*>(vertex->second.resource);
        auto* ib = static_cast<ID3D11Buffer*>(index->second.resource);
        auto* args = static_cast<ID3D11Buffer*>(arguments->second.resource);
        UINT offset = 0;
        context_->IASetVertexBuffers(0, 1, &vb, &vertex->second.stride, &offset);
        context_->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        for (std::uint32_t index = 0; index < draw.maxDrawCount; ++index) {
            context_->DrawIndexedInstancedIndirect(args, static_cast<UINT>(draw.argumentOffset + index * draw.stride));
            ++stats_.drawCalls;
            ++stats_.meshCalls;
            ++stats_.indirectDrawCalls;
        }
    }
    void end_frame() override {
        if (!swapChain_) return;
        const auto result = swapChain_->Present(vsync_ ? 1 : 0, 0);
        if (FAILED(result)) {
            lastError_ = "D3D11 present failed hr=" + std::to_string(static_cast<long long>(result));
            capabilities_.deviceState = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
        }
    }
    void discard_frame() override {
        ++stats_.discardedFrames;
        // D3D11 records directly into the immediate context. Unbind the
        // render targets so a failed graph cannot accidentally present them.
        if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void wait_idle() override { if (context_) context_->Flush(); }
    RenderStats stats() const noexcept override { return stats_; }
};

class DirectX12Backend final : public IRenderBackend {
    static D3D12_CULL_MODE cull_mode(std::string_view mode) {
        if (mode == "front") return D3D12_CULL_MODE_FRONT;
        if (mode == "none") return D3D12_CULL_MODE_NONE;
        return D3D12_CULL_MODE_BACK;
    }
    static D3D12_FILL_MODE fill_mode(std::string_view mode) {
        return mode == "wireframe" ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    }
    static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(std::string_view mode) {
        if (mode == "line") return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        if (mode == "point") return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    static D3D_PRIMITIVE_TOPOLOGY topology(std::string_view mode) {
        if (mode == "line") return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        if (mode == "point") return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    static std::filesystem::path pipeline_cache_path(ID3D12Device* device) {
        LUID luid{};
        if (device) luid = device->GetAdapterLuid();
        const auto key = std::to_string(static_cast<unsigned long long>(luid.HighPart)) + "_" +
            std::to_string(static_cast<unsigned long long>(luid.LowPart));
        // Bump this when the native pipeline/root-signature construction
        // changes. The adapter LUID is part of the filename, so invalidation
        // remains scoped to the exact physical adapter.
        constexpr std::string_view cacheFormat = "v3";
        if (const auto* directory = std::getenv("SHINKOU_PIPELINE_CACHE_DIR")) {
            return std::filesystem::path(directory) / (key + "_" + std::string(cacheFormat) + ".dxc");
        }
        return std::filesystem::temp_directory_path() / "shinkou_pipeline_cache" / (key + "_" + std::string(cacheFormat) + ".dxc");
    }
    static std::wstring pipeline_library_name(const PipelineDesc& description,
                                              const std::vector<std::uint8_t>& vertexBytecode = {},
                                              const std::vector<std::uint8_t>& fragmentBytecode = {},
                                              const std::vector<std::uint8_t>& computeBytecode = {},
                                              std::wstring_view suffix = {}) {
        std::string key = std::to_string(description.vertexShader) + ":" + std::to_string(description.fragmentShader) + ":" +
            std::to_string(description.computeShader) + ":" + (description.depthTest ? "1" : "0") + (description.depthWrite ? "1" : "0") +
            (description.alphaBlend ? "1" : "0") + (description.vertexInput ? "1" : "0") + ":" + description.cullMode + ":" +
            description.fillMode + ":" + description.topology + ":" + description.colorFormat + ":" + description.depthFormat + ":" +
            std::to_string(description.sampleCount);
        for (const auto& format : description.colorFormats) { key += ":" + format; }
        const auto appendBytecode = [&key](const auto& bytecode) {
            for (const auto byte : bytecode) key += std::to_string(byte) + ",";
        };
        appendBytecode(vertexBytecode);
        appendBytecode(fragmentBytecode);
        appendBytecode(computeBytecode);
        const auto hash = std::hash<std::string>{}(key);
        const auto name = std::wstring(L"shinkou_pso_") + std::to_wstring(static_cast<unsigned long long>(hash));
        return std::wstring(name) + std::wstring(suffix);
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        ID3D12Resource* resource{nullptr};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<std::uint8_t> shaderBytecode;
        std::vector<ShaderBinding> shaderBindings;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::size_t size{0};
        UINT stride{0};
        bool indexBuffer{false};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t mipLevels{1};
        std::uint32_t layers{1};
        std::vector<std::uint8_t> baseTextureData;
        GpuAllocation gpuAllocation{};
        D3D12_RESOURCE_DESC nativeDescription{};
        D3D12_CLEAR_VALUE clearValue{};
        bool hasClearValue{false};
        bool placedResource{false};
        ID3D12PipelineState* pipelineState{nullptr};
        ID3D12PipelineState* depthOnlyPipeline{nullptr};
        ID3D12RootSignature* rootSignature{nullptr};
        std::unordered_map<std::uint64_t, UINT> rootParameters;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE cbvGpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE cbvCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE samplerGpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu{};
        UINT srvIndex{0};
        UINT cbvIndex{0};
        UINT uavIndex{0};
        UINT samplerIndex{0};
        UINT rtvIndex{0};
        UINT dsvIndex{0};
        bool srvValid{false};
        bool cbvValid{false};
        bool uavValid{false};
        bool rtvValid{false};
        bool dsvValid{false};
        bool alias{false};
        bool samplerValid{false};
        struct MaterialDescriptorRange {
            UINT base{0};
            UINT count{0};
            bool samplerHeap{false};
        };
        std::unordered_map<std::uint64_t, MaterialDescriptorRange> materialRanges;
        bool materialRangesValid{false};
        ResourceRecord() = default;
        ResourceRecord(const ResourceRecord&) = delete;
        ResourceRecord& operator=(const ResourceRecord&) = delete;
        ResourceRecord(ResourceRecord&& other) noexcept
            : kind(other.kind), resource(other.resource), pipeline(std::move(other.pipeline)), material(std::move(other.material)), shaderBytecode(std::move(other.shaderBytecode)), shaderBindings(std::move(other.shaderBindings)), state(other.state), size(other.size), stride(other.stride), indexBuffer(other.indexBuffer),
              format(other.format), width(other.width), height(other.height), mipLevels(other.mipLevels), layers(other.layers), baseTextureData(std::move(other.baseTextureData)),
              gpuAllocation(other.gpuAllocation), nativeDescription(other.nativeDescription), clearValue(other.clearValue),
              hasClearValue(other.hasClearValue), placedResource(other.placedResource),
              pipelineState(other.pipelineState), depthOnlyPipeline(other.depthOnlyPipeline), rootSignature(other.rootSignature), rootParameters(std::move(other.rootParameters)),
              srvGpu(other.srvGpu), srvCpu(other.srvCpu), uavGpu(other.uavGpu), uavCpu(other.uavCpu), cbvGpu(other.cbvGpu), cbvCpu(other.cbvCpu),
              rtvCpu(other.rtvCpu), dsvCpu(other.dsvCpu), samplerGpu(other.samplerGpu), samplerCpu(other.samplerCpu), srvIndex(other.srvIndex),
              cbvIndex(other.cbvIndex), uavIndex(other.uavIndex), samplerIndex(other.samplerIndex), rtvIndex(other.rtvIndex), dsvIndex(other.dsvIndex),
              srvValid(other.srvValid), cbvValid(other.cbvValid), uavValid(other.uavValid), rtvValid(other.rtvValid), dsvValid(other.dsvValid), alias(other.alias), samplerValid(other.samplerValid),
              materialRanges(std::move(other.materialRanges)), materialRangesValid(other.materialRangesValid) {
            other.resource = nullptr;
            other.gpuAllocation = {};
            other.hasClearValue = false;
            other.placedResource = false;
            other.pipelineState = nullptr;
            other.depthOnlyPipeline = nullptr;
            other.rootSignature = nullptr;
            other.srvValid = false;
            other.cbvValid = false;
            other.uavValid = false;
            other.rtvValid = false;
            other.dsvValid = false;
            other.samplerValid = false;
            other.materialRangesValid = false;
        }
        ResourceRecord& operator=(ResourceRecord&& other) noexcept {
            if (this == &other) return *this;
            if (pipelineState) pipelineState->Release();
            if (depthOnlyPipeline) depthOnlyPipeline->Release();
            if (rootSignature) rootSignature->Release();
            if (resource) resource->Release();
            kind = other.kind;
            resource = other.resource;
            pipeline = std::move(other.pipeline);
            material = std::move(other.material);
            shaderBytecode = std::move(other.shaderBytecode);
            shaderBindings = std::move(other.shaderBindings);
            state = other.state;
            size = other.size;
            stride = other.stride;
            indexBuffer = other.indexBuffer;
            format = other.format;
            width = other.width;
            height = other.height;
            mipLevels = other.mipLevels;
            layers = other.layers;
            baseTextureData = std::move(other.baseTextureData);
            gpuAllocation = other.gpuAllocation;
            nativeDescription = other.nativeDescription;
            clearValue = other.clearValue;
            hasClearValue = other.hasClearValue;
            placedResource = other.placedResource;
            pipelineState = other.pipelineState;
            depthOnlyPipeline = other.depthOnlyPipeline;
            rootSignature = other.rootSignature;
            rootParameters = std::move(other.rootParameters);
            srvGpu = other.srvGpu;
            srvCpu = other.srvCpu;
            uavGpu = other.uavGpu;
            uavCpu = other.uavCpu;
            cbvGpu = other.cbvGpu;
            cbvCpu = other.cbvCpu;
            rtvCpu = other.rtvCpu;
            dsvCpu = other.dsvCpu;
            samplerGpu = other.samplerGpu;
            samplerCpu = other.samplerCpu;
            srvIndex = other.srvIndex;
            cbvIndex = other.cbvIndex;
            uavIndex = other.uavIndex;
            samplerIndex = other.samplerIndex;
            rtvIndex = other.rtvIndex;
            dsvIndex = other.dsvIndex;
            srvValid = other.srvValid;
            cbvValid = other.cbvValid;
            uavValid = other.uavValid;
            rtvValid = other.rtvValid;
            dsvValid = other.dsvValid;
            samplerValid = other.samplerValid;
            alias = other.alias;
            materialRanges = std::move(other.materialRanges);
            materialRangesValid = other.materialRangesValid;
            other.resource = nullptr;
            other.gpuAllocation = {};
            other.hasClearValue = false;
            other.placedResource = false;
            other.pipelineState = nullptr;
            other.depthOnlyPipeline = nullptr;
            other.rootSignature = nullptr;
            other.srvValid = false;
            other.cbvValid = false;
            other.uavValid = false;
            other.rtvValid = false;
            other.dsvValid = false;
            other.samplerValid = false;
            other.materialRangesValid = false;
            return *this;
        }
        ~ResourceRecord() {
            if (pipelineState) pipelineState->Release();
            if (depthOnlyPipeline) depthOnlyPipeline->Release();
            if (rootSignature) rootSignature->Release();
            if (resource) resource->Release();
        }
    };
    ID3D12Device* device_{nullptr};
    ID3D12CommandQueue* queue_{nullptr};
    IDXGISwapChain3* swapChain_{nullptr};
    ID3D12CommandAllocator* commandAllocator_{nullptr};
    ID3D12GraphicsCommandList* commandList_{nullptr};
    ID3D12CommandSignature* indexedDrawSignature_{nullptr};
    ID3D12DescriptorHeap* rtvHeap_{nullptr};
    ID3D12DescriptorHeap* srvHeap_{nullptr};
    ID3D12DescriptorHeap* cpuSrvHeap_{nullptr};
    ID3D12DescriptorHeap* samplerHeap_{nullptr};
    ID3D12DescriptorHeap* dsvHeap_{nullptr};
    ID3D12Resource* depthBuffer_{nullptr};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle_{};
    std::vector<ID3D12Resource*> backBuffers_;
    ID3D12Fence* fence_{nullptr};
    HANDLE fenceEvent_{nullptr};
    std::uint64_t fenceValue_{0};
    ID3D12QueryHeap* timestampQueryHeap_{nullptr};
    ID3D12Resource* timestampReadback_{nullptr};
    UINT64 timestampFrequency_{0};
    UINT timestampCursor_{0};
    UINT submittedTimestampCount_{0};
    UINT rtvStride_{0};
    UINT srvStride_{0};
    UINT cpuSrvStride_{0};
    UINT samplerStride_{0};
    UINT dsvStride_{0};
    UINT srvCursor_{0};
    UINT samplerCursor_{0};
    UINT rtvCursor_{2};
    UINT dsvCursor_{1};
    std::vector<UINT> freeSrvSlots_;
    std::vector<UINT> freeSamplerSlots_;
    std::vector<UINT> freeRtvSlots_;
    std::vector<UINT> freeDsvSlots_;
    std::uint32_t currentRenderTarget_{0};
    struct BindlessTableRecord { std::uint32_t baseIndex{0}; std::uint32_t capacity{0}; };
    std::unordered_map<std::uint32_t, BindlessTableRecord> bindlessTables_;
    std::vector<std::pair<std::uint64_t, BindlessTableRecord>> retiredBindlessTables_;
    std::uint32_t nextBindlessTable_{1};
    std::uint32_t boundBindlessTable_{0};
    std::uint32_t frameIndex_{0};
    bool frameActive_{false};
    std::uint32_t width_{1280};
    std::uint32_t height_{720};
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    GpuMemoryAllocator gpuMemoryAllocator_{};
    std::unordered_map<std::uint64_t, ID3D12Heap*> gpuHeaps_;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resourceStates_;
    // D3D12 currently uses one direct queue/list as the portable fallback.
    // Keep batch dependency metadata explicit even though execution is
    // serialized on that queue.
    std::vector<bool> recordedQueueBatches_;
    std::vector<std::pair<std::uint64_t, ResourceRecord>> retiredResources_;
    std::vector<std::pair<std::uint64_t, std::unordered_map<std::uint64_t, ResourceRecord::MaterialDescriptorRange>>> retiredMaterialRanges_;
    struct PendingUpload {
        std::uint64_t fence{0};
        ID3D12CommandAllocator* allocator{nullptr};
        ID3D12GraphicsCommandList* list{nullptr};
        ID3D12Resource* staging{nullptr};
    };
    std::vector<PendingUpload> pendingUploads_;
    bool uploadBatchActive_{false};
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord* boundPipeline_{nullptr};
    ResourceRecord* boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::string lastError_;
    std::unordered_map<std::uint32_t, std::string> debugNames_;
    ID3D12PipelineLibrary1* pipelineLibrary_{nullptr};
    std::filesystem::path pipelineCachePath_{};
    void read_timestamp_results() {
        if (!timestampReadback_ || submittedTimestampCount_ < 2 || timestampFrequency_ == 0) return;
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(submittedTimestampCount_) * sizeof(UINT64)};
        if (FAILED(timestampReadback_->Map(0, &range, &mapped)) || !mapped) return;
        const auto* values = static_cast<const UINT64*>(mapped);
        stats_.gpuPassNanoseconds.clear();
        stats_.gpuPassNanoseconds.reserve(submittedTimestampCount_ / 2);
        for (UINT i = 0; i + 1 < submittedTimestampCount_; i += 2) {
            const UINT64 ticks = values[i + 1] >= values[i] ? values[i + 1] - values[i] : 0;
            stats_.gpuPassNanoseconds.push_back(static_cast<std::uint64_t>(
                (static_cast<long double>(ticks) * 1000000000.0L) /
                static_cast<long double>(timestampFrequency_)));
        }
        D3D12_RANGE written{0, 0};
        timestampReadback_->Unmap(0, &written);
    }
    void release_completed_uploads() {
        const auto completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
        pendingUploads_.erase(std::remove_if(pendingUploads_.begin(), pendingUploads_.end(),
            [completed](PendingUpload& upload) {
                if (upload.fence != 0 && upload.fence > completed) return false;
                if (upload.list) upload.list->Release();
                if (upload.allocator) upload.allocator->Release();
                if (upload.staging) upload.staging->Release();
                return true;
            }), pendingUploads_.end());
    }
    void save_pipeline_library() {
        if (!pipelineLibrary_) return;
        const auto size = pipelineLibrary_->GetSerializedSize();
        if (size == 0) return;
        std::vector<std::uint8_t> data(size);
        if (FAILED(pipelineLibrary_->Serialize(data.data(), data.size()))) return;
        std::error_code error;
        std::filesystem::create_directories(pipelineCachePath_.parent_path(), error);
        if (error) return;
        const auto temporary = pipelineCachePath_.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        stream.close();
        if (stream) {
            std::filesystem::remove(pipelineCachePath_, error);
            error.clear();
            std::filesystem::rename(temporary, pipelineCachePath_, error);
        }
        if (error) std::filesystem::remove(temporary, error);
    }
    void invalidate_pipeline_cache() {
        if (pipelineLibrary_) {
            pipelineLibrary_->Release();
            pipelineLibrary_ = nullptr;
        }
        if (pipelineCachePath_.empty()) return;
        std::error_code error;
        std::filesystem::remove(pipelineCachePath_, error);
    }
    void append_d3d12_diagnostics(std::string_view operation, HRESULT result) {
        lastError_ = "D3D12 " + std::string(operation) + " failed hr=" +
            std::to_string(static_cast<long long>(result));
        if (device_) {
            const auto reason = device_->GetDeviceRemovedReason();
            lastError_ += " deviceReason=" + std::to_string(static_cast<long long>(reason));
            if (result == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
                invalidate_pipeline_cache();
                lastError_ += " pipelineCacheInvalidated=1";
            }
        }
        ID3D12InfoQueue* infoQueue = nullptr;
        if (!device_ || FAILED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue))) || !infoQueue) return;
        const auto messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < messageCount; ++index) {
            SIZE_T messageSize = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) || messageSize == 0) continue;
            std::vector<std::uint8_t> messageStorage(messageSize);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageStorage.data());
            if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)) && message->pDescription) {
                lastError_ += " [" + std::string(message->pDescription) + "]";
            }
        }
        infoQueue->ClearStoredMessages();
        infoQueue->Release();
    }
public:
    ~DirectX12Backend() override {
        if (queue_ && fence_ && fenceEvent_ && fenceValue_ > 0) {
            queue_->Signal(fence_, fenceValue_);
            if (fence_->GetCompletedValue() < fenceValue_) {
                fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }
        for (auto& resource : resources_) release_gpu_allocation(resource.second);
        for (auto& retired : retiredResources_) release_gpu_allocation(retired.second);
        resources_.clear();
        retiredResources_.clear();
        save_pipeline_library();
        if (pipelineLibrary_) pipelineLibrary_->Release();
        for (auto& upload : pendingUploads_) {
            if (upload.list) upload.list->Release();
            if (upload.allocator) upload.allocator->Release();
            if (upload.staging) upload.staging->Release();
        }
        pendingUploads_.clear();
        for (auto* buffer : backBuffers_) if (buffer) buffer->Release();
        if (depthBuffer_) depthBuffer_->Release();
        if (fenceEvent_) CloseHandle(fenceEvent_);
        if (fence_) fence_->Release();
        if (timestampReadback_) timestampReadback_->Release();
        if (timestampQueryHeap_) timestampQueryHeap_->Release();
        if (rtvHeap_) rtvHeap_->Release();
        if (srvHeap_) srvHeap_->Release();
        if (cpuSrvHeap_) cpuSrvHeap_->Release();
        if (samplerHeap_) samplerHeap_->Release();
        if (dsvHeap_) dsvHeap_->Release();
        if (commandList_) commandList_->Release();
        if (indexedDrawSignature_) indexedDrawSignature_->Release();
        if (commandAllocator_) commandAllocator_->Release();
        if (swapChain_) swapChain_->Release();
        if (queue_) queue_->Release();
        for (auto& heap : gpuHeaps_) if (heap.second) heap.second->Release();
        gpuHeaps_.clear();
        if (device_) device_->Release();
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    std::string last_error() const override { return lastError_; }
    void clear_error() override { lastError_.clear(); }
    bool initialize(const RenderBackendConfig& config) override {
        if (std::getenv("SHINKOU_D3D12_DEBUG") != nullptr) {
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
                debug->EnableDebugLayer();
                debug->Release();
            }
        }
        IDXGIAdapter1* adapter = select_high_performance_adapter();
        HRESULT deviceResult = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (adapter) adapter->Release();
        if (FAILED(deviceResult)) return false;
        pipelineCachePath_ = pipeline_cache_path(device_);
                {
            std::vector<std::uint8_t> cacheData;
            std::ifstream stream(pipelineCachePath_, std::ios::binary | std::ios::ate);
            if (stream) {
                const auto size = stream.tellg();
                if (size > 0 && size <= static_cast<std::streamoff>(1ull << 30)) {
                    cacheData.resize(static_cast<std::size_t>(size));
                    stream.seekg(0);
                    if (!stream.read(reinterpret_cast<char*>(cacheData.data()), size)) cacheData.clear();
                }
            }
            ID3D12Device1* device1 = nullptr;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&device1)))) {
                if (FAILED(device1->CreatePipelineLibrary(cacheData.empty() ? nullptr : cacheData.data(), cacheData.size(),
                    IID_PPV_ARGS(&pipelineLibrary_))) && !cacheData.empty()) {
                    std::error_code error;
                    std::filesystem::remove(pipelineCachePath_, error);
                    device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipelineLibrary_));
                }
                device1->Release();
            }
        }
        width_ = config.width;
        height_ = config.height;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_)))) return false;
        queue_->GetTimestampFrequency(&timestampFrequency_);
        D3D12_QUERY_HEAP_DESC timestampDesc{};
        timestampDesc.Count = 512;
        timestampDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        if (FAILED(device_->CreateQueryHeap(&timestampDesc, IID_PPV_ARGS(&timestampQueryHeap_)))) {
            timestampQueryHeap_ = nullptr;
            timestampFrequency_ = 0;
        }
        if (timestampQueryHeap_) {
            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width = 512ull * sizeof(UINT64);
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES readbackHeap{};
            readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
            if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&timestampReadback_)))) {
                timestampQueryHeap_->Release();
                timestampQueryHeap_ = nullptr;
                timestampFrequency_ = 0;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = 1024;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap_)))) return false;
        srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&cpuSrvHeap_)))) return false;
        cpuSrvStride_ = srvStride_;
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
        samplerHeapDesc.NumDescriptors = 256;
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap_)))) return false;
        samplerStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        if (config.nativeWindow) {
            IDXGIFactory4* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
                DXGI_SWAP_CHAIN_DESC1 swapDesc{};
                swapDesc.Width = config.width;
                swapDesc.Height = config.height;
                swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                swapDesc.SampleDesc.Count = 1;
                swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapDesc.BufferCount = 2;
                swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                IDXGISwapChain1* baseSwapChain = nullptr;
                const HRESULT result = factory->CreateSwapChainForHwnd(queue_, static_cast<HWND>(config.nativeWindow),
                    &swapDesc, nullptr, nullptr, &baseSwapChain);
                if (SUCCEEDED(result)) baseSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain_));
                if (baseSwapChain) baseSwapChain->Release();
                factory->Release();
            }
        }
        if (swapChain_) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.NumDescriptors = 64;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
            rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            backBuffers_.resize(2);
            auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            for (std::uint32_t i = 0; i < backBuffers_.size(); ++i) {
                if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) return false;
                auto rtv = handle;
                rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
                device_->CreateRenderTargetView(backBuffers_[i], nullptr, rtv);
            }
            if (!create_depth_target(config.width, config.height)) return false;
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_)))) return false;
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_, nullptr, IID_PPV_ARGS(&commandList_)))) return false;
            commandList_->Close();
            D3D12_COMMAND_SIGNATURE_DESC signature{};
            signature.ByteStride = sizeof(std::uint32_t) * 5;
            signature.NumArgumentDescs = 1;
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            signature.pArgumentDescs = &argument;
            if (FAILED(device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indexedDrawSignature_)))) return false;
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
            fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_) return false;
        } else {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.NumDescriptors = 64;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
            rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_)))) return false;
            dsvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_)))) return false;
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_, nullptr, IID_PPV_ARGS(&commandList_)))) return false;
            if (FAILED(commandList_->Close())) return false;
            D3D12_COMMAND_SIGNATURE_DESC signature{};
            signature.ByteStride = sizeof(std::uint32_t) * 5;
            signature.NumArgumentDescs = 1;
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            signature.pArgumentDescs = &argument;
            if (FAILED(device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indexedDrawSignature_)))) return false;
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
            fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_) return false;
        }
        capabilities_.api = BackendApi::DirectX12;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
        capabilities_.supportsCompute = true;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool optionsAvailable = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
            &options, sizeof(options)));
        capabilities_.supportsBindless = optionsAvailable && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
        capabilities_.supportsMultiDrawIndirect = true;
        capabilities_.supportsDedicatedComputeQueue = false;
        capabilities_.supportsDedicatedCopyQueue = false;
        capabilities_.supportsGpuMemoryAllocator = true;
        capabilities_.supportsGpuMemoryAliasing = true;
        return true;
    }
    bool resize(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) {
            lastError_ = "D3D12 swapchain dimensions must be non-zero";
            return false;
        }
        if (!swapChain_ || !device_) return true;
        wait_idle();
        for (auto* buffer : backBuffers_) if (buffer) buffer->Release();
        backBuffers_.clear();
        if (depthBuffer_) { depthBuffer_->Release(); depthBuffer_ = nullptr; }
        const auto resizeResult = swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(resizeResult)) {
            lastError_ = "D3D12 swapchain resize failed hr=" + std::to_string(static_cast<long long>(resizeResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState = reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return false;
        }
        backBuffers_.resize(2);
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < backBuffers_.size(); ++i) {
            if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) return false;
            auto rtv = handle;
            rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
            device_->CreateRenderTargetView(backBuffers_[i], nullptr, rtv);
        }
        width_ = width;
        height_ = height;
        const auto resized = create_depth_target(width, height);
        if (resized) capabilities_.deviceState = RenderDeviceState::Ready;
        return resized;
    }
    bool create_depth_target(std::uint32_t width, std::uint32_t height) {
        if (!dsvHeap_) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.NumDescriptors = 64;
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsvHeap_)))) return false;
            dsvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        }
        D3D12_RESOURCE_DESC depth{};
        depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth.Width = width;
        depth.Height = height;
        depth.DepthOrArraySize = 1;
        depth.MipLevels = 1;
        depth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth.SampleDesc.Count = 1;
        depth.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = depth.Format;
        clear.DepthStencil = {1.0f, 0};
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depthBuffer_)))) return false;
        dsvHandle_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateDepthStencilView(depthBuffer_, nullptr, dsvHandle_);
        dsvCursor_ = 1;
        return true;
    }
    static DXGI_FORMAT format_of(std::string_view format) {
        if (format == "bgra8") return DXGI_FORMAT_B8G8R8A8_UNORM;
        if (format == "rgba16f") return DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (format == "r32f") return DXGI_FORMAT_R32_FLOAT;
        if (format == "d24s8") return DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (format == "d32f") return DXGI_FORMAT_D32_FLOAT;
        if (format == "bc1") return DXGI_FORMAT_BC1_UNORM;
        if (format == "bc3") return DXGI_FORMAT_BC3_UNORM;
        if (format == "bc5") return DXGI_FORMAT_BC5_UNORM;
        if (format == "bc6h") return DXGI_FORMAT_BC6H_UF16;
        if (format == "bc7") return DXGI_FORMAT_BC7_UNORM;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    bool take_srv_slot(UINT& slot) {
        if (!freeSrvSlots_.empty()) { slot = freeSrvSlots_.back(); freeSrvSlots_.pop_back(); return true; }
        if (srvCursor_ >= 1024) return false;
        slot = srvCursor_++;
        return true;
    }
    bool take_srv_range(UINT count, UINT& base) {
        if (count == 0) return false;
        std::sort(freeSrvSlots_.begin(), freeSrvSlots_.end());
        for (std::size_t index = 0; index + count <= freeSrvSlots_.size(); ++index) {
            bool contiguous = true;
            for (UINT offset = 1; offset < count; ++offset) {
                if (freeSrvSlots_[index + offset] != freeSrvSlots_[index] + offset) {
                    contiguous = false;
                    break;
                }
            }
            if (!contiguous) continue;
            base = freeSrvSlots_[index];
            freeSrvSlots_.erase(freeSrvSlots_.begin() + static_cast<std::ptrdiff_t>(index),
                freeSrvSlots_.begin() + static_cast<std::ptrdiff_t>(index + count));
            return true;
        }
        if (count > 1024u - srvCursor_) return false;
        base = srvCursor_;
        srvCursor_ += count;
        return true;
    }
    void release_srv_slot(UINT slot) { if (slot < 1024) freeSrvSlots_.push_back(slot); }
    bool take_sampler_slot(UINT& slot) {
        if (!freeSamplerSlots_.empty()) { slot = freeSamplerSlots_.back(); freeSamplerSlots_.pop_back(); return true; }
        if (samplerCursor_ >= 256) return false;
        slot = samplerCursor_++;
        return true;
    }
    bool take_sampler_range(UINT count, UINT& base) {
        if (count == 0) return false;
        std::sort(freeSamplerSlots_.begin(), freeSamplerSlots_.end());
        for (std::size_t index = 0; index + count <= freeSamplerSlots_.size(); ++index) {
            bool contiguous = true;
            for (UINT offset = 1; offset < count; ++offset) {
                if (freeSamplerSlots_[index + offset] != freeSamplerSlots_[index] + offset) {
                    contiguous = false;
                    break;
                }
            }
            if (!contiguous) continue;
            base = freeSamplerSlots_[index];
            freeSamplerSlots_.erase(freeSamplerSlots_.begin() + static_cast<std::ptrdiff_t>(index),
                freeSamplerSlots_.begin() + static_cast<std::ptrdiff_t>(index + count));
            return true;
        }
        if (count > 256u - samplerCursor_) return false;
        base = samplerCursor_;
        samplerCursor_ += count;
        return true;
    }
    void release_sampler_slot(UINT slot) { if (slot < 256) freeSamplerSlots_.push_back(slot); }
    void release_sampler_range(const ResourceRecord::MaterialDescriptorRange& range) {
        for (UINT index = 0; index < range.count; ++index) {
            if (range.samplerHeap) release_sampler_slot(range.base + index);
            else release_srv_slot(range.base + index);
        }
    }
    void release_material_ranges(ResourceRecord& record) {
        for (const auto& [key, range] : record.materialRanges) {
            (void)key;
            release_sampler_range(range);
        }
        record.materialRanges.clear();
        record.materialRangesValid = false;
    }
    void defer_material_ranges(ResourceRecord& record) {
        if (!record.materialRanges.empty()) {
            retiredMaterialRanges_.push_back({fenceValue_, std::move(record.materialRanges)});
        }
        record.materialRanges.clear();
        record.materialRangesValid = false;
    }
    static D3D12_DESCRIPTOR_RANGE_TYPE descriptor_range_type(DescriptorType type) {
        if (type == DescriptorType::Sampler) return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        if (type == DescriptorType::UniformBuffer) return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        if (type == DescriptorType::StorageBuffer || type == DescriptorType::StorageTexture) return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
    bool build_material_descriptor_ranges(ResourceRecord& material, ResourceRecord& pipeline) {
        release_material_ranges(material);
        for (const auto& binding : material.material.bindings) {
            const auto handles = binding.resources.empty()
                ? std::vector<ResourceHandle>{binding.resource}
                : binding.resources;
            if (handles.empty() || (handles.size() == 1 && !handles.front())) {
                lastError_ = "D3D12 material binding '" + binding.name + "' has no resources";
                release_material_ranges(material);
                return false;
            }
            const auto rangeType = descriptor_range_type(binding.type);
            const auto key = d3d12_binding_key(binding.space, binding.slot, rangeType);
            if (pipeline.rootParameters.find(key) == pipeline.rootParameters.end()) {
                lastError_ = "D3D12 material binding '" + binding.name + "' has no root descriptor range";
                release_material_ranges(material);
                return false;
            }
            ResourceRecord::MaterialDescriptorRange range{};
            range.count = static_cast<UINT>(handles.size());
            range.samplerHeap = rangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            const bool allocated = range.samplerHeap ? take_sampler_range(range.count, range.base) : take_srv_range(range.count, range.base);
            if (!allocated) {
                lastError_ = "D3D12 descriptor heap cannot allocate a contiguous material range for '" + binding.name + "'";
                release_material_ranges(material);
                return false;
            }
            material.materialRanges[key] = range;
            for (UINT index = 0; index < range.count; ++index) {
                const auto resource = resources_.find(handles[index].id);
                if (resource == resources_.end()) {
                    lastError_ = "D3D12 material binding '" + binding.name + "' contains an invalid resource";
                    release_material_ranges(material);
                    return false;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE source{};
                if (binding.type == DescriptorType::Sampler) {
                    if (!resource->second.samplerValid) {
                        lastError_ = "D3D12 sampler binding '" + binding.name + "' contains an invalid sampler";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.samplerCpu;
                } else if (binding.type == DescriptorType::UniformBuffer) {
                    if (!resource->second.cbvValid) {
                        lastError_ = "D3D12 uniform buffer binding '" + binding.name + "' contains an invalid buffer";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.cbvCpu;
                } else if (binding.type == DescriptorType::StorageBuffer || binding.type == DescriptorType::StorageTexture) {
                    if (!resource->second.uavValid) {
                        lastError_ = "D3D12 storage binding '" + binding.name + "' contains an invalid resource";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.uavCpu;
                } else {
                    if (!resource->second.srvValid) {
                        lastError_ = "D3D12 shader resource binding '" + binding.name + "' contains an invalid resource";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.srvCpu;
                }
                if (!source.ptr) {
                    release_material_ranges(material);
                    return false;
                }
                auto destination = (range.samplerHeap ? samplerHeap_->GetCPUDescriptorHandleForHeapStart() : srvHeap_->GetCPUDescriptorHandleForHeapStart());
                destination.ptr += static_cast<SIZE_T>(range.base + index) * (range.samplerHeap ? samplerStride_ : srvStride_);
                device_->CopyDescriptorsSimple(1, destination, source,
                    range.samplerHeap ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        material.materialRangesValid = true;
        return true;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE material_range_gpu(const ResourceRecord::MaterialDescriptorRange& range) const {
        auto gpu = (range.samplerHeap ? samplerHeap_->GetGPUDescriptorHandleForHeapStart() : srvHeap_->GetGPUDescriptorHandleForHeapStart());
        gpu.ptr += static_cast<UINT64>(range.base) * (range.samplerHeap ? samplerStride_ : srvStride_);
        return gpu;
    }
    bool take_rtv_slot(UINT& slot) {
        if (!freeRtvSlots_.empty()) { slot = freeRtvSlots_.back(); freeRtvSlots_.pop_back(); return true; }
        if (rtvCursor_ >= 64) return false;
        slot = rtvCursor_++;
        return true;
    }
    void release_rtv_slot(UINT slot) { if (slot >= 2 && slot < 64) freeRtvSlots_.push_back(slot); }
    bool take_dsv_slot(UINT& slot) {
        if (!freeDsvSlots_.empty()) { slot = freeDsvSlots_.back(); freeDsvSlots_.pop_back(); return true; }
        if (dsvCursor_ >= 64) return false;
        slot = dsvCursor_++;
        return true;
    }
    void release_dsv_slot(UINT slot) { if (slot >= 1 && slot < 64) freeDsvSlots_.push_back(slot); }
    bool allocate_srv(ResourceRecord& record, const D3D12_SHADER_RESOURCE_VIEW_DESC& description) {
        if (!srvHeap_ || !cpuSrvHeap_) return false;
        UINT slot = 0;
        if (!take_srv_slot(slot)) return false;
        auto sourceCpu = cpuSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        sourceCpu.ptr += static_cast<SIZE_T>(slot) * cpuSrvStride_;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot) * srvStride_;
        device_->CreateShaderResourceView(record.resource, &description, sourceCpu);
        auto destinationCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destinationCpu.ptr += static_cast<SIZE_T>(slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.srvCpu = sourceCpu;
        record.srvIndex = slot;
        record.srvGpu = gpu;
        record.srvValid = true;
        return true;
    }
    bool allocate_cbv(ResourceRecord& record, std::size_t sizeBytes) {
        if (!srvHeap_ || !cpuSrvHeap_ || !record.resource) return false;
        UINT slot = 0;
        if (!take_srv_slot(slot)) return false;
        const auto alignedSize = (static_cast<UINT>(std::max<std::size_t>(sizeBytes, 1)) + 255u) & ~255u;
        auto sourceCpu = cpuSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        sourceCpu.ptr += static_cast<SIZE_T>(slot) * cpuSrvStride_;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot) * srvStride_;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = record.resource->GetGPUVirtualAddress();
        cbv.SizeInBytes = alignedSize;
        device_->CreateConstantBufferView(&cbv, sourceCpu);
        auto destinationCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destinationCpu.ptr += static_cast<SIZE_T>(slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.cbvCpu = sourceCpu;
        record.cbvGpu = gpu;
        record.cbvIndex = slot;
        record.cbvValid = true;
        return true;
    }
    bool allocate_uav(ResourceRecord& record) {
        if (!srvHeap_ || !cpuSrvHeap_ || !record.resource) return false;
        UINT slot = 0;
        if (!take_srv_slot(slot)) return false;
        auto sourceCpu = cpuSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        sourceCpu.ptr += static_cast<SIZE_T>(slot) * cpuSrvStride_;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot) * srvStride_;
        device_->CreateUnorderedAccessView(record.resource, nullptr, nullptr, sourceCpu);
        auto destinationCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destinationCpu.ptr += static_cast<SIZE_T>(slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.uavCpu = sourceCpu;
        record.uavGpu = gpu;
        record.uavIndex = slot;
        record.uavValid = true;
        return true;
    }
    ID3D12Heap* ensure_gpu_heap(const GpuAllocation& allocation) {
        if (!allocation) return nullptr;
        const auto existing = gpuHeaps_.find(allocation.blockId);
        if (existing != gpuHeaps_.end()) return existing->second;
        const auto capacity = gpuMemoryAllocator_.block_capacity(allocation.blockId);
        if (capacity == 0) return nullptr;
        D3D12_HEAP_DESC description{};
        description.SizeInBytes = capacity;
        description.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        description.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        description.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        description.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap* heap = nullptr;
        const auto result = device_->CreateHeap(&description, IID_PPV_ARGS(&heap));
        if (FAILED(result) || !heap) {
            lastError_ = "D3D12 native GPU heap creation failed hr=" +
                std::to_string(static_cast<long long>(result));
            if (heap) heap->Release();
            return nullptr;
        }
        gpuHeaps_.emplace(allocation.blockId, heap);
        return heap;
    }
    bool try_create_placed_resource(ResourceRecord& record, const D3D12_RESOURCE_DESC& native,
                                    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
        if (!device_) return false;
        const auto allocationInfo = device_->GetResourceAllocationInfo(0, 1, &native);
        if (allocationInfo.SizeInBytes == 0 || allocationInfo.SizeInBytes == UINT64_MAX) return false;
        const auto allocation = gpuMemoryAllocator_.allocate(
            static_cast<std::size_t>(allocationInfo.SizeInBytes),
            static_cast<std::size_t>(allocationInfo.Alignment), 0, GpuMemoryLifetime::Persistent);
        if (!allocation) return false;
        auto* heap = ensure_gpu_heap(allocation);
        if (!heap) {
            gpuMemoryAllocator_.release(allocation);
            return false;
        }
        ID3D12Resource* resource = nullptr;
        const auto result = device_->CreatePlacedResource(heap, allocation.offset, &native, state, clear,
            IID_PPV_ARGS(&resource));
        if (FAILED(result) || !resource) {
            gpuMemoryAllocator_.release(allocation);
            if (resource) resource->Release();
            lastError_ = "D3D12 placed resource creation failed; falling back to committed resource hr=" +
                std::to_string(static_cast<long long>(result));
            return false;
        }
        record.resource = resource;
        record.gpuAllocation = allocation;
        record.nativeDescription = native;
        record.hasClearValue = clear != nullptr;
        if (clear) record.clearValue = *clear;
        record.placedResource = true;
        return true;
    }
    void release_gpu_allocation(ResourceRecord& record) {
        if (!record.alias && record.gpuAllocation) gpuMemoryAllocator_.release(record.gpuAllocation);
        record.gpuAllocation = {};
        record.placedResource = false;
    }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "D3D12 resource handle and description kind do not match";
            return false;
        }
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        std::visit([&](const auto& desc) {
            using T = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<T, TextureDesc>) {
                TexturePhysicalImagePlan plan;
                if (!prepare_backend_texture(desc, BackendApi::DirectX12, plan, lastError_)) return;
                D3D12_RESOURCE_DESC native{};
                native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                native.Width = plan.width;
                native.Height = plan.height;
                native.DepthOrArraySize = static_cast<UINT16>(plan.physicalArrayLayers);
                native.MipLevels = static_cast<UINT16>(plan.mipLevels);
                native.Format = format_of(texture_format_name(plan.format));
                record.format = native.Format;
                record.width = plan.width;
                record.height = plan.height;
                record.mipLevels = plan.mipLevels;
                record.layers = plan.physicalArrayLayers;
                native.SampleDesc.Count = plan.sampleCount;
                native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                const bool depth = handle.kind == ResourceKind::DepthStencil || texture_format_is_depth(plan.format);
                native.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL :
                    ((desc.renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE) |
                     (desc.storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE));
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_CLEAR_VALUE clear{};
                clear.Format = native.Format;
                if (depth) clear.DepthStencil = {1.0f, 0};
                else clear.Color[3] = 1.0f;
                record.nativeDescription = native;
                record.hasClearValue = desc.renderTarget || depth;
                if (record.hasClearValue) record.clearValue = clear;
                const auto* clearValue = record.hasClearValue ? &clear : nullptr;
                if (!try_create_placed_resource(record, native, D3D12_RESOURCE_STATE_COMMON, clearValue)) {
                    const auto placedError = lastError_;
                    lastError_.clear();
                    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                        D3D12_RESOURCE_STATE_COMMON, clearValue, IID_PPV_ARGS(&record.resource));
                    if (!record.resource && !placedError.empty()) {
                        lastError_ = placedError + "; committed-resource fallback also failed";
                    }
                }
                if (record.resource && !depth) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = native.Format;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (native.SampleDesc.Count > 1) {
                        srv.ViewDimension = plan.physicalArrayLayers > 1
                            ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
                        if (plan.physicalArrayLayers > 1) srv.Texture2DMSArray.ArraySize = plan.physicalArrayLayers;
                    } else if (plan.physicalArrayLayers > 1) {
                        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        srv.Texture2DArray.MipLevels = native.MipLevels;
                        srv.Texture2DArray.ArraySize = plan.physicalArrayLayers;
                    } else {
                        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srv.Texture2D.MipLevels = native.MipLevels;
                    }
                    allocate_srv(record, srv);
                    if (desc.storage) allocate_uav(record);
                    if (desc.renderTarget && rtvHeap_) {
                        UINT slot = 0;
                        if (!take_rtv_slot(slot)) return;
                        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
                        rtv.ptr += static_cast<SIZE_T>(slot) * rtvStride_;
                        device_->CreateRenderTargetView(record.resource, nullptr, rtv);
                        record.rtvCpu = rtv;
                        record.rtvIndex = slot;
                        record.rtvValid = true;
                    }
                } else if (record.resource && depth && dsvHeap_) {
                    UINT slot = 0;
                    if (!take_dsv_slot(slot)) return;
                    auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
                    dsv.ptr += static_cast<SIZE_T>(slot) * dsvStride_;
                    device_->CreateDepthStencilView(record.resource, nullptr, dsv);
                    record.dsvCpu = dsv;
                    record.dsvIndex = slot;
                    record.dsvValid = true;
                }
            } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                if (!samplerHeap_) return;
                UINT slot = 0;
                if (!take_sampler_slot(slot)) return;
                D3D12_SAMPLER_DESC sampler{};
                sampler.Filter = desc.filter == "nearest" ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.AddressU = desc.addressU == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampler.AddressV = desc.addressV == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampler.AddressW = desc.addressW == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampler.MaxAnisotropy = static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy));
                sampler.ComparisonFunc = desc.compareEnable ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
                auto cpu = samplerHeap_->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(slot) * samplerStride_;
                auto gpu = samplerHeap_->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += static_cast<UINT64>(slot) * samplerStride_;
                device_->CreateSampler(&sampler, cpu);
                record.samplerCpu = cpu;
                record.samplerGpu = gpu;
                record.samplerIndex = slot;
                record.samplerValid = true;
            } else if constexpr (std::is_same_v<T, BufferDesc>) {
                record.size = desc.size;
                record.stride = static_cast<UINT>(desc.stride);
                record.indexBuffer = desc.indexBuffer;
                D3D12_RESOURCE_DESC native{};
                native.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                native.Width = std::max<std::uint64_t>(desc.size, 1);
                native.Height = 1;
                native.DepthOrArraySize = 1;
                native.MipLevels = 1;
                native.SampleDesc.Count = 1;
                native.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (desc.storageBuffer) native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = (desc.initialData.empty() || desc.storageBuffer) ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;
                if (desc.indirectBuffer) record.state = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                else if (!desc.initialData.empty()) record.state = D3D12_RESOURCE_STATE_GENERIC_READ;
                record.nativeDescription = native;
                // Upload heaps have stricter placement rules and are kept on
                // the committed path. Default GPU buffers use the pooled
                // placed-resource path and fall back safely if unavailable.
                if (heap.Type == D3D12_HEAP_TYPE_DEFAULT) {
                    if (!try_create_placed_resource(record, native, record.state, nullptr)) {
                        const auto placedError = lastError_;
                        lastError_.clear();
                        device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                            record.state, nullptr, IID_PPV_ARGS(&record.resource));
                        if (!record.resource && !placedError.empty()) {
                            lastError_ = placedError + "; committed-resource fallback also failed";
                        }
                    }
                } else {
                    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                        record.state, nullptr, IID_PPV_ARGS(&record.resource));
                }
                if (record.resource && !desc.initialData.empty()) {
                    void* mapped = nullptr;
                    D3D12_RANGE range{0, 0};
                    if (SUCCEEDED(record.resource->Map(0, &range, &mapped))) {
                        std::memcpy(mapped, desc.initialData.data(), std::min<std::size_t>(desc.initialData.size(), desc.size));
                        record.resource->Unmap(0, nullptr);
                    }
                }
                if (record.resource && desc.structuredBuffer && desc.stride != 0) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = DXGI_FORMAT_UNKNOWN;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Buffer.NumElements = static_cast<UINT>(desc.size / desc.stride);
                    srv.Buffer.StructureByteStride = static_cast<UINT>(desc.stride);
                    allocate_srv(record, srv);
                }
                if (record.resource && desc.storageBuffer) allocate_uav(record);
                if (record.resource && !desc.vertexBuffer && !desc.indexBuffer) allocate_cbv(record, desc.size);
            } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                const auto compiled = shaderCompiler_->compile(desc, BackendApi::DirectX12);
                if (compiled.valid) {
                    if (!validate_backend_shader_layout(compiled.bindings, lastError_)) return;
                    record.shaderBytecode = compiled.bytecode;
                    record.shaderBindings = compiled.bindings;
                }
                else lastError_ = compiled.diagnostics;
            } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                record.pipeline = desc;
            } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                record.material = desc;
            }
        }, description);
        resources_.emplace(handle.id, std::move(record));
        if (const auto inserted = resources_.find(handle.id); inserted != resources_.end() && inserted->second.resource) {
            resourceStates_[inserted->second.resource] = inserted->second.state;
        }
        ++stats_.resourceCreates;
        if (const auto* buffer = std::get_if<BufferDesc>(&description);
            buffer && buffer->storageBuffer && !buffer->initialData.empty()) {
            update_buffer({handle, 0, buffer->initialData});
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture && !texture->initialData.empty()) {
            const auto textureFormat = texture->formatKind == TextureFormat::Unknown
                ? texture_format_from_name(texture->format) : texture->formatKind;
            const auto bytesPerPixel = textureFormat == TextureFormat::RGBA16Float ? 8u : 4u;
            if (!update_texture({handle, 0, 0, texture->width, texture->height,
                static_cast<std::size_t>(texture->width) * bytesPerPixel, texture->initialData})) {
                lastError_ = "D3D12 initial texture upload failed";
            } else if (texture->generateMips && (texture->mipLevels == 0 || texture->mipLevels > 1) && !generate_mips(handle)) {
                lastError_ = "D3D12 initial texture mip generation failed";
            }
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture) {
            for (const auto& subresource : texture->initialSubresources) {
                if (!update_texture({handle, subresource.mipLevel, subresource.layer, subresource.width,
                    subresource.height, subresource.rowPitch, subresource.data})) {
                    lastError_ = "D3D12 initial texture subresource upload failed";
                    return false;
                }
            }
        }
        const auto created = resources_.find(handle.id);
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil || handle.kind == ResourceKind::Buffer) &&
             !created->second.resource)) {
            if (lastError_.empty()) lastError_ = "D3D12 resource creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Shader && created->second.shaderBytecode.empty()) {
            if (lastError_.empty()) lastError_ = "D3D12 shader compilation failed";
            return false;
        }
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        retire_resource(handle);
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() || source->second.kind != physical.kind ||
            source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D && source->second.kind != ResourceKind::Buffer) ||
            !source->second.resource) return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.resource = source->second.resource;
        alias.pipeline = {};
        alias.material = {};
        alias.state = source->second.state;
        alias.size = source->second.size;
        alias.format = source->second.format;
        alias.width = source->second.width;
        alias.height = source->second.height;
        alias.mipLevels = source->second.mipLevels;
        alias.layers = source->second.layers;
        alias.srvGpu = source->second.srvGpu;
        alias.srvCpu = source->second.srvCpu;
        alias.cbvGpu = source->second.cbvGpu;
        alias.cbvCpu = source->second.cbvCpu;
        alias.rtvCpu = source->second.rtvCpu;
        alias.dsvCpu = source->second.dsvCpu;
        alias.uavGpu = source->second.uavGpu;
        alias.uavCpu = source->second.uavCpu;
        alias.srvIndex = source->second.srvIndex;
        alias.cbvIndex = source->second.cbvIndex;
        alias.uavIndex = source->second.uavIndex;
        alias.samplerIndex = source->second.samplerIndex;
        alias.rtvIndex = source->second.rtvIndex;
        alias.dsvIndex = source->second.dsvIndex;
        alias.srvValid = source->second.srvValid;
        alias.cbvValid = source->second.cbvValid;
        alias.uavValid = source->second.uavValid;
        alias.rtvValid = source->second.rtvValid;
        alias.dsvValid = source->second.dsvValid;
        alias.samplerGpu = source->second.samplerGpu;
        alias.samplerCpu = source->second.samplerCpu;
        alias.samplerValid = source->second.samplerValid;
        alias.alias = true;
        alias.resource->AddRef();
        resources_.emplace(logical.id, std::move(alias));
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() || it->second.kind != ResourceKind::Buffer) {
            lastError_ = "D3D12 buffer handle or resource kind is invalid";
            return false;
        }
        if (!it->second.resource) { lastError_ = "D3D12 buffer has no native resource"; return false; }
        if (update.data.empty() || update.offset + update.data.size() > it->second.size) { lastError_ = "D3D12 buffer update exceeds resource size"; return false; }
        void* mapped = nullptr;
        D3D12_RANGE range{0, 0};
        if (SUCCEEDED(it->second.resource->Map(0, &range, &mapped))) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + update.offset, update.data.data(), update.data.size());
            it->second.resource->Unmap(0, nullptr);
            return true;
        }
        if (frameActive_ || !queue_ || !fence_ || !fenceEvent_) {
            lastError_ = "D3D12 buffer requires staging upload outside an active frame";
            return false;
        }
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = update.data.size();
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        const HRESULT uploadResult = device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(uploadResult)) {
            lastError_ = "D3D12 staging buffer creation failed hr=" + std::to_string(static_cast<long long>(uploadResult));
            return false;
        }
        if (FAILED(upload->Map(0, &range, &mapped))) {
            upload->Release();
            lastError_ = "D3D12 staging buffer map failed";
            return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        upload->Unmap(0, nullptr);
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        const bool created = SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)));
        if (!created) {
            if (list) list->Release();
            if (allocator) allocator->Release();
            upload->Release();
            lastError_ = "D3D12 staging command list creation failed";
            return false;
        }
        D3D12_RESOURCE_BARRIER before{};
        before.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before.Transition.pResource = it->second.resource;
        before.Transition.StateBefore = it->second.state;
        before.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        before.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &before);
        list->CopyBufferRegion(it->second.resource, update.offset, upload, 0, update.data.size());
        D3D12_RESOURCE_BARRIER after = before;
        after.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after.Transition.StateAfter = it->second.state;
        list->ResourceBarrier(1, &after);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        ++fenceValue_;
        queue_->Signal(fence_, fenceValue_);
        if (uploadBatchActive_) {
            pendingUploads_.push_back({fenceValue_, allocator, list, upload});
            return true;
        }
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        list->Release();
        allocator->Release();
        upload->Release();
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            !it->second.resource || update.data.empty() || update.width == 0 || update.height == 0 || frameActive_) {
            lastError_ = "D3D12 texture handle or update data is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.kind == ResourceKind::DepthStencil || update.mipLevel >= resource.mipLevels || update.layer >= resource.layers) return false;
        const auto bytesPerPixel = resource.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        const auto sourcePitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * bytesPerPixel : update.rowPitch;
        if (update.x > std::max(1u, resource.width >> update.mipLevel) ||
            update.y > std::max(1u, resource.height >> update.mipLevel) ||
            update.width > std::max(1u, resource.width >> update.mipLevel) - update.x ||
            update.height > std::max(1u, resource.height >> update.mipLevel) - update.y ||
            sourcePitch < static_cast<std::size_t>(update.width) * bytesPerPixel ||
            sourcePitch * update.height > update.data.size()) return false;
        if (update.mipLevel == 0 && update.layer == 0 && bytesPerPixel == 4) {
            const auto fullSize = static_cast<std::size_t>(resource.width) * resource.height * 4u;
            if (resource.baseTextureData.size() != fullSize) resource.baseTextureData.assign(fullSize, 0);
            for (std::uint32_t row = 0; row < update.height; ++row) {
                const auto destinationOffset = (static_cast<std::size_t>(update.y + row) * resource.width + update.x) * 4u;
                std::memcpy(resource.baseTextureData.data() + destinationOffset, update.data.data() + row * sourcePitch,
                    static_cast<std::size_t>(update.width) * 4u);
            }
        }
        const auto rowPitch = (static_cast<std::size_t>(update.width) * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        footprint.Footprint.Format = resource.format;
        footprint.Footprint.Width = update.width;
        footprint.Footprint.Height = update.height;
        footprint.Footprint.Depth = 1;
        footprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = rowPitch * update.height;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(upload->Map(0, &readRange, &mapped))) { upload->Release(); return false; }
        for (std::uint32_t row = 0; row < update.height; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + row * rowPitch,
                update.data.data() + row * sourcePitch, std::min<std::size_t>(sourcePitch, rowPitch));
        }
        upload->Unmap(0, nullptr);
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
            if (list) list->Release();
            if (allocator) allocator->Release();
            upload->Release();
            return false;
        }
        D3D12_RESOURCE_BARRIER before{};
        before.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before.Transition.pResource = resource.resource;
        before.Transition.StateBefore = resource.state;
        before.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        before.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &before);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = resource.resource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = update.mipLevel + update.layer * resource.mipLevels;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, update.x, update.y, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER after = before;
        after.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after.Transition.StateAfter = resource.state;
        list->ResourceBarrier(1, &after);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        ++fenceValue_;
        queue_->Signal(fence_, fenceValue_);
        if (uploadBatchActive_) {
            pendingUploads_.push_back({fenceValue_, allocator, list, upload});
            return true;
        }
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        list->Release(); allocator->Release(); upload->Release();
        return true;
    }
    bool resolve_texture(ResourceHandle destination, ResourceHandle source) override {
        const auto destinationIt = resources_.find(destination.id);
        const auto sourceIt = resources_.find(source.id);
        if (!frameActive_ || !commandList_ || destination.kind != ResourceKind::Texture2D ||
            source.kind != ResourceKind::Texture2D || destination.id == source.id ||
            destinationIt == resources_.end() || sourceIt == resources_.end() ||
            !destinationIt->second.resource || !sourceIt->second.resource) {
            lastError_ = "D3D12 resolve requires an active frame and two valid texture resources";
            return false;
        }
        const auto& destinationDescription = destinationIt->second.nativeDescription;
        const auto& sourceDescription = sourceIt->second.nativeDescription;
        if (sourceDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            destinationDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            sourceDescription.SampleDesc.Count <= 1 || destinationDescription.SampleDesc.Count != 1 ||
            sourceDescription.Width != destinationDescription.Width ||
            sourceDescription.Height != destinationDescription.Height ||
            sourceDescription.DepthOrArraySize != destinationDescription.DepthOrArraySize ||
            sourceDescription.MipLevels != destinationDescription.MipLevels ||
            sourceDescription.Format != destinationDescription.Format ||
            sourceDescription.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
            sourceDescription.Format == DXGI_FORMAT_D32_FLOAT) {
            lastError_ = "D3D12 resolve resources must match and use a color MSAA source";
            return false;
        }
        auto sourceState = sourceIt->second.state;
        if (const auto state = resourceStates_.find(sourceIt->second.resource); state != resourceStates_.end()) sourceState = state->second;
        auto destinationState = destinationIt->second.state;
        if (const auto state = resourceStates_.find(destinationIt->second.resource); state != resourceStates_.end()) destinationState = state->second;
        const auto addBarrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                    D3D12_RESOURCE_STATES after) {
            if (before == after) return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList_->ResourceBarrier(1, &barrier);
            ++stats_.barriers;
        };
        addBarrier(sourceIt->second.resource, sourceState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        addBarrier(destinationIt->second.resource, destinationState, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        for (UINT layer = 0; layer < sourceDescription.DepthOrArraySize; ++layer) {
            for (UINT mip = 0; mip < sourceDescription.MipLevels; ++mip) {
                const auto subresource = mip + layer * sourceDescription.MipLevels;
                commandList_->ResolveSubresource(destinationIt->second.resource, subresource,
                                                 sourceIt->second.resource, subresource, sourceDescription.Format);
            }
        }
        addBarrier(sourceIt->second.resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceState);
        addBarrier(destinationIt->second.resource, D3D12_RESOURCE_STATE_RESOLVE_DEST, destinationState);
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            it->second.mipLevels <= 1) {
            lastError_ = "D3D12 mip generation handle or resource is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.format != DXGI_FORMAT_R8G8B8A8_UNORM || resource.baseTextureData.size() !=
            static_cast<std::size_t>(resource.width) * resource.height * 4u) {
            lastError_ = "D3D12 mip generation currently requires an uploaded rgba8 base level";
            return false;
        }
        std::vector<std::uint8_t> source = resource.baseTextureData;
        auto sourceWidth = resource.width;
        auto sourceHeight = resource.height;
        for (std::uint32_t level = 1; level < resource.mipLevels; ++level) {
            const auto destinationWidth = std::max(1u, sourceWidth / 2u);
            const auto destinationHeight = std::max(1u, sourceHeight / 2u);
            std::vector<std::uint8_t> destination(static_cast<std::size_t>(destinationWidth) * destinationHeight * 4u);
            for (std::uint32_t y = 0; y < destinationHeight; ++y) {
                for (std::uint32_t x = 0; x < destinationWidth; ++x) {
                    for (std::uint32_t channel = 0; channel < 4; ++channel) {
                        std::uint32_t sum = 0;
                        for (std::uint32_t sampleY = 0; sampleY < 2; ++sampleY) {
                            for (std::uint32_t sampleX = 0; sampleX < 2; ++sampleX) {
                                const auto sourceX = std::min(sourceWidth - 1u, x * 2u + sampleX);
                                const auto sourceY = std::min(sourceHeight - 1u, y * 2u + sampleY);
                                sum += source[(static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4u + channel];
                            }
                        }
                        destination[(static_cast<std::size_t>(y) * destinationWidth + x) * 4u + channel] = static_cast<std::uint8_t>(sum / 4u);
                    }
                }
            }
            if (!update_texture({texture, level, 0, destinationWidth, destinationHeight, 0, destination})) return false;
            source = std::move(destination);
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        return true;
    }
    BindlessTableHandle create_bindless_table(const BindlessTableDesc& description) override {
        if (!capabilities_.supportsBindless || description.type != DescriptorType::Texture || !srvHeap_ ||
            description.capacity == 0 || description.capacity > 1024u) {
            lastError_ = "D3D12 bindless tables currently support texture SRVs only";
            return {};
        }
        UINT baseIndex = 0;
        if (!take_srv_range(description.capacity, baseIndex)) {
            lastError_ = "D3D12 bindless descriptor range is exhausted or fragmented";
            return {};
        }
        const auto id = nextBindlessTable_++;
        bindlessTables_[id] = {baseIndex, description.capacity};
        return {id, bindlessTables_[id].baseIndex, bindlessTables_[id].capacity};
    }
    bool update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource, DescriptorType type) override {
        const auto tableIt = bindlessTables_.find(table.id);
        const auto resourceIt = resources_.find(resource.id);
        if (tableIt == bindlessTables_.end() || slot >= tableIt->second.capacity || resourceIt == resources_.end() ||
            !resourceIt->second.srvValid || type != DescriptorType::Texture) {
            lastError_ = "D3D12 bindless update requires a texture SRV";
            return false;
        }
        auto destination = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destination.ptr += static_cast<SIZE_T>(tableIt->second.baseIndex + slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destination, resourceIt->second.srvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }
    void destroy_bindless_table(BindlessTableHandle table) override {
        if (boundBindlessTable_ == table.id) boundBindlessTable_ = 0;
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end()) return;
        retiredBindlessTables_.push_back({fenceValue_, it->second});
        bindlessTables_.erase(it);
    }
    void bind_bindless_table(BindlessTableHandle table) override {
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end() || !commandList_ || !srvHeap_) return;
        boundBindlessTable_ = table.id;
        ID3D12DescriptorHeap* heaps[] = {srvHeap_};
        commandList_->SetDescriptorHeaps(1, heaps);
        if (!boundPipeline_ || !boundPipeline_->rootSignature || !boundPipeline_->pipelineState) return;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(it->second.baseIndex) * srvStride_;
        const auto parameter = boundPipeline_->rootParameters.find(d3d12_binding_key(0, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV));
        if (parameter == boundPipeline_->rootParameters.end()) return;
        if (boundPipeline_->pipeline.computeShader) commandList_->SetComputeRootDescriptorTable(parameter->second, gpu);
        else commandList_->SetGraphicsRootDescriptorTable(parameter->second, gpu);
    }
    void retire_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) return;
        if (boundPipeline_ == &it->second) boundPipeline_ = nullptr;
        if (boundMaterial_ == &it->second) boundMaterial_ = nullptr;
        if (it->second.kind == ResourceKind::Material) defer_material_ranges(it->second);
        if (!it->second.alias && it->second.resource) resourceStates_.erase(it->second.resource);
        retiredResources_.push_back({fenceValue_, std::move(it->second)});
        resources_.erase(it);
    }
    void collect_garbage() override {
        release_completed_uploads();
        const auto completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
        retiredBindlessTables_.erase(std::remove_if(retiredBindlessTables_.begin(), retiredBindlessTables_.end(),
            [&](auto& retired) {
                if (retired.first != 0 && retired.first > completed) return false;
                for (std::uint32_t slot = 0; slot < retired.second.capacity; ++slot) {
                    release_srv_slot(retired.second.baseIndex + slot);
                }
                return true;
            }), retiredBindlessTables_.end());
        retiredMaterialRanges_.erase(std::remove_if(retiredMaterialRanges_.begin(), retiredMaterialRanges_.end(),
            [&](auto& retired) {
                if (retired.first != 0 && retired.first > completed) return false;
                for (const auto& [key, range] : retired.second) {
                    (void)key;
                    release_sampler_range(range);
                }
                return true;
            }), retiredMaterialRanges_.end());
            retiredResources_.erase(std::remove_if(retiredResources_.begin(), retiredResources_.end(),
            [&](auto& retired) {
                if (retired.first != 0 && retired.first > completed) return false;
                // Alias records share the physical resource's descriptor slots.
                // They retain the COM resource, but must never return the
                // physical descriptors to a freelist a second time.
                if (!retired.second.alias) {
                    release_gpu_allocation(retired.second);
                    if (retired.second.srvValid) release_srv_slot(retired.second.srvIndex);
                    if (retired.second.cbvValid) release_srv_slot(retired.second.cbvIndex);
                    if (retired.second.uavValid) release_srv_slot(retired.second.uavIndex);
                    if (retired.second.samplerValid) release_sampler_slot(retired.second.samplerIndex);
                    if (retired.second.rtvValid) release_rtv_slot(retired.second.rtvIndex);
                    if (retired.second.dsvValid) release_dsv_slot(retired.second.dsvIndex);
                }
                ++stats_.resourceDestroys;
                return true;
            }), retiredResources_.end());
    }
    void begin_upload_batch() override {
        uploadBatchActive_ = true;
        release_completed_uploads();
    }
    bool flush_upload_batch() override {
        uploadBatchActive_ = false;
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        if (!commandList_) { lastError_ = "D3D12 resource transition command list is invalid"; return false; }
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind || !it->second.resource) {
            lastError_ = "D3D12 resource transition handle is invalid";
            return false;
        }
        D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
        if (is_color_attachment_usage(usage)) target = D3D12_RESOURCE_STATE_RENDER_TARGET;
        else if (usage == ResourceUsage::DepthStencil) target = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        else if (usage == ResourceUsage::VertexBuffer) target = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        else if (usage == ResourceUsage::IndexBuffer) target = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        else if (usage == ResourceUsage::UniformBuffer) target = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        else if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::StorageRead) {
            target = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        } else if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite) {
            target = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else if (usage == ResourceUsage::CopySource) target = D3D12_RESOURCE_STATE_COPY_SOURCE;
        else if (usage == ResourceUsage::CopyDestination) target = D3D12_RESOURCE_STATE_COPY_DEST;
        else if (usage == ResourceUsage::IndirectArguments) target = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        else if (usage == ResourceUsage::Present) target = D3D12_RESOURCE_STATE_PRESENT;
        const auto stateIt = resourceStates_.find(it->second.resource);
        const auto currentState = stateIt == resourceStates_.end() ? it->second.state : stateIt->second;
        if (currentState == target || !it->second.resource) return true;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = it->second.resource;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);
        it->second.state = target;
        resourceStates_[it->second.resource] = target;
        ++stats_.barriers;
        return true;
    }
    void set_render_target(ResourceHandle target) override {
        if (!commandList_ || !frameActive_) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        if (target) {
            const auto it = resources_.find(target.id);
            if (it != resources_.end() && it->second.rtvValid) handle = it->second.rtvCpu;
        }
        if (!handle.ptr) {
            handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
        }
        commandList_->OMSetRenderTargets(1, &handle, FALSE, dsvHeap_ ? &dsvHandle_ : nullptr);
        currentRenderTarget_ = target.id;
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        if (!commandList_ || !frameActive_) return;
        D3D12_CPU_DESCRIPTOR_HANDLE colorHandle{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (color && colorIt != resources_.end() && colorIt->second.rtvValid) colorHandle = colorIt->second.rtvCpu;
        if (color && !colorHandle.ptr) {
            colorHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            colorHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
        }
        if (depth && depthIt != resources_.end() && depthIt->second.dsvValid) depthHandle = depthIt->second.dsvCpu;
        commandList_->OMSetRenderTargets(color ? 1u : 0u, color ? &colorHandle : nullptr, FALSE, depthHandle.ptr ? &depthHandle : nullptr);
        currentRenderTarget_ = color ? color.id : depth.id;
    }
    void set_render_targets(const std::vector<ResourceHandle>& colors, ResourceHandle depth, bool clearAttachments) override {
        if (!commandList_ || !frameActive_) return;
        if (colors.size() <= 1) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth);
            if (clearAttachments) {
                constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (!colors.empty()) {
                    const auto color = resources_.find(colors.front().id);
                    if (color != resources_.end() && color->second.rtvValid) commandList_->ClearRenderTargetView(color->second.rtvCpu, clear, 0, nullptr);
                }
                if (depth) {
                    const auto depthResource = resources_.find(depth.id);
                    if (depthResource != resources_.end() && depthResource->second.dsvValid) {
                        commandList_->ClearDepthStencilView(depthResource->second.dsvCpu,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
                    }
                }
            }
            return;
        }
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> colorHandles{};
        UINT count = static_cast<UINT>(std::min<std::size_t>(colors.size(), colorHandles.size()));
        for (UINT index = 0; index < count; ++index) {
            const auto it = resources_.find(colors[index].id);
            if (it != resources_.end() && it->second.rtvValid) colorHandles[index] = it->second.rtvCpu;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (it != resources_.end() && it->second.dsvValid) depthHandle = it->second.dsvCpu;
        }
        commandList_->OMSetRenderTargets(count, count ? colorHandles.data() : nullptr, FALSE,
            depthHandle.ptr ? &depthHandle : nullptr);
        if (clearAttachments) {
            constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (UINT index = 0; index < count; ++index) {
                if (colorHandles[index].ptr) commandList_->ClearRenderTargetView(colorHandles[index], clear, 0, nullptr);
            }
            if (depth && depthHandle.ptr) commandList_->ClearDepthStencilView(depthHandle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        }
        currentRenderTarget_ = colors.empty() ? depth.id : colors.front().id;
    }
    void set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth) override {
        if (!commandList_ || !frameActive_) return;
        D3D12_VIEWPORT viewport{x, y, width, height, minDepth, maxDepth};
        D3D12_RECT scissor{
            static_cast<LONG>(std::floor(x)),
            static_cast<LONG>(std::floor(y)),
            static_cast<LONG>(std::ceil(x + width)),
            static_cast<LONG>(std::ceil(y + height))};
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
    }
    bool ensure_pipeline(ResourceRecord& record) {
        if (record.pipelineState) return true;
        if (!device_ || !commandList_) return false;
        if (record.pipeline.computeShader != 0) {
            const auto compute = resources_.find(record.pipeline.computeShader);
            if (compute == resources_.end() || compute->second.shaderBytecode.empty()) return false;
            std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
            std::vector<std::uint64_t> keys;
            const auto addBinding = [&](std::uint32_t space, std::uint32_t slot, D3D12_DESCRIPTOR_RANGE_TYPE type,
                                        std::uint32_t count = 1, bool unbounded = false) {
                const auto key = d3d12_binding_key(space, slot, type);
                const auto existing = std::find(keys.begin(), keys.end(), key);
                if (existing != keys.end()) {
                    const auto index = static_cast<std::size_t>(std::distance(keys.begin(), existing));
                    return ranges[index].RangeType == type && ranges[index].NumDescriptors == (unbounded ? UINT_MAX : count);
                }
                if (keys.size() >= 64) return false;
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = type;
                range.BaseShaderRegister = slot;
                range.RegisterSpace = space;
                range.NumDescriptors = unbounded ? UINT_MAX : count;
                ranges.push_back(range);
                keys.push_back(key);
                return true;
            };
            const auto addReflected = [&](const ShaderBinding& binding) {
                if (binding.count == 0 && !binding.unbounded) return false;
                if (binding.type == "sampler") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, binding.count, binding.unbounded);
                if (binding.type == "uniform_buffer") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, binding.count, binding.unbounded);
                if (binding.type == "storage_buffer" || binding.type == "storage_texture") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding.count, binding.unbounded);
                return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                    binding.count, binding.unbounded);
            };
            for (const auto& reflected : compute->second.shaderBindings) if (!addReflected(reflected)) return false;
            std::vector<D3D12_ROOT_PARAMETER> parameters(ranges.size());
            record.rootParameters.clear();
            for (UINT i = 0; i < ranges.size(); ++i) {
                parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                parameters[i].DescriptorTable.NumDescriptorRanges = 1;
                parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
                record.rootParameters[keys[i]] = i;
            }
            D3D12_ROOT_SIGNATURE_DESC rootDesc{};
            rootDesc.NumParameters = static_cast<UINT>(parameters.size());
            rootDesc.pParameters = parameters.data();
            ID3DBlob* serialized = nullptr;
            ID3DBlob* errors = nullptr;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))) {
                lastError_ = "D3D12 compute root signature serialization failed";
                if (errors && errors->GetBufferPointer()) {
                    lastError_ += " [" + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) + "]";
                }
                if (errors) errors->Release();
                return false;
            }
            const auto rootResult = device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&record.rootSignature));
            serialized->Release();
            if (FAILED(rootResult)) {
                append_d3d12_diagnostics("compute root signature creation", rootResult);
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = record.rootSignature;
            pipeline.CS = {compute->second.shaderBytecode.data(), compute->second.shaderBytecode.size()};
            const auto cacheName = pipeline_library_name(record.pipeline, {}, {}, compute->second.shaderBytecode, L":compute");
            if (pipelineLibrary_ && SUCCEEDED(pipelineLibrary_->LoadComputePipeline(cacheName.c_str(), &pipeline,
                IID_PPV_ARGS(&record.pipelineState)))) {
                ++stats_.pipelineCacheHits;
                return true;
            }
            if (FAILED(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&record.pipelineState)))) {
                record.rootSignature->Release();
                record.rootSignature = nullptr;
                return false;
            }
            if (pipelineLibrary_) pipelineLibrary_->StorePipeline(cacheName.c_str(), record.pipelineState);
            return true;
        }
        if (record.pipeline.sampleCount != 1 ||
            (record.pipeline.fillMode != "solid" && record.pipeline.fillMode != "wireframe") ||
            (record.pipeline.cullMode != "back" && record.pipeline.cullMode != "front" && record.pipeline.cullMode != "none") ||
            (record.pipeline.topology != "triangle" && record.pipeline.topology != "line" && record.pipeline.topology != "point") ||
            record.pipeline.colorFormats.size() > 8 ||
            std::any_of(record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string& format) { return !is_supported_color_format(format); }) ||
            !is_supported_color_format(record.pipeline.colorFormat) ||
            (record.pipeline.depthFormat != "d24s8" && record.pipeline.depthFormat != "none")) {
            lastError_ = "D3D12 pipeline fixed-function state or attachment format is unsupported";
            return false;
        }
        const auto vertex = resources_.find(record.pipeline.vertexShader);
        const auto fragment = resources_.find(record.pipeline.fragmentShader);
        if (vertex == resources_.end() || fragment == resources_.end() || vertex->second.kind != ResourceKind::Shader ||
            fragment->second.kind != ResourceKind::Shader || vertex->second.shaderBytecode.empty() || fragment->second.shaderBytecode.empty()) {
            lastError_ = "D3D12 graphics pipeline references missing or empty shader bytecode";
            return false;
        }
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
        std::vector<std::uint64_t> keys;
            const auto addBinding = [&](std::uint32_t space, std::uint32_t slot, D3D12_DESCRIPTOR_RANGE_TYPE type,
                                    std::uint32_t count = 1, bool unbounded = false) {
            const auto key = d3d12_binding_key(space, slot, type);
            const auto existing = std::find(keys.begin(), keys.end(), key);
            if (existing != keys.end()) {
                const auto index = static_cast<std::size_t>(std::distance(keys.begin(), existing));
                return ranges[index].RangeType == type && ranges[index].NumDescriptors == (unbounded ? UINT_MAX : count);
            }
            if (keys.size() >= 64) return false;
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = type;
            range.BaseShaderRegister = slot;
            range.RegisterSpace = space;
            range.NumDescriptors = unbounded ? UINT_MAX : count;
            ranges.push_back(range);
            keys.push_back(key);
            return true;
        };
        const auto addReflected = [&](const ShaderBinding& binding) {
            if (binding.count == 0 && !binding.unbounded) return false;
            if (binding.type == "sampler") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, binding.count, binding.unbounded);
            if (binding.type == "uniform_buffer") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, binding.count, binding.unbounded);
            if (binding.type == "storage_buffer" || binding.type == "storage_texture") return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding.count, binding.unbounded);
            return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                binding.count, binding.unbounded);
        };
        for (const auto shaderId : {record.pipeline.vertexShader, record.pipeline.fragmentShader}) {
            const auto shader = resources_.find(shaderId);
            if (shader == resources_.end()) continue;
            for (const auto& reflected : shader->second.shaderBindings) if (!addReflected(reflected)) return false;
        }
        std::vector<D3D12_ROOT_PARAMETER> parameters(ranges.size());
        record.rootParameters.clear();
        for (UINT i = 0; i < ranges.size(); ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[i].DescriptorTable.NumDescriptorRanges = 1;
            parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            record.rootParameters[keys[i]] = i;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = static_cast<UINT>(parameters.size());
        rootDesc.pParameters = parameters.data();
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* serialized = nullptr;
        ID3DBlob* errors = nullptr;
        const auto serializeResult = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (FAILED(serializeResult)) {
            lastError_ = "D3D12 graphics root signature serialization failed hr=" + std::to_string(static_cast<long long>(serializeResult));
            if (errors && errors->GetBufferPointer()) {
                lastError_ += " [" + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) + "]";
            }
            if (errors) errors->Release();
            return false;
        }
        const HRESULT rootResult = device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&record.rootSignature));
        serialized->Release();
        if (FAILED(rootResult)) {
            append_d3d12_diagnostics("graphics root signature creation", rootResult);
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = record.rootSignature;
        pipeline.VS = {vertex->second.shaderBytecode.data(), vertex->second.shaderBytecode.size()};
        pipeline.PS = {fragment->second.shaderBytecode.data(), fragment->second.shaderBytecode.size()};
        D3D12_INPUT_ELEMENT_DESC inputElement{};
        if (record.pipeline.vertexInput) {
            inputElement.SemanticName = "POSITION";
            inputElement.SemanticIndex = 0;
            inputElement.Format = DXGI_FORMAT_R32G32B32_FLOAT;
            inputElement.InputSlot = 0;
            inputElement.AlignedByteOffset = 0;
            inputElement.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            inputElement.InstanceDataStepRate = 0;
            pipeline.InputLayout = {&inputElement, 1};
        }
        pipeline.RasterizerState.FillMode = fill_mode(record.pipeline.fillMode);
        pipeline.RasterizerState.CullMode = cull_mode(record.pipeline.cullMode);
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.BlendState.RenderTarget[0].BlendEnable = record.pipeline.alphaBlend ? TRUE : FALSE;
        pipeline.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pipeline.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pipeline.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pipeline.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pipeline.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pipeline.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pipeline.DepthStencilState.DepthEnable = record.pipeline.depthTest ? TRUE : FALSE;
        pipeline.DepthStencilState.DepthWriteMask = record.pipeline.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = topology_type(record.pipeline.topology);
        const auto colorFormats = record.pipeline.colorFormats.empty()
            ? std::vector<std::string>{record.pipeline.colorFormat} : record.pipeline.colorFormats;
        pipeline.NumRenderTargets = static_cast<UINT>(colorFormats.size());
        for (UINT index = 0; index < pipeline.NumRenderTargets; ++index) {
            pipeline.RTVFormats[index] = format_of(colorFormats[index]);
        }
        pipeline.DSVFormat = record.pipeline.depthFormat == "none" ? DXGI_FORMAT_UNKNOWN : format_of(record.pipeline.depthFormat);
        if (record.pipeline.depthFormat == "none") {
            pipeline.DepthStencilState.DepthEnable = FALSE;
            pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        pipeline.SampleDesc.Count = record.pipeline.sampleCount;
        const auto cacheName = pipeline_library_name(record.pipeline, vertex->second.shaderBytecode, fragment->second.shaderBytecode);
        if (pipelineLibrary_ && SUCCEEDED(pipelineLibrary_->LoadGraphicsPipeline(cacheName.c_str(), &pipeline,
            IID_PPV_ARGS(&record.pipelineState)))) {
            ++stats_.pipelineCacheHits;
            return true;
        }
        const auto pipelineResult = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&record.pipelineState));
        if (FAILED(pipelineResult)) {
            record.rootSignature->Release();
            record.rootSignature = nullptr;
            lastError_ = "D3D12 graphics pipeline creation failed hr=" + std::to_string(static_cast<long long>(pipelineResult));
            ID3D12InfoQueue* infoQueue = nullptr;
            if (device_ && SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue) {
                const auto messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
                for (UINT64 index = 0; index < messageCount; ++index) {
                    SIZE_T messageSize = 0;
                    if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) || messageSize == 0) continue;
                    std::vector<std::uint8_t> messageStorage(messageSize);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageStorage.data());
                    if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)) && message->pDescription) {
                        lastError_ += " [" + std::string(message->pDescription) + "]";
                    }
                }
                infoQueue->ClearStoredMessages();
                infoQueue->Release();
            }
            return false;
        }
        if (pipelineLibrary_) pipelineLibrary_->StorePipeline(cacheName.c_str(), record.pipelineState);
        auto depthOnly = pipeline;
        depthOnly.NumRenderTargets = 0;
        depthOnly.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        if (FAILED(device_->CreateGraphicsPipelineState(&depthOnly, IID_PPV_ARGS(&record.depthOnlyPipeline))) && record.depthOnlyPipeline) {
            record.depthOnlyPipeline->Release();
            record.depthOnlyPipeline = nullptr;
        }
        return true;
    }
    void begin_frame() override {
        ++stats_.frames;
        recordedQueueBatches_.clear();
        if (!commandList_ || !commandAllocator_) return;
        const auto deviceReason = device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
        if (deviceReason == DXGI_ERROR_DEVICE_REMOVED || deviceReason == DXGI_ERROR_DEVICE_RESET) {
            lastError_ = "D3D12 device lost before frame begin hr=" + std::to_string(static_cast<long long>(deviceReason));
            capabilities_.deviceState = RenderDeviceState::Lost;
            return;
        }
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (FAILED(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_))) {
                lastError_ = "D3D12 fence wait setup failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
            }
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        read_timestamp_results();
        submittedTimestampCount_ = 0;
        timestampCursor_ = 0;
        collect_garbage();
        if (swapChain_) frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
        if (FAILED(commandAllocator_->Reset()) || FAILED(commandList_->Reset(commandAllocator_, nullptr))) {
            lastError_ = "D3D12 command recording reset failed";
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState = reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return;
        }
        if (!swapChain_) {
            timestampCursor_ = 0;
            frameActive_ = true;
            return;
        }
        auto* buffer = backBuffers_[frameIndex_];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
        commandList_->OMSetRenderTargets(1, &handle, FALSE, dsvHeap_ ? &dsvHandle_ : nullptr);
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
        constexpr float clear[4] = {0.035f, 0.045f, 0.065f, 1.0f};
        commandList_->ClearRenderTargetView(handle, clear, 0, nullptr);
        if (dsvHeap_) commandList_->ClearDepthStencilView(dsvHandle_, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        frameActive_ = true;
    }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        if (!frameActive_ || !commandList_) {
            lastError_ = "D3D12 queue batch began outside an active frame";
            return false;
        }
        for (const auto waitBatch : waitBatches) {
            if (waitBatch >= recordedQueueBatches_.size() || !recordedQueueBatches_[waitBatch]) {
                lastError_ = "D3D12 queue batch wait references an unsignaled batch";
                return false;
            }
        }
        if (recordedQueueBatches_.size() <= batchIndex) recordedQueueBatches_.resize(batchIndex + 1, false);
        recordedQueueBatches_[batchIndex] = true;
        return true;
    }
    void begin_pass() override {
        if (timestampQueryHeap_ && commandList_ && frameActive_ && timestampCursor_ < 512) {
            commandList_->EndQuery(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP, timestampCursor_++);
        }
    }
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void end_pass() override {
        if (timestampQueryHeap_ && commandList_ && frameActive_ && timestampCursor_ < 512) {
            commandList_->EndQuery(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP, timestampCursor_++);
        }
    }
    void set_debug_name(ResourceHandle handle, std::string_view name) override {
        debugNames_[handle.id] = std::string(name);
        const auto resource = resources_.find(handle.id);
        if (resource != resources_.end() && resource->second.resource) {
            const auto wide = wide_name(name);
            resource->second.resource->SetName(wide.c_str());
        }
    }
    void begin_debug_label(std::string_view name) override {
        ++stats_.debugMarkers;
        if (commandList_) {
            const auto wide = wide_name(name);
            commandList_->BeginEvent(0, wide.c_str(), static_cast<UINT>(wide.size() * sizeof(wchar_t)));
        }
    }
    void end_debug_label() override { if (commandList_) commandList_->EndEvent(); }
    void bind_pipeline(std::string_view name) override {
        for (auto& [id, resource] : resources_) {
            if (resource.kind == ResourceKind::Pipeline && resource.pipeline.name == name) {
                bind_pipeline(ResourceHandle{id, ResourceKind::Pipeline});
                return;
            }
        }
        boundPipeline_ = nullptr;
        lastError_ = "D3D12 pipeline name is invalid: " + std::string(name);
    }
    void bind_pipeline(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        boundPipeline_ = it != resources_.end() && it->second.kind == ResourceKind::Pipeline &&
            handle.kind == ResourceKind::Pipeline ? &it->second : nullptr;
        if (!boundPipeline_) {
            lastError_ = "D3D12 pipeline handle is invalid";
            return;
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_) && commandList_) {
            if (boundPipeline_->pipeline.computeShader) {
                commandList_->SetComputeRootSignature(boundPipeline_->rootSignature);
                commandList_->SetPipelineState(boundPipeline_->pipelineState);
            } else {
                commandList_->SetGraphicsRootSignature(boundPipeline_->rootSignature);
                const auto target = resources_.find(currentRenderTarget_);
                const bool depthOnly = target != resources_.end() && target->second.kind == ResourceKind::DepthStencil;
                commandList_->SetPipelineState(depthOnly && boundPipeline_->depthOnlyPipeline ? boundPipeline_->depthOnlyPipeline : boundPipeline_->pipelineState);
                commandList_->IASetPrimitiveTopology(topology(boundPipeline_->pipeline.topology));
            }
            const auto table = bindlessTables_.find(boundBindlessTable_);
            if (table != bindlessTables_.end()) {
                bind_bindless_table({boundBindlessTable_, table->second.baseIndex, table->second.capacity});
            }
        } else if (boundPipeline_ && lastError_.empty()) {
            lastError_ = "D3D12 pipeline binding failed";
        }
    }
    void bind_material(ResourceHandle handle) override {
        const auto material = resources_.find(handle.id);
        if (material != resources_.end() && material->second.kind == ResourceKind::Material &&
            handle.kind == ResourceKind::Material) {
            ++stats_.materialBinds;
            boundMaterial_ = &material->second;
            bind_pipeline(material->second.material.pipeline);
        }
        if (material == resources_.end() || material->second.kind != ResourceKind::Material ||
            handle.kind != ResourceKind::Material) {
            lastError_ = "D3D12 material handle is invalid";
            return;
        }
        if (!commandList_ || !srvHeap_ || !boundPipeline_ || !boundPipeline_->rootSignature || !boundPipeline_->pipelineState) {
            if (lastError_.empty()) lastError_ = "D3D12 material binding has invalid command or pipeline state";
            return;
        }
        if (material->second.material.bindless) {
            if (boundBindlessTable_ != 0) {
                const auto table = bindlessTables_.find(boundBindlessTable_);
                if (table != bindlessTables_.end()) {
                    bind_bindless_table({boundBindlessTable_, table->second.baseIndex, table->second.capacity});
                }
            }
            return;
        }
        for (const auto& binding : material->second.material.bindings) {
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId : {boundPipeline_->pipeline.vertexShader, boundPipeline_->pipeline.fragmentShader, boundPipeline_->pipeline.computeShader}) {
                const auto shader = resources_.find(shaderId);
                if (shader == resources_.end() || shader->second.shaderBindings.empty()) continue;
                reflectionAvailable = true;
                found |= std::any_of(shader->second.shaderBindings.begin(), shader->second.shaderBindings.end(),
                    [&](const auto& reflected) { return shader_binding_matches(reflected, binding); });
            }
            if (reflectionAvailable && !found) {
                lastError_ = "D3D12 material '" + material->second.material.name + "' binding '" + binding.name +
                    "' does not match shader slot=" + std::to_string(binding.slot) +
                    " space=" + std::to_string(binding.space) + " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        ID3D12DescriptorHeap* heaps[] = {srvHeap_, samplerHeap_};
        commandList_->SetDescriptorHeaps(samplerHeap_ ? 2 : 1, heaps);
        if (!material->second.materialRangesValid && !build_material_descriptor_ranges(material->second, *boundPipeline_)) return;
        stats_.descriptorBinds += material->second.material.bindings.size();
        for (const auto& binding : material->second.material.bindings) {
            if (binding.slot < 16) {
                const auto rangeType = descriptor_range_type(binding.type);
                const auto parameter = boundPipeline_->rootParameters.find(d3d12_binding_key(binding.space, binding.slot, rangeType));
                const auto setTable = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
                    if (parameter == boundPipeline_->rootParameters.end()) return;
                    if (boundPipeline_->pipeline.computeShader) commandList_->SetComputeRootDescriptorTable(parameter->second, gpu);
                    else commandList_->SetGraphicsRootDescriptorTable(parameter->second, gpu);
                };
                const auto range = material->second.materialRanges.find(d3d12_binding_key(binding.space, binding.slot, rangeType));
                if (range != material->second.materialRanges.end()) setTable(material_range_gpu(range->second));
            }
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() || material->second.kind != ResourceKind::Material) {
            lastError_ = "D3D12 material handle not found";
            return false;
        }
        if (material->second.material.pipeline.id != description.pipeline.id) {
            lastError_ = "D3D12 material pipeline cannot be changed after creation";
            return false;
        }
        defer_material_ranges(material->second);
        material->second.material = description;
        if (boundMaterial_ == &material->second) boundMaterial_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot, std::uint32_t space) override {
        if (!commandList_ || !srvHeap_ || slot >= 16) {
            lastError_ = "D3D12 uniform buffer binding has invalid command state or slot";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() || resource->second.kind != ResourceKind::Buffer ||
            !resource->second.cbvValid) {
            lastError_ = "D3D12 uniform buffer handle is invalid";
            return;
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_)) {
            const auto parameter = boundPipeline_->rootParameters.find(d3d12_binding_key(space, slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV));
            if (parameter == boundPipeline_->rootParameters.end()) {
                lastError_ = "D3D12 uniform buffer binding is absent from the pipeline layout";
                return;
            }
            if (boundPipeline_->pipeline.computeShader) commandList_->SetComputeRootDescriptorTable(parameter->second, resource->second.cbvGpu);
            else commandList_->SetGraphicsRootDescriptorTable(parameter->second, resource->second.cbvGpu);
        }
    }
    void draw_sprite(const SpriteDraw&) override {
        ++stats_.drawCalls; ++stats_.spriteCalls;
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState) {
            if (lastError_.empty()) lastError_ = "D3D12 sprite draw has invalid command state";
            return;
        }
        commandList_->DrawInstanced(6, 1, 0, 0);
    }
    void draw_mesh(const MeshDraw& draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState || !draw.indexCount) {
            if (lastError_.empty()) lastError_ = "D3D12 mesh draw has invalid command state";
            return;
        }
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        if (vertex == resources_.end() || index == resources_.end() || !vertex->second.resource || !index->second.resource) {
            lastError_ = "D3D12 mesh draw buffers are invalid";
            return;
        }
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        vertexView.BufferLocation = vertex->second.resource->GetGPUVirtualAddress();
        vertexView.SizeInBytes = static_cast<UINT>(vertex->second.size);
        vertexView.StrideInBytes = vertex->second.stride;
        D3D12_INDEX_BUFFER_VIEW indexView{};
        indexView.BufferLocation = index->second.resource->GetGPUVirtualAddress();
        indexView.SizeInBytes = static_cast<UINT>(index->second.size);
        indexView.Format = DXGI_FORMAT_R32_UINT;
        commandList_->IASetVertexBuffers(0, 1, &vertexView);
        commandList_->IASetIndexBuffer(&indexView);
        commandList_->DrawIndexedInstanced(draw.indexCount, 1, 0, 0, 0);
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (!commandList_ || !frameActive_ || dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) {
            lastError_ = "D3D12 dispatch has invalid command state or zero group count";
            return false;
        }
        if (!boundPipeline_ || !boundPipeline_->pipeline.computeShader) {
            lastError_ = "D3D12 dispatch requires a compute shader pipeline";
            return false;
        }
        commandList_->Dispatch(dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
        ++stats_.dispatchCalls;
        return true;
    }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState ||
            !indexedDrawSignature_ || vertex == resources_.end() || index == resources_.end() ||
            arguments == resources_.end() || !vertex->second.resource || !index->second.resource ||
            !arguments->second.resource || draw.maxDrawCount == 0 || draw.stride < sizeof(std::uint32_t) * 5) {
            if (lastError_.empty()) lastError_ = "D3D12 indirect draw has invalid command state or buffer";
            return;
        }
        D3D12_VERTEX_BUFFER_VIEW vertexView{vertex->second.resource->GetGPUVirtualAddress(),
            static_cast<UINT>(vertex->second.size), vertex->second.stride};
        D3D12_INDEX_BUFFER_VIEW indexView{index->second.resource->GetGPUVirtualAddress(),
            static_cast<UINT>(index->second.size), DXGI_FORMAT_R32_UINT};
        commandList_->IASetVertexBuffers(0, 1, &vertexView);
        commandList_->IASetIndexBuffer(&indexView);
        commandList_->ExecuteIndirect(indexedDrawSignature_, draw.maxDrawCount, arguments->second.resource,
            draw.argumentOffset, nullptr, 0);
        ++stats_.indirectDrawCalls;
    }
    void end_frame() override {
        if (!commandList_ || !frameActive_) return;
        if (timestampQueryHeap_ && timestampReadback_ && timestampCursor_ > 0) {
            commandList_->ResolveQueryData(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP,
                0, timestampCursor_, timestampReadback_, 0);
            submittedTimestampCount_ = timestampCursor_;
        }
        if (!swapChain_) {
            const auto closeResult = commandList_->Close();
            if (FAILED(closeResult)) {
                lastError_ = "D3D12 off-screen command list close failed hr=" + std::to_string(static_cast<long long>(closeResult));
                capabilities_.deviceState = RenderDeviceState::Lost;
                frameActive_ = false;
                return;
            }
            ID3D12CommandList* lists[] = {commandList_};
            queue_->ExecuteCommandLists(1, lists);
            ++stats_.queueSubmissions;
            ++fenceValue_;
            const auto signalResult = queue_->Signal(fence_, fenceValue_);
            if (FAILED(signalResult)) {
                lastError_ = "D3D12 off-screen fence signal failed hr=" + std::to_string(static_cast<long long>(signalResult));
                capabilities_.deviceState = RenderDeviceState::Lost;
            }
            frameActive_ = false;
            return;
        }
        auto* buffer = backBuffers_[frameIndex_];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);
        commandList_->Close();
        ID3D12CommandList* lists[] = {commandList_};
        queue_->ExecuteCommandLists(1, lists);
        ++stats_.queueSubmissions;
        const auto presentResult = swapChain_->Present(1, 0);
        if (FAILED(presentResult)) {
            lastError_ = "D3D12 present failed hr=" + std::to_string(static_cast<long long>(presentResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState = reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
        }
        ++fenceValue_;
        const auto signalResult = queue_->Signal(fence_, fenceValue_);
        if (FAILED(signalResult)) {
            lastError_ = "D3D12 frame fence signal failed hr=" + std::to_string(static_cast<long long>(signalResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState = reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
        }
        frameActive_ = false;
    }
    void discard_frame() override {
        if (!frameActive_ || !commandList_ || !commandAllocator_) return;
        ++stats_.discardedFrames;
        commandList_->Close();
        commandAllocator_->Reset();
        frameActive_ = false;
        boundPipeline_ = nullptr;
        boundMaterial_ = nullptr;
    }
    void wait_idle() override {
        if (!queue_ || !fence_ || !fenceEvent_ || fenceValue_ == 0) return;
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }
    RenderStats stats() const noexcept override { return stats_; }
};
#endif

#if defined(SHINKOU_WITH_VULKAN)
class VulkanBackend final : public IRenderBackend {
    using DebugUtilsMessengerCallback = VkBool32 (VKAPI_PTR*)(VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*, void*);
    static VkCullModeFlags cull_mode(std::string_view mode) {
        if (mode == "front") return VK_CULL_MODE_FRONT_BIT;
        if (mode == "none") return VK_CULL_MODE_NONE;
        return VK_CULL_MODE_BACK_BIT;
    }
    static VkPolygonMode fill_mode(std::string_view mode) {
        return mode == "wireframe" ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }
    static VkPrimitiveTopology topology(std::string_view mode) {
        if (mode == "line") return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        if (mode == "point") return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        VkBuffer buffer{VK_NULL_HANDLE};
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        VkFramebuffer framebuffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        GpuAllocation gpuAllocation{};
        bool pooledMemory{false};
        bool hostVisible{false};
        bool depthStencil{false};
        std::size_t size{0};
        VkShaderModule shader{VK_NULL_HANDLE};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<ShaderBinding> shaderBindings;
        VkPipeline graphicsPipeline{VK_NULL_HANDLE};
        VkPipeline depthOnlyPipeline{VK_NULL_HANDLE};
        VkRenderPass pipelineRenderPass{VK_NULL_HANDLE};
        VkPipeline computePipeline{VK_NULL_HANDLE};
        VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
        std::vector<VkDescriptorSetLayout> descriptorLayouts;
        std::vector<VkDescriptorSet> descriptorSets;
        bool alias{false};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t mipLevels{1};
        std::uint32_t layers{1};
        ResourceUsage usage{ResourceUsage::Unknown};
        bool concurrentSharing{false};
        RenderQueue ownerQueue{RenderQueue::Graphics};
        std::uint32_t ownerBatch{UINT32_MAX};
        ~ResourceRecord() = default;
    };
    struct BufferState {
        ResourceUsage usage{ResourceUsage::Unknown};
    };
    struct BindlessTableRecord {
        VkDescriptorSet set{VK_NULL_HANDLE};
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        std::uint32_t capacity{0};
    };
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    PFN_vkSetDebugUtilsObjectNameEXT setDebugUtilsObjectName_{nullptr};
    PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel_{nullptr};
    PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel_{nullptr};
    bool validationEnabled_{false};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    VkQueue presentQueue_{VK_NULL_HANDLE};
    VkQueue computeQueue_{VK_NULL_HANDLE};
    VkQueue copyQueue_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkRenderPass renderPass_{VK_NULL_HANDLE};
    VkRenderPass depthOnlyRenderPass_{VK_NULL_HANDLE};
    VkRenderPass depthOnlyLoadRenderPass_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;
    std::unordered_map<std::uint64_t, VkFramebuffer> dynamicFramebuffers_;
    std::unordered_map<std::uint64_t, VkRenderPass> dynamicRenderPasses_;
    VkImage depthImage_{VK_NULL_HANDLE};
    VkDeviceMemory depthMemory_{VK_NULL_HANDLE};
    VkImageView depthView_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorPool> descriptorPools_;
    VkPipelineCache pipelineCache_{VK_NULL_HANDLE};
    VkSemaphore uploadTimeline_{VK_NULL_HANDLE};
    std::uint64_t uploadTimelineValue_{0};
    bool timelineSemaphoreSupported_{false};
    VkDescriptorSetLayout bindlessLayout_{VK_NULL_HANDLE};
    VkSampler defaultSampler_{VK_NULL_HANDLE};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    std::vector<bool> swapchainInitialized_;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    VkCommandPool computeCommandPool_{VK_NULL_HANDLE};
    VkCommandPool copyCommandPool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> commandBuffers_;
    VkCommandBuffer activeCommandBuffer_{VK_NULL_HANDLE};
    std::vector<VkSemaphore> batchSignals_;
    std::vector<VkFence> batchFences_;
    std::vector<std::uint32_t> currentWaitBatches_;
    std::uint32_t currentBatchIndex_{0};
    bool swapchainPrepared_{false};
    bool currentBatchWaitsForAcquire_{false};
    std::vector<VkCommandBuffer> transientBatchCommands_;
    VkCommandBuffer pendingCommandBuffer_{VK_NULL_HANDLE};
    VkQueue pendingQueue_{VK_NULL_HANDLE};
    VkCommandPool pendingCommandPool_{VK_NULL_HANDLE};
    RenderQueue pendingQueueKind_{RenderQueue::Graphics};
    std::uint32_t pendingBatchIndex_{UINT32_MAX};
    std::vector<std::uint32_t> pendingWaitBatches_;
    std::uint64_t pendingBatchUploadWaitValue_{0};
    bool pendingBatchWaitsForAcquire_{false};
    VkQueue activeQueue_{VK_NULL_HANDLE};
    RenderQueue activeQueueKind_{RenderQueue::Graphics};
    VkSemaphore imageAvailable_{VK_NULL_HANDLE};
    VkSemaphore renderFinished_{VK_NULL_HANDLE};
    VkFence frameFence_{VK_NULL_HANDLE};
    VkQueryPool timestampPool_{VK_NULL_HANDLE};
    std::uint32_t timestampCursor_{0};
    std::uint32_t submittedTimestampCount_{0};
    bool timestampPoolResetThisFrame_{false};
    float timestampPeriod_{0.0f};
    std::uint32_t imageIndex_{0};
    bool frameActive_{false};
    bool vsync_{true};
    bool renderPassActive_{false};
    VkRenderPass activeRenderPass_{VK_NULL_HANDLE};
    bool computeQueueActive_{false};
    bool frameSetupPending_{false};
    std::uint32_t currentRenderTarget_{0};
    std::uint32_t graphicsFamily_{0};
    std::uint32_t presentFamily_{0};
    std::uint32_t computeFamily_{UINT32_MAX};
    std::uint32_t copyFamily_{UINT32_MAX};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    GpuMemoryAllocator gpuMemoryAllocator_{};
    std::unordered_map<std::uint64_t, VkDeviceMemory> gpuMemoryBlocks_;
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    std::unordered_map<VkImage, ResourceUsage> imageUsages_;
    std::unordered_map<VkBuffer, ResourceUsage> bufferUsages_;
    std::vector<ResourceRecord> retiredResources_;
    struct RetiredDescriptorSets {
        VkDescriptorPool pool{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> sets;
    };
    std::vector<RetiredDescriptorSets> retiredDescriptorSets_;
    struct PendingUpload {
        VkFence fence{VK_NULL_HANDLE};
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkBuffer staging{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        std::uint64_t timelineValue{0};
    };
    std::vector<PendingUpload> pendingUploads_;
    bool uploadBatchActive_{false};
    std::uint64_t pendingUploadWaitValue_{0};
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord* boundPipeline_{nullptr};
    ResourceRecord* boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::string lastError_;
    bool descriptorIndexingEnabled_{false};
    std::unordered_map<std::uint32_t, std::string> debugNames_;
    std::unordered_map<std::uint32_t, BindlessTableRecord> bindlessTables_;
    std::vector<BindlessTableRecord> retiredBindlessTables_;
    std::uint32_t nextBindlessTable_{1};
    std::uint32_t boundBindlessTable_{0};
    std::vector<std::string> debugLabelStorage_;
    std::filesystem::path pipeline_cache_path() const {
        VkPhysicalDeviceProperties properties{};
        if (physicalDevice_ != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        const auto key = std::to_string(properties.vendorID) + "_" + std::to_string(properties.deviceID) + "_" +
            std::to_string(properties.driverVersion) + "_" + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) +
            "_" + std::to_string(VK_VERSION_MINOR(properties.apiVersion));
        if (const auto* directory = std::getenv("SHINKOU_PIPELINE_CACHE_DIR")) return std::filesystem::path(directory) / (key + ".vpc");
        return std::filesystem::temp_directory_path() / "shinkou_pipeline_cache" / (key + ".vpc");
    }
    std::vector<std::uint8_t> load_pipeline_cache() const {
        std::ifstream stream(pipeline_cache_path(), std::ios::binary | std::ios::ate);
        if (!stream) return {};
        const auto size = stream.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(1ull << 30)) return {};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(data.data()), size)) return {};
        if (data.size() < 32) return {};
        VkPhysicalDeviceProperties properties{};
        if (physicalDevice_ != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        const auto header = reinterpret_cast<const std::uint32_t*>(data.data());
        const auto headerVersion = header[0];
        const auto headerVendor = header[2];
        const auto headerDevice = header[3];
        if (headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            headerVendor != properties.vendorID || headerDevice != properties.deviceID) return {};
        return data;
    }
    void save_pipeline_cache() const {
        if (device_ == VK_NULL_HANDLE || pipelineCache_ == VK_NULL_HANDLE) return;
        std::size_t size = 0;
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
        std::vector<std::uint8_t> data(size);
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
        std::error_code error;
        const auto path = pipeline_cache_path();
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        const auto temporary = path.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
        stream.close();
        if (stream) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) std::filesystem::remove(temporary, error);
    }
    VkCommandBuffer command_buffer() const noexcept {
        return activeCommandBuffer_ != VK_NULL_HANDLE ? activeCommandBuffer_ :
            (commandBuffers_.empty() ? VK_NULL_HANDLE : commandBuffers_[imageIndex_]);
    }
    VkCommandBuffer recording_command() const noexcept { return command_buffer(); }
    VkQueue queue_for(RenderQueue queue) const noexcept {
        if (queue == RenderQueue::Compute && computeQueue_ != VK_NULL_HANDLE) return computeQueue_;
        if (queue == RenderQueue::Copy && copyQueue_ != VK_NULL_HANDLE) return copyQueue_;
        return graphicsQueue_;
    }
    VkCommandPool pool_for(RenderQueue queue) const noexcept {
        if (queue == RenderQueue::Compute && computeCommandPool_ != VK_NULL_HANDLE) return computeCommandPool_;
        if (queue == RenderQueue::Copy && copyCommandPool_ != VK_NULL_HANDLE) return copyCommandPool_;
        return commandPool_;
    }
    QueueFamilyMap queue_family_map() const noexcept {
        return {graphicsFamily_, computeFamily_, copyFamily_};
    }
    bool record_ownership_barrier(VkCommandBuffer command,
                                  const VulkanQueueFamilyOwnershipTransfer& transfer,
                                  const ResourceRecord& resource) {
        if (command == VK_NULL_HANDLE || transfer.srcQueueFamilyIndex == kIgnoredQueueFamily ||
            transfer.dstQueueFamilyIndex == kIgnoredQueueFamily ||
            transfer.srcQueueFamilyIndex == transfer.dstQueueFamilyIndex) {
            lastError_ = "Vulkan ownership barrier has invalid queue families";
            return false;
        }
        const auto srcStage = static_cast<VkPipelineStageFlags>(transfer.srcStageMask);
        const auto dstStage = static_cast<VkPipelineStageFlags>(transfer.dstStageMask);
        if (transfer.resourceType == VulkanBarrierResource::Buffer) {
            if (resource.buffer == VK_NULL_HANDLE) {
                lastError_ = "Vulkan ownership barrier has no buffer handle";
                return false;
            }
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = static_cast<VkAccessFlags>(transfer.srcAccessMask);
            barrier.dstAccessMask = static_cast<VkAccessFlags>(transfer.dstAccessMask);
            barrier.srcQueueFamilyIndex = transfer.srcQueueFamilyIndex;
            barrier.dstQueueFamilyIndex = transfer.dstQueueFamilyIndex;
            barrier.buffer = resource.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
            ++stats_.barriers;
            return true;
        }
        if (resource.image == VK_NULL_HANDLE) {
            lastError_ = "Vulkan ownership barrier has no image handle";
            return false;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = static_cast<VkAccessFlags>(transfer.srcAccessMask);
        barrier.dstAccessMask = static_cast<VkAccessFlags>(transfer.dstAccessMask);
        barrier.oldLayout = static_cast<VkImageLayout>(static_cast<std::int32_t>(transfer.oldLayout));
        barrier.newLayout = static_cast<VkImageLayout>(static_cast<std::int32_t>(transfer.newLayout));
        barrier.srcQueueFamilyIndex = transfer.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = transfer.dstQueueFamilyIndex;
        barrier.image = resource.image;
        barrier.subresourceRange.aspectMask = resource.depthStencil
            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        ++stats_.barriers;
        return true;
    }
    bool finalize_pending_queue_batch() {
        if (pendingCommandBuffer_ == VK_NULL_HANDLE) return true;
        const auto command = pendingCommandBuffer_;
        const auto queue = pendingQueue_;
        const auto batchIndex = pendingBatchIndex_;
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue command buffer finalization failed";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        if (pendingBatchWaitsForAcquire_ && imageAvailable_ != VK_NULL_HANDLE) {
            waits.push_back(imageAvailable_);
            stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        for (const auto waitBatch : pendingWaitBatches_) {
            if (waitBatch >= batchSignals_.size() || batchSignals_[waitBatch] == VK_NULL_HANDLE) {
                lastError_ = "Vulkan pending queue batch wait references an unsignaled batch";
                capabilities_.deviceState = RenderDeviceState::NeedsResize;
                pendingCommandBuffer_ = VK_NULL_HANDLE;
                pendingQueue_ = VK_NULL_HANDLE;
                return false;
            }
            waits.push_back(batchSignals_[waitBatch]);
            stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        const bool hasUploadWait = pendingBatchUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE;
        if (hasUploadWait) {
            waits.push_back(uploadTimeline_);
            stages.push_back(pendingQueueKind_ == RenderQueue::Copy ? VK_PIPELINE_STAGE_TRANSFER_BIT :
                (pendingQueueKind_ == RenderQueue::Compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
        }
        VkSemaphore signal = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &signal) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue signal semaphore creation failed";
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        if (batchSignals_.size() <= batchIndex) batchSignals_.resize(batchIndex + 1, VK_NULL_HANDLE);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        VkTimelineSemaphoreSubmitInfo timelineWait{};
        std::vector<std::uint64_t> waitValues;
        if (hasUploadWait) {
            waitValues.assign(waits.size(), 0);
            waitValues.back() = pendingBatchUploadWaitValue_;
            timelineWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineWait.waitSemaphoreValueCount = static_cast<std::uint32_t>(waitValues.size());
            timelineWait.pWaitSemaphoreValues = waitValues.data();
            submit.pNext = &timelineWait;
        }
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS ||
            vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue submission failed";
            capabilities_.deviceState = RenderDeviceState::Lost;
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
            vkDestroySemaphore(device_, signal, nullptr);
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        batchSignals_[batchIndex] = signal;
        batchFences_.push_back(fence);
        ++stats_.queueSubmissions;
        if (hasUploadWait) pendingBatchUploadWaitValue_ = 0;
        pendingCommandBuffer_ = VK_NULL_HANDLE;
        pendingQueue_ = VK_NULL_HANDLE;
        pendingCommandPool_ = VK_NULL_HANDLE;
        pendingBatchIndex_ = UINT32_MAX;
        pendingWaitBatches_.clear();
        pendingBatchWaitsForAcquire_ = false;
        return true;
    }
    bool record_ownership_transfer(ResourceHandle handle, ResourceRecord& resource,
                                   ResourceUsage before, ResourceUsage after) {
        if (resource.concurrentSharing || resource.ownerBatch == UINT32_MAX) return true;
        const auto families = queue_family_map();
        if (families.same_family(resource.ownerQueue, activeQueueKind_)) return true;
        const QueueSyncRequest request{handle, resource.ownerQueue, activeQueueKind_, before, after,
            resource.ownerBatch, currentBatchIndex_};
        const auto plan = VulkanQueueSyncPlanner::build({request}, families);
        std::string planError;
        if (!VulkanQueueSyncPlanner::validate(plan, &planError) || !plan.requiresOwnershipTransfer() ||
            plan.releaseBarriers.empty() || plan.acquireBarriers.empty()) {
            lastError_ = "Vulkan ownership plan is invalid: " + planError;
            return false;
        }
        if (pendingCommandBuffer_ == VK_NULL_HANDLE || pendingBatchIndex_ != resource.ownerBatch ||
            pendingQueueKind_ != resource.ownerQueue) {
            lastError_ = "Vulkan ownership release batch is no longer pending";
            return false;
        }
        if (std::find(currentWaitBatches_.begin(), currentWaitBatches_.end(), pendingBatchIndex_) == currentWaitBatches_.end()) {
            currentWaitBatches_.push_back(pendingBatchIndex_);
        }
        if (!record_ownership_barrier(pendingCommandBuffer_, plan.releaseBarriers.front(), resource)) return false;
        if (!finalize_pending_queue_batch()) return false;
        if (resource.image != VK_NULL_HANDLE && renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
        }
        if (!record_ownership_barrier(activeCommandBuffer_, plan.acquireBarriers.front(), resource)) return false;
        resource.ownerQueue = activeQueueKind_;
        resource.ownerBatch = currentBatchIndex_;
        return true;
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {
        if (userData) {
            auto* backend = static_cast<VulkanBackend*>(userData);
            ++backend->stats_.validationMessages;
            if (data && data->pMessage) {
                const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "error" :
                    ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? "warning" : "info");
                backend->stats_.validationDiagnostics.emplace_back(std::string(level) + ": " + data->pMessage);
            }
        }
        return VK_FALSE;
    }
    static VkResult create_debug_messenger(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT& info,
                                           VkDebugUtilsMessengerEXT* messenger) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        return create ? create(instance, &info, nullptr, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    static void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger, nullptr);
    }
    void clear_dynamic_framebuffers() {
        for (const auto& [key, framebuffer] : dynamicFramebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        dynamicFramebuffers_.clear();
    }
    void clear_dynamic_render_passes() {
        for (const auto& [key, renderPass] : dynamicRenderPasses_) {
            if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass, nullptr);
        }
        dynamicRenderPasses_.clear();
    }
    VkDescriptorPool create_descriptor_pool(std::uint32_t multiplier) {
        const std::uint32_t safeMultiplier = std::min<std::uint32_t>(multiplier, 64u);
        VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1024u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048u * safeMultiplier}
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
            (descriptorIndexingEnabled_ ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0);
        poolInfo.maxSets = 512u * safeMultiplier;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        descriptorPools_.push_back(pool);
        if (descriptorPool_ == VK_NULL_HANDLE) descriptorPool_ = pool;
        return pool;
    }
    bool allocate_descriptor_sets(std::uint32_t count, const VkDescriptorSetLayout* layouts,
                                  VkDescriptorSet* sets, VkDescriptorPool& allocatedPool, const void* pNext = nullptr) {
        if (count == 0 || !layouts || !sets) return false;
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.pNext = pNext;
        allocation.descriptorSetCount = count;
        allocation.pSetLayouts = layouts;
        for (const auto pool : descriptorPools_) {
            allocation.descriptorPool = pool;
            const auto result = vkAllocateDescriptorSets(device_, &allocation, sets);
            if (result == VK_SUCCESS) { allocatedPool = pool; return true; }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) break;
        }
        const auto multiplier = descriptorPools_.empty() ? 1u : (1u << std::min<std::size_t>(descriptorPools_.size(), 6u));
        const auto pool = create_descriptor_pool(multiplier);
        if (pool == VK_NULL_HANDLE) return false;
        allocation.descriptorPool = pool;
        if (vkAllocateDescriptorSets(device_, &allocation, sets) != VK_SUCCESS) return false;
        allocatedPool = pool;
        return true;
    }
    void release_completed_uploads(bool waitForAll = false) {
        for (auto it = pendingUploads_.begin(); it != pendingUploads_.end();) {
            if (waitForAll) vkWaitForFences(device_, 1, &it->fence, VK_TRUE, UINT64_MAX);
            else if (vkGetFenceStatus(device_, it->fence) != VK_SUCCESS) {
                ++it;
                continue;
            }
            if (it->command != VK_NULL_HANDLE) vkFreeCommandBuffers(device_, commandPool_, 1, &it->command);
            if (it->fence != VK_NULL_HANDLE) vkDestroyFence(device_, it->fence, nullptr);
            if (it->staging != VK_NULL_HANDLE) vkDestroyBuffer(device_, it->staging, nullptr);
            if (it->memory != VK_NULL_HANDLE) vkFreeMemory(device_, it->memory, nullptr);
            it = pendingUploads_.erase(it);
        }
    }
public:
    ~VulkanBackend() override {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        release_completed_uploads(true);
        save_pipeline_cache();
        while (!resources_.empty()) destroy_resource(ResourceHandle{resources_.begin()->first, resources_.begin()->second.kind});
        for (auto& resource : retiredResources_) release_resource(resource);
        retiredResources_.clear();
        for (auto& block : gpuMemoryBlocks_) if (block.second != VK_NULL_HANDLE) vkFreeMemory(device_, block.second, nullptr);
        gpuMemoryBlocks_.clear();
        for (auto& table : bindlessTables_) {
            if (table.second.set != VK_NULL_HANDLE && table.second.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.second.pool, 1, &table.second.set);
            }
        }
        bindlessTables_.clear();
        for (auto& table : retiredBindlessTables_) {
            if (table.set != VK_NULL_HANDLE && table.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.pool, 1, &table.set);
            }
        }
        retiredBindlessTables_.clear();
        for (auto& descriptorSets : retiredDescriptorSets_) {
            if (descriptorSets.pool != VK_NULL_HANDLE && !descriptorSets.sets.empty()) {
                vkFreeDescriptorSets(device_, descriptorSets.pool,
                    static_cast<std::uint32_t>(descriptorSets.sets.size()), descriptorSets.sets.data());
            }
        }
        retiredDescriptorSets_.clear();
        if (bindlessLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, bindlessLayout_, nullptr);
        if (defaultSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, defaultSampler_, nullptr);
        for (const auto pool : descriptorPools_) {
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool, nullptr);
        }
        descriptorPools_.clear();
        descriptorPool_ = VK_NULL_HANDLE;
        if (pipelineCache_ != VK_NULL_HANDLE) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        if (uploadTimeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, uploadTimeline_, nullptr);
        for (auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (depthOnlyRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, depthOnlyRenderPass_, nullptr);
        if (depthOnlyLoadRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, depthOnlyLoadRenderPass_, nullptr);
        clear_dynamic_framebuffers();
        clear_dynamic_render_passes();
        if (depthView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, depthImage_, nullptr);
        if (depthMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, depthMemory_, nullptr);
        if (frameFence_ != VK_NULL_HANDLE) vkDestroyFence(device_, frameFence_, nullptr);
        if (timestampPool_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_, timestampPool_, nullptr);
        if (imageAvailable_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, imageAvailable_, nullptr);
        if (renderFinished_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, renderFinished_, nullptr);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (computeCommandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, computeCommandPool_, nullptr);
        if (copyCommandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, copyCommandPool_, nullptr);
        for (auto semaphore : batchSignals_) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        for (auto fence : batchFences_) if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        for (auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        if (debugMessenger_ != VK_NULL_HANDLE) destroy_debug_messenger(instance_, debugMessenger_);
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    std::string last_error() const override { return lastError_; }
    void clear_error() override { lastError_.clear(); }
#if defined(SHINKOU_WITH_IMGUI)
    bool initialize_imgui() override {
        if (device_ == VK_NULL_HANDLE || renderPass_ == VK_NULL_HANDLE || !ImGui::GetCurrentContext()) return false;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_0;
        info.Instance = instance_;
        info.PhysicalDevice = physicalDevice_;
        info.Device = device_;
        info.QueueFamily = graphicsFamily_;
        info.Queue = graphicsQueue_;
        info.RenderPass = renderPass_;
        info.MinImageCount = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(swapchainImages_.size()));
        info.ImageCount = info.MinImageCount;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineCache = pipelineCache_;
        info.DescriptorPoolSize = 1000;
        info.MinAllocationSize = 1024 * 1024;
        return ImGui_ImplVulkan_Init(&info);
    }
    void shutdown_imgui() override {
        if (ImGui::GetCurrentContext()) ImGui_ImplVulkan_Shutdown();
    }
    void render_imgui(ImDrawData* drawData) override {
        if (!drawData || !frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        ImGui_ImplVulkan_NewFrame();
        set_render_target({});
        if (renderPassActive_) ImGui_ImplVulkan_RenderDrawData(drawData, activeCommandBuffer_);
    }
#endif
    bool initialize(const RenderBackendConfig& config) override {
        vsync_ = config.vsync;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ShinkouEngine";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "ShinkouEngine";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &app;
        std::vector<const char*> instanceExtensions;
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (config.nativeWindow) {
            instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            instanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        }
#endif
        std::uint32_t instanceExtensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
        std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, availableInstanceExtensions.data());
        const auto hasInstanceExtension = [&](const char* name) {
            return std::any_of(availableInstanceExtensions.begin(), availableInstanceExtensions.end(), [name](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        const bool validationLayerAvailable = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        validationEnabled_ = config.enableValidation && validationLayerAvailable &&
            hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (validationEnabled_) instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
        createInfo.ppEnabledExtensionNames = instanceExtensions.data();
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        if (validationEnabled_) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
        }
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (validationEnabled_) {
            debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = &debug_callback;
            debugCreateInfo.pUserData = this;
            createInfo.pNext = &debugCreateInfo;
        }
        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) return false;
        if (validationEnabled_ && create_debug_messenger(instance_, debugCreateInfo, &debugMessenger_) != VK_SUCCESS) {
            validationEnabled_ = false;
        }
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (config.nativeWindow) {
            VkWin32SurfaceCreateInfoKHR surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            surfaceInfo.hinstance = GetModuleHandleA(nullptr);
            surfaceInfo.hwnd = static_cast<HWND>(config.nativeWindow);
            if (vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) return false;
        }
#endif

        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        std::uint32_t bestGraphicsFamily = 0;
        std::uint32_t bestPresentFamily = 0;
        std::uint32_t bestComputeFamily = UINT32_MAX;
        std::uint32_t bestCopyFamily = UINT32_MAX;
        std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
        for (const auto device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            std::uint32_t graphicsFamily = UINT32_MAX;
            std::uint32_t presentFamily = surface_ == VK_NULL_HANDLE ? 0u : UINT32_MAX;
            std::uint32_t dedicatedComputeFamily = UINT32_MAX;
            std::uint32_t dedicatedCopyFamily = UINT32_MAX;
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if (graphicsFamily == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) graphicsFamily = i;
                if (dedicatedComputeFamily == UINT32_MAX &&
                    (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) dedicatedComputeFamily = i;
                if (dedicatedCopyFamily == UINT32_MAX &&
                    (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) dedicatedCopyFamily = i;
                if (surface_) {
                    VkBool32 supported = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &supported);
                    if (presentFamily == UINT32_MAX && supported) presentFamily = i;
                }
            }
            if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) continue;
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(device, &memoryProperties);
            std::uint64_t localMemory = 0;
            for (std::uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i) {
                if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    localMemory += memoryProperties.memoryHeaps[i].size;
                }
            }
            std::int64_t score = static_cast<std::int64_t>(localMemory / (256ull * 1024ull * 1024ull));
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100000;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10000;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score += 1000;
            if (graphicsFamily == presentFamily) score += 100;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score -= 1000000;
            if (score > bestScore) {
                bestScore = score;
                bestDevice = device;
                bestGraphicsFamily = graphicsFamily;
                bestPresentFamily = presentFamily;
                bestComputeFamily = dedicatedComputeFamily;
                bestCopyFamily = dedicatedCopyFamily;
            }
        }
        if (bestDevice == VK_NULL_HANDLE) return false;
        physicalDevice_ = bestDevice;
        graphicsFamily_ = bestGraphicsFamily;
        presentFamily_ = bestPresentFamily;
        computeFamily_ = bestComputeFamily;
        copyFamily_ = bestCopyFamily;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        const std::array<std::uint32_t, 4> requestedFamilies{
            graphicsFamily_, presentFamily_, computeFamily_, copyFamily_};
        for (const auto queueFamily : requestedFamilies) {
            if (queueFamily == UINT32_MAX) continue;
            bool duplicate = false;
            for (const auto& existing : queueInfos) duplicate |= existing.queueFamilyIndex == queueFamily;
            if (duplicate) continue;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = queueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            queueInfos.push_back(queueInfo);
        }
        VkPhysicalDeviceProperties physicalProperties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &physicalProperties);
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        deviceInfo.pQueueCreateInfos = queueInfos.data();
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, extensions.data());
        const auto hasExtension = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        const bool descriptorExtension = hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        VkPhysicalDeviceFeatures2 supportedFeatures{};
        supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceDescriptorIndexingFeatures supportedIndexing{};
        supportedIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        supportedFeatures.pNext = &supportedIndexing;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
        VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline{};
        supportedTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        supportedFeatures.pNext = &supportedTimeline;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
        const auto apiVersion = physicalProperties.apiVersion;
        const bool descriptorCore = VK_VERSION_MAJOR(apiVersion) > 1 ||
            (VK_VERSION_MAJOR(apiVersion) == 1 && VK_VERSION_MINOR(apiVersion) >= 2);
        const bool timelineCore = descriptorCore;
        const bool timelineExtension = hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        timelineSemaphoreSupported_ = (timelineCore || timelineExtension) && supportedTimeline.timelineSemaphore == VK_TRUE;
        descriptorIndexingEnabled_ = (descriptorExtension || descriptorCore) &&
            supportedIndexing.runtimeDescriptorArray && supportedIndexing.descriptorBindingPartiallyBound &&
            supportedIndexing.descriptorBindingVariableDescriptorCount &&
            supportedIndexing.descriptorBindingSampledImageUpdateAfterBind;
        std::vector<const char*> deviceExtensions;
        if (surface_) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (descriptorIndexingEnabled_ && descriptorExtension) deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        if (timelineSemaphoreSupported_ && timelineExtension) deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        VkPhysicalDeviceDescriptorIndexingFeatures enabledIndexing{};
        VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimeline{};
        if (descriptorIndexingEnabled_) {
            enabledIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            enabledIndexing.runtimeDescriptorArray = VK_TRUE;
            enabledIndexing.descriptorBindingPartiallyBound = VK_TRUE;
            enabledIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
            enabledIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            deviceInfo.pNext = &enabledIndexing;
        }
        if (timelineSemaphoreSupported_) {
            enabledTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
            enabledTimeline.timelineSemaphore = VK_TRUE;
            enabledTimeline.pNext = descriptorIndexingEnabled_ ? static_cast<void*>(&enabledIndexing) : nullptr;
            deviceInfo.pNext = &enabledTimeline;
        }
        if (vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) return false;
        setDebugUtilsObjectName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
        cmdBeginDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
        cmdEndDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
        vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
        if (computeFamily_ != UINT32_MAX) vkGetDeviceQueue(device_, computeFamily_, 0, &computeQueue_);
        if (copyFamily_ != UINT32_MAX) vkGetDeviceQueue(device_, copyFamily_, 0, &copyQueue_);
        timestampPeriod_ = physicalProperties.limits.timestampPeriod;
        if (physicalProperties.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 512;
            vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampPool_);
        }
        if (create_descriptor_pool(1) == VK_NULL_HANDLE) return false;
        VkPipelineCacheCreateInfo pipelineCacheInfo{};
        pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        auto pipelineCacheData = load_pipeline_cache();
        pipelineCacheInfo.initialDataSize = pipelineCacheData.size();
        pipelineCacheInfo.pInitialData = pipelineCacheData.empty() ? nullptr : pipelineCacheData.data();
        auto pipelineCacheResult = vkCreatePipelineCache(device_, &pipelineCacheInfo, nullptr, &pipelineCache_);
        if (pipelineCacheResult != VK_SUCCESS && !pipelineCacheData.empty()) {
            pipelineCacheData.clear();
            pipelineCacheInfo.initialDataSize = 0;
            pipelineCacheInfo.pInitialData = nullptr;
            pipelineCacheResult = vkCreatePipelineCache(device_, &pipelineCacheInfo, nullptr, &pipelineCache_);
        }
        if (pipelineCacheResult != VK_SUCCESS) return false;
        if (timelineSemaphoreSupported_) {
            VkSemaphoreTypeCreateInfo timelineInfo{};
            timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timelineInfo.initialValue = 0;
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphoreInfo.pNext = &timelineInfo;
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &uploadTimeline_) != VK_SUCCESS) {
                timelineSemaphoreSupported_ = false;
            }
        }
        if (descriptorIndexingEnabled_ && !create_bindless_layout()) {
            descriptorIndexingEnabled_ = false;
        }
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        if (vkCreateSampler(device_, &samplerInfo, nullptr, &defaultSampler_) != VK_SUCCESS) return false;

        if (surface_ && !create_swapchain(config.width, config.height)) return false;
        if (!surface_) {
            VkCommandPoolCreateInfo uploadPoolInfo{};
            uploadPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            uploadPoolInfo.queueFamilyIndex = graphicsFamily_;
            uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &commandPool_) != VK_SUCCESS) return false;
            if (computeFamily_ != UINT32_MAX && computeFamily_ != graphicsFamily_) {
                uploadPoolInfo.queueFamilyIndex = computeFamily_;
                if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &computeCommandPool_) != VK_SUCCESS) return false;
            }
            if (copyFamily_ != UINT32_MAX && copyFamily_ != graphicsFamily_ && copyFamily_ != computeFamily_) {
                uploadPoolInfo.queueFamilyIndex = copyFamily_;
                if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &copyCommandPool_) != VK_SUCCESS) return false;
            }
            commandBuffers_.resize(1);
            VkCommandBufferAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocation.commandPool = commandPool_;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &allocation, commandBuffers_.data()) != VK_SUCCESS) return false;
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_) != VK_SUCCESS) return false;
        }
        capabilities_.api = BackendApi::Vulkan;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
        capabilities_.supportsCompute = true;
        capabilities_.supportsBindless = descriptorIndexingEnabled_;
        capabilities_.supportsMultiDrawIndirect = true;
        capabilities_.supportsDedicatedComputeQueue = computeFamily_ != UINT32_MAX;
        capabilities_.supportsDedicatedCopyQueue = copyFamily_ != UINT32_MAX;
        capabilities_.supportsValidation = validationEnabled_;
        capabilities_.supportsGpuMemoryAllocator = true;
        capabilities_.supportsGpuMemoryAliasing = true;
        return true;
    }
    bool resize(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) {
            lastError_ = "Vulkan swapchain dimensions must be non-zero";
            return false;
        }
        if (!device_ || !surface_) return true;
        const auto idleResult = vkDeviceWaitIdle(device_);
        if (idleResult == VK_ERROR_DEVICE_LOST) {
            lastError_ = "Vulkan device lost while resizing swapchain";
            capabilities_.deviceState = RenderDeviceState::Lost;
            return false;
        }
        release_completed_uploads(true);
        clear_dynamic_framebuffers();
        clear_dynamic_render_passes();
        for (auto& [id, resource] : resources_) {
            if (resource.graphicsPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, resource.graphicsPipeline, nullptr);
                resource.graphicsPipeline = VK_NULL_HANDLE;
            }
            if (resource.depthOnlyPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, resource.depthOnlyPipeline, nullptr);
                resource.depthOnlyPipeline = VK_NULL_HANDLE;
            }
            if (resource.pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, resource.pipelineLayout, nullptr);
                resource.pipelineLayout = VK_NULL_HANDLE;
            }
            if (resource.descriptorLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, resource.descriptorLayout, nullptr);
                resource.descriptorLayout = VK_NULL_HANDLE;
            }
            for (const auto layout : resource.descriptorLayouts) {
                if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
            }
            resource.descriptorLayouts.clear();
            release_descriptor_sets(resource, false);
            resource.pipelineRenderPass = VK_NULL_HANDLE;
        }
        for (auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (renderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
        if (depthOnlyRenderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, depthOnlyRenderPass_, nullptr); depthOnlyRenderPass_ = VK_NULL_HANDLE; }
        if (depthOnlyLoadRenderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, depthOnlyLoadRenderPass_, nullptr); depthOnlyLoadRenderPass_ = VK_NULL_HANDLE; }
        if (depthView_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
        if (depthImage_ != VK_NULL_HANDLE) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
        if (depthMemory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE; }
        for (auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        swapchainViews_.clear();
        if (swapchain_ != VK_NULL_HANDLE) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
        if (frameFence_ != VK_NULL_HANDLE) { vkDestroyFence(device_, frameFence_, nullptr); frameFence_ = VK_NULL_HANDLE; }
        if (imageAvailable_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, imageAvailable_, nullptr); imageAvailable_ = VK_NULL_HANDLE; }
        if (renderFinished_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, renderFinished_, nullptr); renderFinished_ = VK_NULL_HANDLE; }
        if (commandPool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, commandPool_, nullptr); commandPool_ = VK_NULL_HANDLE; }
        commandBuffers_.clear();
        frameActive_ = false;
        const bool recreated = create_swapchain(width, height);
        if (!recreated) {
            lastError_ = "Vulkan swapchain recreation failed";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
        } else {
            capabilities_.deviceState = RenderDeviceState::Ready;
        }
        return recreated;
    }
    bool create_swapchain(std::uint32_t width, std::uint32_t height) {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities) != VK_SUCCESS) return false;
        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        if (formatCount == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
        VkSurfaceFormatKHR format = formats.front();
        for (const auto candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) { format = candidate; break; }
        }
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        swapchainFormat_ = format.format;
        swapchainExtent_ = extent;
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0) info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const std::uint32_t families[] = {graphicsFamily_, presentFamily_};
        if (graphicsFamily_ != presentFamily_) {
            info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        } else {
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_) {
            std::uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
            std::vector<VkPresentModeKHR> presentModes(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());
            if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end()) {
                info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            } else if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.end()) {
                info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) return false;
        std::uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
        swapchainViews_.resize(imageCount);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]) != VK_SUCCESS) return false;
        }
        VkImageCreateInfo depthInfo{};
        depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthInfo.imageType = VK_IMAGE_TYPE_2D;
        depthInfo.format = VK_FORMAT_D24_UNORM_S8_UINT;
        depthInfo.extent = {extent.width, extent.height, 1};
        depthInfo.mipLevels = 1;
        depthInfo.arrayLayers = 1;
        depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vkCreateImage(device_, &depthInfo, nullptr, &depthImage_) != VK_SUCCESS) return false;
        VkMemoryRequirements depthRequirements{};
        vkGetImageMemoryRequirements(device_, depthImage_, &depthRequirements);
        const auto depthMemoryType = find_memory_type(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (depthMemoryType == UINT32_MAX) return false;
        VkMemoryAllocateInfo depthAllocation{};
        depthAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depthAllocation.allocationSize = depthRequirements.size;
        depthAllocation.memoryTypeIndex = depthMemoryType;
        if (vkAllocateMemory(device_, &depthAllocation, nullptr, &depthMemory_) != VK_SUCCESS) return false;
        vkBindImageMemory(device_, depthImage_, depthMemory_, 0);
        VkImageViewCreateInfo depthViewInfo{};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = depthImage_;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = depthInfo.format;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &depthViewInfo, nullptr, &depthView_) != VK_SUCCESS) return false;
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapchainFormat_;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = depthInfo.format;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        const VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 2;
        renderPassInfo.pAttachments = attachments;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) return false;
        VkAttachmentDescription depthOnlyAttachment = depthAttachment;
        VkAttachmentReference depthOnlyReference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription depthOnlySubpass{};
        depthOnlySubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        depthOnlySubpass.pDepthStencilAttachment = &depthOnlyReference;
        VkRenderPassCreateInfo depthOnlyInfo{};
        depthOnlyInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        depthOnlyInfo.attachmentCount = 1;
        depthOnlyInfo.pAttachments = &depthOnlyAttachment;
        depthOnlyInfo.subpassCount = 1;
        depthOnlyInfo.pSubpasses = &depthOnlySubpass;
        if (vkCreateRenderPass(device_, &depthOnlyInfo, nullptr, &depthOnlyRenderPass_) != VK_SUCCESS) return false;
        VkAttachmentDescription depthLoadAttachment = depthOnlyAttachment;
        depthLoadAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthLoadAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthLoadAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo depthLoadInfo = depthOnlyInfo;
        depthLoadInfo.pAttachments = &depthLoadAttachment;
        if (vkCreateRenderPass(device_, &depthLoadInfo, nullptr, &depthOnlyLoadRenderPass_) != VK_SUCCESS) return false;
        framebuffers_.resize(imageCount);
        swapchainInitialized_.assign(imageCount, false);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass_;
            const VkImageView framebufferAttachments[] = {swapchainViews_[i], depthView_};
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = framebufferAttachments;
            framebufferInfo.width = swapchainExtent_.width;
            framebufferInfo.height = swapchainExtent_.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
        }
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = graphicsFamily_;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) return false;
        if (computeFamily_ != UINT32_MAX && computeFamily_ != graphicsFamily_) {
            poolInfo.queueFamilyIndex = computeFamily_;
            if (vkCreateCommandPool(device_, &poolInfo, nullptr, &computeCommandPool_) != VK_SUCCESS) return false;
        }
        if (copyFamily_ != UINT32_MAX && copyFamily_ != graphicsFamily_ && copyFamily_ != computeFamily_) {
            poolInfo.queueFamilyIndex = copyFamily_;
            if (vkCreateCommandPool(device_, &poolInfo, nullptr, &copyCommandPool_) != VK_SUCCESS) return false;
        }
        commandBuffers_.resize(imageCount);
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = imageCount;
        if (vkAllocateCommandBuffers(device_, &allocation, commandBuffers_.data()) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_) == VK_SUCCESS;
    }
    std::uint32_t find_memory_type(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const {
        for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (memoryProperties_.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        return UINT32_MAX;
    }
    VkDeviceMemory ensure_gpu_memory_block(const GpuAllocation& allocation) {
        if (!allocation) return VK_NULL_HANDLE;
        const auto existing = gpuMemoryBlocks_.find(allocation.blockId);
        if (existing != gpuMemoryBlocks_.end()) return existing->second;
        const auto capacity = gpuMemoryAllocator_.block_capacity(allocation.blockId);
        if (capacity == 0 || allocation.memoryType >= memoryProperties_.memoryTypeCount) return VK_NULL_HANDLE;
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = capacity;
        info.memoryTypeIndex = allocation.memoryType;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const auto result = vkAllocateMemory(device_, &info, nullptr, &memory);
        if (result != VK_SUCCESS || memory == VK_NULL_HANDLE) {
            lastError_ = "Vulkan pooled device memory allocation failed result=" +
                std::to_string(static_cast<int>(result));
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device_, memory, nullptr);
            return VK_NULL_HANDLE;
        }
        gpuMemoryBlocks_.emplace(allocation.blockId, memory);
        return memory;
    }
    bool allocate_resource_memory(const VkMemoryRequirements& requirements,
                                  VkMemoryPropertyFlags properties, VkDeviceMemory& memory,
                                  GpuAllocation& allocation, bool& pooled) {
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, properties);
        if (memoryType == UINT32_MAX) {
            lastError_ = "Vulkan resource has no compatible memory type";
            return false;
        }
        allocation = gpuMemoryAllocator_.allocate(static_cast<std::size_t>(requirements.size),
            static_cast<std::size_t>(requirements.alignment), memoryType, GpuMemoryLifetime::Persistent);
        if (allocation) {
            memory = ensure_gpu_memory_block(allocation);
            if (memory != VK_NULL_HANDLE) {
                pooled = true;
                return true;
            }
            gpuMemoryAllocator_.release(allocation);
            allocation = {};
        }
        // Keep the dedicated path as a portable fallback for small heaps,
        // unusual device limits, and drivers that reject the pooled block.
        lastError_.clear();
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memoryType;
        const auto result = vkAllocateMemory(device_, &info, nullptr, &memory);
        if (result != VK_SUCCESS || memory == VK_NULL_HANDLE) {
            lastError_ = "Vulkan device memory allocation failed result=" +
                std::to_string(static_cast<int>(result));
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device_, memory, nullptr);
            return false;
        }
        pooled = false;
        return true;
    }
    void release_gpu_allocation(ResourceRecord& resource) {
        if (resource.pooledMemory && resource.gpuAllocation) {
            gpuMemoryAllocator_.release(resource.gpuAllocation);
        }
        resource.gpuAllocation = {};
        resource.pooledMemory = false;
    }
    static VkFormat format_of(std::string_view format) {
        if (format == "bgra8") return VK_FORMAT_B8G8R8A8_UNORM;
        if (format == "rgba16f") return VK_FORMAT_R16G16B16A16_SFLOAT;
        if (format == "r32f") return VK_FORMAT_R32_SFLOAT;
        if (format == "d24s8") return VK_FORMAT_D24_UNORM_S8_UINT;
        if (format == "d32f") return VK_FORMAT_D32_SFLOAT;
        if (format == "bc1") return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (format == "bc3") return VK_FORMAT_BC3_UNORM_BLOCK;
        if (format == "bc5") return VK_FORMAT_BC5_UNORM_BLOCK;
        if (format == "bc6h") return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        if (format == "bc7") return VK_FORMAT_BC7_UNORM_BLOCK;
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    VkRenderPass get_or_create_color_render_pass(const std::vector<VkFormat>& colorFormats, VkFormat depthFormat,
                                                 bool clearAttachments) {
        std::uint64_t key = clearAttachments ? 1ull : 0ull;
        for (const auto format : colorFormats) key = key * 257ull + static_cast<std::uint64_t>(format);
        key = key * 257ull + static_cast<std::uint64_t>(depthFormat);
        const auto existing = dynamicRenderPasses_.find(key);
        if (existing != dynamicRenderPasses_.end()) return existing->second;
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorReferences;
        const bool depth = depthFormat != VK_FORMAT_UNDEFINED;
        attachments.reserve(colorFormats.size() + (depth ? 1 : 0));
        colorReferences.reserve(colorFormats.size());
        for (std::size_t index = 0; index < colorFormats.size(); ++index) {
            VkAttachmentDescription attachment{};
            attachment.format = colorFormats[index];
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);
            colorReferences.push_back({static_cast<std::uint32_t>(index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        }
        VkAttachmentReference depthReference{};
        if (depth) {
            VkAttachmentDescription attachment{};
            attachment.format = depthFormat;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.stencilLoadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = clearAttachments ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);
            depthReference = {static_cast<std::uint32_t>(colorFormats.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorReferences.size());
        subpass.pColorAttachments = colorReferences.data();
        subpass.pDepthStencilAttachment = depth ? &depthReference : nullptr;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        if (vkCreateRenderPass(device_, &info, nullptr, &renderPass) != VK_SUCCESS) return VK_NULL_HANDLE;
        dynamicRenderPasses_.emplace(key, renderPass);
        return renderPass;
    }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "Vulkan resource handle and description kind do not match";
            return false;
        }
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        std::visit([&](const auto& desc) {
            using T = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<T, TextureDesc>) {
                TexturePhysicalImagePlan plan;
                if (!prepare_backend_texture(desc, BackendApi::Vulkan, plan, lastError_)) return;
                const VkFormat format = format_of(texture_format_name(plan.format));
                const bool depth = texture_format_is_depth(plan.format);
                record.depthStencil = depth;
                record.format = format;
                record.width = plan.width;
                record.height = plan.height;
                record.mipLevels = plan.mipLevels;
                record.layers = plan.physicalArrayLayers;
                VkImageCreateInfo imageInfo{};
                imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.format = format;
                imageInfo.extent = {plan.width, plan.height, 1};
                imageInfo.mipLevels = plan.mipLevels;
                imageInfo.arrayLayers = plan.physicalArrayLayers;
                switch (plan.sampleCount) {
                case 1: imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; break;
                case 2: imageInfo.samples = VK_SAMPLE_COUNT_2_BIT; break;
                case 4: imageInfo.samples = VK_SAMPLE_COUNT_4_BIT; break;
                case 8: imageInfo.samples = VK_SAMPLE_COUNT_8_BIT; break;
                default: lastError_ = "Vulkan texture sample count is not representable"; return;
                }
                record.samples = imageInfo.samples;
                imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                imageInfo.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (desc.renderTarget) imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (desc.storage) imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                const std::array<std::uint32_t, 3> resourceFamilies{graphicsFamily_, computeFamily_, copyFamily_};
                std::vector<std::uint32_t> sharingFamilies;
                for (const auto family : resourceFamilies) {
                    if (family == UINT32_MAX || std::find(sharingFamilies.begin(), sharingFamilies.end(), family) != sharingFamilies.end()) continue;
                    sharingFamilies.push_back(family);
                }
                record.concurrentSharing = sharingFamilies.size() > 1;
                if (record.concurrentSharing) {
                    imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    imageInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(sharingFamilies.size());
                    imageInfo.pQueueFamilyIndices = sharingFamilies.data();
                }
                if (vkCreateImage(device_, &imageInfo, nullptr, &record.image) != VK_SUCCESS) return;
                VkMemoryRequirements requirements{};
                vkGetImageMemoryRequirements(device_, record.image, &requirements);
                if (!allocate_resource_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        record.memory, record.gpuAllocation, record.pooledMemory)) {
                    vkDestroyImage(device_, record.image, nullptr);
                    record.image = VK_NULL_HANDLE;
                    return;
                }
                if (vkBindImageMemory(device_, record.image, record.memory,
                        record.pooledMemory ? record.gpuAllocation.offset : 0) != VK_SUCCESS) {
                    const auto wasPooled = record.pooledMemory;
                    if (wasPooled) release_gpu_allocation(record);
                    if (!wasPooled && record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
                    record.memory = VK_NULL_HANDLE;
                    vkDestroyImage(device_, record.image, nullptr);
                    record.image = VK_NULL_HANDLE;
                    lastError_ = "Vulkan image memory binding failed";
                    return;
                }
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = record.image;
                viewInfo.viewType = plan.physicalArrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = format;
                viewInfo.subresourceRange.aspectMask = depth
                    ? (plan.format == TextureFormat::D24UnormS8Uint
                        ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.levelCount = plan.mipLevels;
                viewInfo.subresourceRange.layerCount = plan.physicalArrayLayers;
                if (vkCreateImageView(device_, &viewInfo, nullptr, &record.view) != VK_SUCCESS) {
                    lastError_ = "Vulkan texture image view creation failed";
                    return;
                }
                // Off-screen color targets use the dynamic attachment path at bind time.
                // The static framebuffer is reserved for the swapchain's actual format.
            } else if constexpr (std::is_same_v<T, BufferDesc>) {
                record.size = desc.size;
                VkBufferCreateInfo bufferInfo{};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = std::max<std::size_t>(desc.size, 1);
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                if (desc.vertexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                if (desc.indexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                if (desc.indirectBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                if (!desc.vertexBuffer && !desc.indexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                const std::array<std::uint32_t, 3> resourceFamilies{graphicsFamily_, computeFamily_, copyFamily_};
                std::vector<std::uint32_t> sharingFamilies;
                for (const auto family : resourceFamilies) {
                    if (family == UINT32_MAX || std::find(sharingFamilies.begin(), sharingFamilies.end(), family) != sharingFamilies.end()) continue;
                    sharingFamilies.push_back(family);
                }
                record.concurrentSharing = sharingFamilies.size() > 1;
                if (record.concurrentSharing) {
                    bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    bufferInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(sharingFamilies.size());
                    bufferInfo.pQueueFamilyIndices = sharingFamilies.data();
                }
                if (vkCreateBuffer(device_, &bufferInfo, nullptr, &record.buffer) != VK_SUCCESS) return;
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device_, record.buffer, &requirements);
                record.hostVisible = false;
                if (!allocate_resource_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        record.memory, record.gpuAllocation, record.pooledMemory)) {
                    vkDestroyBuffer(device_, record.buffer, nullptr);
                    record.buffer = VK_NULL_HANDLE;
                    return;
                }
                if (vkBindBufferMemory(device_, record.buffer, record.memory,
                        record.pooledMemory ? record.gpuAllocation.offset : 0) != VK_SUCCESS) {
                    const auto wasPooled = record.pooledMemory;
                    if (wasPooled) release_gpu_allocation(record);
                    if (!wasPooled && record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
                    record.memory = VK_NULL_HANDLE;
                    vkDestroyBuffer(device_, record.buffer, nullptr);
                    record.buffer = VK_NULL_HANDLE;
                    lastError_ = "Vulkan buffer memory binding failed";
                    return;
                }
            } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = desc.filter == "nearest" ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                samplerInfo.minFilter = desc.filter == "nearest" ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                samplerInfo.mipmapMode = desc.filter == "nearest" ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerInfo.addressModeU = desc.addressU == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeV = desc.addressV == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeW = desc.addressW == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.anisotropyEnable = desc.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
                samplerInfo.maxAnisotropy = std::max(1.0f, desc.maxAnisotropy);
                samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
                samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
                vkCreateSampler(device_, &samplerInfo, nullptr, &record.sampler);
            } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                const auto compiled = shaderCompiler_->compile(desc, BackendApi::Vulkan);
                if (!compiled.valid || compiled.bytecode.size() % sizeof(std::uint32_t) != 0) {
                    lastError_ = compiled.diagnostics.empty() ? "invalid Vulkan SPIR-V bytecode" : compiled.diagnostics;
                    return;
                }
                record.shaderBindings = compiled.bindings;
                if (!validate_backend_shader_layout(record.shaderBindings, lastError_)) return;
                VkShaderModuleCreateInfo shaderInfo{};
                shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                shaderInfo.codeSize = compiled.bytecode.size();
                shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(compiled.bytecode.data());
                vkCreateShaderModule(device_, &shaderInfo, nullptr, &record.shader);
            } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                record.pipeline = desc;
            } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                record.material = desc;
            }
        }, description);
        resources_.emplace(handle.id, std::move(record));
        if (const auto inserted = resources_.find(handle.id); inserted != resources_.end() && inserted->second.buffer != VK_NULL_HANDLE) {
            bufferUsages_[inserted->second.buffer] = ResourceUsage::Unknown;
        }
        ++stats_.resourceCreates;
        if (const auto* buffer = std::get_if<BufferDesc>(&description); buffer && !buffer->initialData.empty()) {
            if (!update_buffer({handle, 0, buffer->initialData})) lastError_ = "Vulkan initial buffer upload failed";
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture && !texture->initialData.empty()) {
            const auto bytesPerPixel = texture->format == "rgba16f" ? 8u : 4u;
            if (!update_texture({handle, 0, 0, texture->width, texture->height,
                static_cast<std::size_t>(texture->width) * bytesPerPixel, texture->initialData})) {
                lastError_ = "Vulkan initial texture upload failed";
            }
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture) {
            for (const auto& subresource : texture->initialSubresources) {
                if (!update_texture({handle, subresource.mipLevel, subresource.layer, subresource.width,
                    subresource.height, subresource.rowPitch, subresource.data})) {
                    lastError_ = "Vulkan initial texture subresource upload failed";
                    return false;
                }
            }
            const auto hasExplicitMip = std::any_of(texture->initialSubresources.begin(), texture->initialSubresources.end(),
                [](const TextureDesc::SubresourceData& subresource) { return subresource.mipLevel > 0; });
            if (texture->generateMips && texture->mipLevels > 1 && !hasExplicitMip &&
                (!texture->initialData.empty() || !texture->initialSubresources.empty()) && !generate_mips(handle)) {
                lastError_ = "Vulkan initial texture mip generation failed";
                return false;
            }
        }
        const auto created = resources_.find(handle.id);
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil) &&
                (!created->second.image || !created->second.memory)) ||
            (handle.kind == ResourceKind::Buffer && (!created->second.buffer || !created->second.memory))) {
            if (lastError_.empty()) lastError_ = "Vulkan resource creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Sampler && !created->second.sampler) {
            if (lastError_.empty()) lastError_ = "Vulkan sampler creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Shader && !created->second.shader) {
            if (lastError_.empty()) lastError_ = "Vulkan shader module creation failed";
            return false;
        }
        return true;
    }
    void release_descriptor_sets(ResourceRecord& resource, bool defer = true) {
        if (resource.descriptorPool == VK_NULL_HANDLE || resource.descriptorSets.empty()) return;
        std::vector<VkDescriptorSet> sets;
        for (const auto set : resource.descriptorSets) if (set != VK_NULL_HANDLE) sets.push_back(set);
        if (!sets.empty()) {
            if (defer) retiredDescriptorSets_.push_back({resource.descriptorPool, std::move(sets)});
            else vkFreeDescriptorSets(device_, resource.descriptorPool,
                static_cast<std::uint32_t>(sets.size()), sets.data());
        }
        resource.descriptorSets.clear();
        resource.descriptorSet = VK_NULL_HANDLE;
        resource.descriptorPool = VK_NULL_HANDLE;
    }
    void release_resource(ResourceRecord& resource) {
        if (resource.alias) {
            resource = {};
            return;
        }
        release_descriptor_sets(resource, false);
        if (resource.shader != VK_NULL_HANDLE) vkDestroyShaderModule(device_, resource.shader, nullptr);
        if (resource.graphicsPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.graphicsPipeline, nullptr);
        if (resource.depthOnlyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.depthOnlyPipeline, nullptr);
        if (resource.computePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.computePipeline, nullptr);
        if (resource.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, resource.pipelineLayout, nullptr);
        if (resource.descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, resource.descriptorLayout, nullptr);
        for (const auto layout : resource.descriptorLayouts) {
            if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }
        if (resource.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, resource.framebuffer, nullptr);
        if (resource.view != VK_NULL_HANDLE) vkDestroyImageView(device_, resource.view, nullptr);
        if (resource.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, resource.sampler, nullptr);
        if (resource.image != VK_NULL_HANDLE) {
            imageUsages_.erase(resource.image);
            vkDestroyImage(device_, resource.image, nullptr);
        }
        if (resource.buffer != VK_NULL_HANDLE) {
            bufferUsages_.erase(resource.buffer);
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        }
        const auto wasPooled = resource.pooledMemory;
        if (wasPooled) release_gpu_allocation(resource);
        if (!wasPooled && resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
        resource.memory = VK_NULL_HANDLE;
        resource = {};
    }
    void destroy_resource(ResourceHandle handle) override {
        retire_resource(handle);
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() || source->second.kind != physical.kind ||
            source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D && source->second.kind != ResourceKind::Buffer) ||
            ((source->second.kind == ResourceKind::Texture2D && source->second.image == VK_NULL_HANDLE) ||
             (source->second.kind == ResourceKind::Buffer && source->second.buffer == VK_NULL_HANDLE))) return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.buffer = source->second.buffer;
        alias.image = source->second.image;
        alias.view = source->second.view;
        alias.framebuffer = source->second.framebuffer;
        alias.memory = source->second.memory;
        alias.gpuAllocation = source->second.gpuAllocation;
        alias.pooledMemory = source->second.pooledMemory;
        alias.depthStencil = source->second.depthStencil;
        alias.hostVisible = source->second.hostVisible;
        alias.size = source->second.size;
        alias.format = source->second.format;
        alias.width = source->second.width;
        alias.height = source->second.height;
        alias.mipLevels = source->second.mipLevels;
        alias.layers = source->second.layers;
        alias.usage = source->second.usage;
        alias.concurrentSharing = source->second.concurrentSharing;
        alias.ownerQueue = source->second.ownerQueue;
        alias.ownerBatch = source->second.ownerBatch;
        alias.alias = true;
        resources_.emplace(logical.id, std::move(alias));
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() || it->second.kind != ResourceKind::Buffer ||
            !it->second.buffer || update.data.empty() || update.offset + update.data.size() > it->second.size ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan buffer handle or update state is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.hostVisible) {
            void* mapped = nullptr;
            if (vkMapMemory(device_, resource.memory, update.offset, update.data.size(), 0, &mapped) != VK_SUCCESS) return false;
            std::memcpy(mapped, update.data.data(), update.data.size());
            vkUnmapMemory(device_, resource.memory);
            return true;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = update.data.size();
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX) {
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device_, &allocation, nullptr, &stagingMemory) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        vkBindBufferMemory(device_, staging, stagingMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, update.data.size(), 0, &mapped) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        vkUnmapMemory(device_, stagingMemory);
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commandPool_;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandAllocation, &command) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        const auto previous = bufferUsages_.count(resource.buffer) ? bufferUsages_[resource.buffer] : ResourceUsage::Unknown;
        VkBufferMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        before.srcAccessMask = buffer_access_of(previous);
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.buffer = resource.buffer;
        before.offset = update.offset;
        before.size = update.data.size();
        vkCmdPipelineBarrier(command, buffer_stage_of(previous), VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &before, 0, nullptr);
        VkBufferCopy copy{0, 0, update.data.size()};
        vkCmdCopyBuffer(command, staging, resource.buffer, 1, &copy);
        VkBufferMemoryBarrier after = before;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = buffer_access_of(previous);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, buffer_stage_of(previous),
            0, 0, nullptr, 1, &after, 0, nullptr);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkFence uploadFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        const std::uint64_t signalValue = uploadTimeline_ != VK_NULL_HANDLE ? ++uploadTimelineValue_ : 0;
        if (uploadTimeline_ != VK_NULL_HANDLE) {
            timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineSubmit.signalSemaphoreValueCount = 1;
            timelineSubmit.pSignalSemaphoreValues = &signalValue;
            submit.pNext = &timelineSubmit;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &uploadTimeline_;
        }
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, uploadFence) == VK_SUCCESS;
        if (!submitted) {
            vkDestroyFence(device_, uploadFence, nullptr);
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        if (uploadBatchActive_) {
            pendingUploads_.push_back({uploadFence, command, staging, stagingMemory, signalValue});
            pendingUploadWaitValue_ = std::max(pendingUploadWaitValue_, signalValue);
            bufferUsages_[resource.buffer] = previous;
            return true;
        }
        vkWaitForFences(device_, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, uploadFence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        bufferUsages_[resource.buffer] = previous;
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            it->second.image == VK_NULL_HANDLE || update.data.empty() || update.width == 0 || update.height == 0 ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan texture handle or update state is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.depthStencil || update.mipLevel >= resource.mipLevels || update.layer >= resource.layers) return false;
        const auto mipWidth = std::max(1u, (resource.width >> update.mipLevel));
        const auto mipHeight = std::max(1u, (resource.height >> update.mipLevel));
        const auto bytesPerPixel = resource.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
        const auto sourcePitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * bytesPerPixel : update.rowPitch;
        if (update.x > mipWidth || update.y > mipHeight || update.width > mipWidth - update.x ||
            update.height > mipHeight - update.y || sourcePitch < static_cast<std::size_t>(update.width) * bytesPerPixel ||
            sourcePitch * update.height > update.data.size()) return false;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = update.data.size();
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX) { vkDestroyBuffer(device_, staging, nullptr); return false; }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device_, &allocation, nullptr, &stagingMemory) != VK_SUCCESS) { vkDestroyBuffer(device_, staging, nullptr); return false; }
        vkBindBufferMemory(device_, staging, stagingMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, update.data.size(), 0, &mapped) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr); vkDestroyBuffer(device_, staging, nullptr); return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        vkUnmapMemory(device_, stagingMemory);
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commandPool_;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandAllocation, &command) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr); vkDestroyBuffer(device_, staging, nullptr); return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(command, &begin);
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        const auto usageIt = imageUsages_.find(resource.image);
        const auto previousUsage = usageIt == imageUsages_.end() ? ResourceUsage::Unknown : usageIt->second;
        toTransfer.oldLayout = previousUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previousUsage);
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcAccessMask = access_of(previousUsage);
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = resource.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = update.mipLevel;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.baseArrayLayer = update.layer;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = static_cast<std::uint32_t>(sourcePitch / bytesPerPixel);
        copy.bufferImageHeight = update.height;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = update.mipLevel;
        copy.imageSubresource.baseArrayLayer = update.layer;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {static_cast<std::int32_t>(update.x), static_cast<std::int32_t>(update.y), 0};
        copy.imageExtent = {update.width, update.height, 1};
        vkCmdCopyBufferToImage(command, staging, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier toShader = toTransfer;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
        vkEndCommandBuffer(command);
        VkFence uploadFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        const std::uint64_t signalValue = uploadTimeline_ != VK_NULL_HANDLE ? ++uploadTimelineValue_ : 0;
        if (uploadTimeline_ != VK_NULL_HANDLE) {
            timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineSubmit.signalSemaphoreValueCount = 1;
            timelineSubmit.pSignalSemaphoreValues = &signalValue;
            submit.pNext = &timelineSubmit;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &uploadTimeline_;
        }
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, uploadFence) == VK_SUCCESS;
        if (!submitted) {
            vkDestroyFence(device_, uploadFence, nullptr);
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        if (uploadBatchActive_) {
            pendingUploads_.push_back({uploadFence, command, staging, stagingMemory, signalValue});
            pendingUploadWaitValue_ = std::max(pendingUploadWaitValue_, signalValue);
            imageUsages_[resource.image] = ResourceUsage::ShaderRead;
            resource.usage = ResourceUsage::ShaderRead;
            return true;
        }
        vkWaitForFences(device_, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, uploadFence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        imageUsages_[resource.image] = ResourceUsage::ShaderRead;
        resource.usage = ResourceUsage::ShaderRead;
        return true;
    }
    bool resolve_texture(ResourceHandle destination, ResourceHandle source) override {
        const auto destinationIt = resources_.find(destination.id);
        const auto sourceIt = resources_.find(source.id);
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE ||
            destination.kind != ResourceKind::Texture2D || source.kind != ResourceKind::Texture2D ||
            destination.id == source.id || destinationIt == resources_.end() || sourceIt == resources_.end() ||
            destinationIt->second.kind != ResourceKind::Texture2D || sourceIt->second.kind != ResourceKind::Texture2D ||
            destinationIt->second.image == VK_NULL_HANDLE || sourceIt->second.image == VK_NULL_HANDLE) {
            lastError_ = "Vulkan resolve requires an active command buffer and two valid texture resources";
            return false;
        }
        const auto& destinationResource = destinationIt->second;
        const auto& sourceResource = sourceIt->second;
        if (sourceResource.samples == VK_SAMPLE_COUNT_1_BIT || destinationResource.samples != VK_SAMPLE_COUNT_1_BIT ||
            sourceResource.width != destinationResource.width || sourceResource.height != destinationResource.height ||
            sourceResource.layers != destinationResource.layers || sourceResource.mipLevels != destinationResource.mipLevels ||
            sourceResource.format != destinationResource.format || sourceResource.depthStencil || destinationResource.depthStencil) {
            lastError_ = "Vulkan resolve resources must match and use a color MSAA source";
            return false;
        }
        if (renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
        }
        const auto sourceUsageIt = imageUsages_.find(sourceResource.image);
        const auto destinationUsageIt = imageUsages_.find(destinationResource.image);
        const auto sourceUsage = sourceUsageIt == imageUsages_.end() ? ResourceUsage::Unknown : sourceUsageIt->second;
        const auto destinationUsage = destinationUsageIt == imageUsages_.end() ? ResourceUsage::Unknown : destinationUsageIt->second;
        if (sourceUsage == ResourceUsage::Unknown) {
            lastError_ = "Vulkan resolve source image has no tracked layout";
            return false;
        }
        const auto makeBarrier = [](VkImage image, ResourceUsage previous, VkImageLayout oldLayout,
                                    VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                    std::uint32_t mipLevels, std::uint32_t layers) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcAccessMask = sourceAccess == 0 ? access_of(previous) : sourceAccess;
            barrier.dstAccessMask = destinationAccess;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
            return barrier;
        };
        const auto sourceToTransfer = makeBarrier(sourceResource.image, sourceUsage, layout_of(sourceUsage),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
            sourceResource.mipLevels, sourceResource.layers);
        const auto destinationOldLayout = destinationUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(destinationUsage);
        const auto destinationToTransfer = makeBarrier(destinationResource.image, destinationUsage, destinationOldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destinationUsage == ResourceUsage::Unknown ? VK_ACCESS_NONE : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, destinationResource.mipLevels, destinationResource.layers);
        const std::array<VkImageMemoryBarrier, 2> before{sourceToTransfer, destinationToTransfer};
        vkCmdPipelineBarrier(activeCommandBuffer_, queue_stage_mask(sourceUsage), VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, static_cast<std::uint32_t>(before.size()), before.data());
        for (std::uint32_t mip = 0; mip < sourceResource.mipLevels; ++mip) {
            VkImageResolve region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, sourceResource.layers};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, destinationResource.layers};
            region.extent = {std::max(1u, sourceResource.width >> mip), std::max(1u, sourceResource.height >> mip), 1};
            vkCmdResolveImage(activeCommandBuffer_, sourceResource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                destinationResource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        const auto sourceAfter = makeBarrier(sourceResource.image, ResourceUsage::ShaderRead,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, sourceResource.mipLevels, sourceResource.layers);
        const auto destinationAfter = makeBarrier(destinationResource.image, ResourceUsage::ShaderRead,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, destinationResource.mipLevels, destinationResource.layers);
        const std::array<VkImageMemoryBarrier, 2> after{sourceAfter, destinationAfter};
        vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            0, nullptr, 0, nullptr, static_cast<std::uint32_t>(after.size()), after.data());
        imageUsages_[sourceResource.image] = ResourceUsage::ShaderRead;
        imageUsages_[destinationResource.image] = ResourceUsage::ShaderRead;
        sourceIt->second.usage = ResourceUsage::ShaderRead;
        destinationIt->second.usage = ResourceUsage::ShaderRead;
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            it->second.image == VK_NULL_HANDLE || it->second.depthStencil || it->second.mipLevels <= 1 ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan mip generation handle or resource is invalid";
            return false;
        }
        auto& resource = it->second;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocation, &command) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            return false;
        }
        VkImageMemoryBarrier firstSource{};
        firstSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        const auto usageIt = imageUsages_.find(resource.image);
        const auto previousUsage = usageIt == imageUsages_.end() ? ResourceUsage::Unknown : usageIt->second;
        firstSource.oldLayout = previousUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previousUsage);
        firstSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        firstSource.srcAccessMask = access_of(previousUsage);
        firstSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        firstSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        firstSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        firstSource.image = resource.image;
        firstSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier firstDestination = firstSource;
        firstDestination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        firstDestination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        firstDestination.srcAccessMask = 0;
        firstDestination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        firstDestination.subresourceRange.baseMipLevel = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 2, std::array<VkImageMemoryBarrier, 2>{firstSource, firstDestination}.data());
        std::int32_t sourceWidth = static_cast<std::int32_t>(resource.width);
        std::int32_t sourceHeight = static_cast<std::int32_t>(resource.height);
        for (std::uint32_t level = 1; level < resource.mipLevels; ++level) {
            const auto destinationWidth = std::max(1, sourceWidth / 2);
            const auto destinationHeight = std::max(1, sourceHeight / 2);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1] = {sourceWidth, sourceHeight, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {destinationWidth, destinationHeight, 1};
            vkCmdBlitImage(command, resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            VkImageMemoryBarrier sourceAfter{};
            sourceAfter.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            sourceAfter.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceAfter.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sourceAfter.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceAfter.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceAfter.image = resource.image;
            sourceAfter.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
            VkImageMemoryBarrier destinationAfter = sourceAfter;
            destinationAfter.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationAfter.newLayout = level + 1 < resource.mipLevels ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            destinationAfter.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationAfter.dstAccessMask = level + 1 < resource.mipLevels ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
            destinationAfter.subresourceRange.baseMipLevel = level;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                level + 1 < resource.mipLevels ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, std::array<VkImageMemoryBarrier, 2>{sourceAfter, destinationAfter}.data());
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (submitted) vkQueueWaitIdle(graphicsQueue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        if (!submitted) return false;
        imageUsages_[resource.image] = ResourceUsage::ShaderRead;
        resource.usage = ResourceUsage::ShaderRead;
        return true;
    }
    BindlessTableHandle create_bindless_table(const BindlessTableDesc& description) override {
        if (!descriptorIndexingEnabled_ || descriptorPool_ == VK_NULL_HANDLE || bindlessLayout_ == VK_NULL_HANDLE ||
            description.type != DescriptorType::Texture || description.capacity == 0 || description.capacity > 4096) {
            lastError_ = "Vulkan bindless tables currently require texture descriptors with capacity 1..4096";
            return {};
        }
        std::uint32_t variableCount = description.capacity;
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
        variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        variableInfo.descriptorSetCount = 1;
        variableInfo.pDescriptorCounts = &variableCount;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!allocate_descriptor_sets(1, &bindlessLayout_, &set, pool, &variableInfo)) {
            lastError_ = "Vulkan bindless descriptor set allocation failed";
            return {};
        }
        const auto id = nextBindlessTable_++;
        bindlessTables_[id] = {set, bindlessLayout_, pool, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, description.capacity};
        return {id, 0, description.capacity};
    }
    bool update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource, DescriptorType type) override {
        const auto tableIt = bindlessTables_.find(table.id);
        const auto resourceIt = resources_.find(resource.id);
        if (tableIt == bindlessTables_.end() || slot >= tableIt->second.capacity || resourceIt == resources_.end() ||
            type != DescriptorType::Texture || tableIt->second.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            lastError_ = "Vulkan bindless descriptor update type or slot is invalid";
            return false;
        }
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = tableIt->second.set;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        VkDescriptorImageInfo image{};
        if (type == DescriptorType::Texture && resourceIt->second.view != VK_NULL_HANDLE) {
            image.sampler = defaultSampler_;
            image.imageView = resourceIt->second.view;
            image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
        } else {
            lastError_ = "Vulkan bindless resource has no compatible native view";
            return false;
        }
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    }
    void destroy_bindless_table(BindlessTableHandle table) override {
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end()) return;
        if (boundBindlessTable_ == table.id) boundBindlessTable_ = 0;
        retiredBindlessTables_.push_back(it->second);
        bindlessTables_.erase(it);
    }
    void bind_bindless_table(BindlessTableHandle table) override {
        if (bindlessTables_.find(table.id) == bindlessTables_.end()) return;
        boundBindlessTable_ = table.id;
        const auto pipeline = boundPipeline_;
        const auto it = bindlessTables_.find(table.id);
        if (pipeline && it != bindlessTables_.end() && pipeline->pipelineLayout != VK_NULL_HANDLE && frameActive_) {
            const auto bindPoint = pipeline->pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
            vkCmdBindDescriptorSets(recording_command(), bindPoint,
                pipeline->pipelineLayout, 0, 1, &it->second.set, 0, nullptr);
        }
    }
    void retire_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) return;
        if (boundPipeline_ == &it->second) boundPipeline_ = nullptr;
        if (boundMaterial_ == &it->second) boundMaterial_ = nullptr;
        retiredResources_.push_back(std::move(it->second));
        resources_.erase(it);
    }
    void collect_garbage() override {
        if (device_ == VK_NULL_HANDLE) return;
        if (frameFence_ != VK_NULL_HANDLE && vkGetFenceStatus(device_, frameFence_) != VK_SUCCESS) return;
        release_completed_uploads();
        clear_dynamic_framebuffers();
        for (auto& resource : retiredResources_) {
            release_resource(resource);
            ++stats_.resourceDestroys;
        }
        retiredResources_.clear();
        for (auto& table : retiredBindlessTables_) {
            if (table.set != VK_NULL_HANDLE && table.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.pool, 1, &table.set);
            }
        }
        retiredBindlessTables_.clear();
        for (auto& descriptorSets : retiredDescriptorSets_) {
            if (descriptorSets.pool != VK_NULL_HANDLE && !descriptorSets.sets.empty()) {
                vkFreeDescriptorSets(device_, descriptorSets.pool,
                    static_cast<std::uint32_t>(descriptorSets.sets.size()), descriptorSets.sets.data());
            }
        }
        retiredDescriptorSets_.clear();
    }
    void begin_upload_batch() override {
        uploadBatchActive_ = true;
        release_completed_uploads();
    }
    bool flush_upload_batch() override {
        uploadBatchActive_ = false;
        if (!timelineSemaphoreSupported_) {
            for (auto& upload : pendingUploads_) {
                if (vkWaitForFences(device_, 1, &upload.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                    lastError_ = "Vulkan upload fence wait failed on a device without timeline semaphores";
                    return false;
                }
            }
        }
        return true;
    }
    static VkImageLayout layout_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) return VK_IMAGE_LAYOUT_GENERAL;
        if (is_color_attachment_usage(usage)) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (usage == ResourceUsage::DepthStencil) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (usage == ResourceUsage::CopyDestination) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (usage == ResourceUsage::CopySource) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (usage == ResourceUsage::Present) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    static VkAccessFlags access_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::StorageRead) return VK_ACCESS_SHADER_READ_BIT;
        if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite) return VK_ACCESS_SHADER_WRITE_BIT;
        if (is_color_attachment_usage(usage)) return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (usage == ResourceUsage::DepthStencil) return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (usage == ResourceUsage::CopySource) return VK_ACCESS_TRANSFER_READ_BIT;
        if (usage == ResourceUsage::CopyDestination) return VK_ACCESS_TRANSFER_WRITE_BIT;
        if (usage == ResourceUsage::Present) return VK_ACCESS_MEMORY_READ_BIT;
        return 0;
    }
    static VkPipelineStageFlags stage_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
            usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) {
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        if (is_color_attachment_usage(usage)) return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (usage == ResourceUsage::DepthStencil) return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        if (usage == ResourceUsage::CopySource || usage == ResourceUsage::CopyDestination) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (usage == ResourceUsage::Present) return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    VkPipelineStageFlags queue_stage_mask(ResourceUsage usage) const {
        const auto stages = stage_of(usage);
        if (activeQueueKind_ == RenderQueue::Copy) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (activeQueueKind_ == RenderQueue::Compute) {
            if (stages == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            if (stages == VK_PIPELINE_STAGE_TRANSFER_BIT) return VK_PIPELINE_STAGE_TRANSFER_BIT;
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        return stages;
    }
    VkPipelineStageFlags queue_buffer_stage_mask(ResourceUsage usage) const {
        const auto stages = buffer_stage_of(usage);
        if (activeQueueKind_ == RenderQueue::Copy) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (activeQueueKind_ == RenderQueue::Compute) {
            if (stages == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT || stages == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {
                return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        return stages;
    }
    static VkAccessFlags buffer_access_of(ResourceUsage usage) {
        if (usage == ResourceUsage::VertexBuffer) return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        if (usage == ResourceUsage::IndexBuffer) return VK_ACCESS_INDEX_READ_BIT;
        if (usage == ResourceUsage::UniformBuffer) return VK_ACCESS_UNIFORM_READ_BIT;
        if (usage == ResourceUsage::IndirectArguments) return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        return access_of(usage);
    }
    static VkPipelineStageFlags buffer_stage_of(ResourceUsage usage) {
        if (usage == ResourceUsage::VertexBuffer) return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (usage == ResourceUsage::IndexBuffer) return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (usage == ResourceUsage::UniformBuffer) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (usage == ResourceUsage::IndirectArguments) return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        return stage_of(usage);
    }
    static VkDescriptorType descriptor_type_of(DescriptorType type) {
        if (type == DescriptorType::UniformBuffer) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (type == DescriptorType::StorageBuffer || type == DescriptorType::StructuredBuffer) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (type == DescriptorType::Sampler) return VK_DESCRIPTOR_TYPE_SAMPLER;
        if (type == DescriptorType::StorageTexture) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    bool create_bindless_layout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 4096;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &bindingFlags;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        layoutInfo.pNext = &flagsInfo;
        return vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &bindlessLayout_) == VK_SUCCESS;
    }
    void read_timestamps() {
        if (!timestampPool_ || submittedTimestampCount_ < 2) return;
        std::vector<std::uint64_t> values(submittedTimestampCount_);
        if (vkGetQueryPoolResults(device_, timestampPool_, 0, submittedTimestampCount_,
            values.size() * sizeof(std::uint64_t), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        stats_.gpuPassNanoseconds.clear();
        stats_.gpuPassNanoseconds.reserve(submittedTimestampCount_ / 2);
        for (std::uint32_t i = 0; i + 1 < submittedTimestampCount_; i += 2) {
            const auto ticks = values[i + 1] >= values[i] ? values[i + 1] - values[i] : 0;
            stats_.gpuPassNanoseconds.push_back(static_cast<std::uint64_t>(ticks * timestampPeriod_));
        }
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan resource transition command buffer is invalid";
            return false;
        }
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) {
            lastError_ = "Vulkan resource transition handle is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.buffer != VK_NULL_HANDLE) {
            const auto state = bufferUsages_.find(resource.buffer);
            const auto previous = state == bufferUsages_.end() ? ResourceUsage::Unknown : state->second;
            if (!record_ownership_transfer(handle, resource, previous, usage)) return false;
            if (previous != usage) {
                VkBufferMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barrier.srcAccessMask = buffer_access_of(previous);
                barrier.dstAccessMask = buffer_access_of(usage);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = resource.buffer;
                barrier.offset = 0;
                barrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(activeCommandBuffer_, queue_buffer_stage_mask(previous), queue_buffer_stage_mask(usage),
                    0, 0, nullptr, 1, &barrier, 0, nullptr);
                ++stats_.barriers;
            }
            bufferUsages_[resource.buffer] = usage;
            resource.usage = usage;
            resource.ownerQueue = activeQueueKind_;
            resource.ownerBatch = currentBatchIndex_;
            return true;
        }
        if (resource.image != VK_NULL_HANDLE) {
            const auto state = imageUsages_.find(resource.image);
            const auto previous = state == imageUsages_.end() ? ResourceUsage::Unknown : state->second;
            if (!record_ownership_transfer(handle, resource, previous, usage)) return false;
            if (previous == usage) {
                resource.usage = usage;
                resource.ownerQueue = activeQueueKind_;
                resource.ownerBatch = currentBatchIndex_;
                return true;
            }
            if (renderPassActive_) {
                vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
            }
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = previous == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previous);
            barrier.newLayout = layout_of(usage);
            barrier.srcAccessMask = access_of(previous);
            barrier.dstAccessMask = access_of(usage);
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = resource.image;
            barrier.subresourceRange.aspectMask = resource.depthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            vkCmdPipelineBarrier(activeCommandBuffer_, queue_stage_mask(previous), queue_stage_mask(usage),
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            ++stats_.barriers;
            imageUsages_[resource.image] = usage;
            resource.usage = usage;
            resource.ownerQueue = activeQueueKind_;
            resource.ownerBatch = currentBatchIndex_;
            return true;
        }
        lastError_ = "Vulkan resource transition has no native image or buffer";
        return false;
    }
    void set_render_target(ResourceHandle target) override {
        if (!frameActive_ || commandBuffers_.empty()) return;
        if (renderPassActive_ && currentRenderTarget_ == target.id) return;
        const auto command = activeCommandBuffer_;
        if (renderPassActive_) {
            vkCmdEndRenderPass(command);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
        }
        VkClearValue clear[2]{};
        clear[0].color = {{0.035f, 0.045f, 0.065f, 1.0f}};
        clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPassBegin{};
        renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBegin.renderPass = renderPass_;
        renderPassBegin.clearValueCount = 2;
        renderPassBegin.pClearValues = clear;
        if (target) {
            const auto it = resources_.find(target.id);
            if (it == resources_.end() || it->second.framebuffer == VK_NULL_HANDLE) {
                lastError_ = "Vulkan render target has no compatible framebuffer";
                return;
            }
            renderPassBegin.framebuffer = it->second.framebuffer;
            renderPassBegin.renderArea.extent = {it->second.width, it->second.height};
        } else {
            renderPassBegin.framebuffer = framebuffers_[imageIndex_];
            renderPassBegin.renderArea.extent = swapchainExtent_;
        }
        vkCmdBeginRenderPass(command, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        const auto extent = target ? VkExtent2D{resources_[target.id].width, resources_[target.id].height} : swapchainExtent_;
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        renderPassActive_ = true;
        activeRenderPass_ = renderPass_;
        currentRenderTarget_ = target.id;
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        set_render_targets(color, depth, true);
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth, bool clearAttachments) override {
        if (!frameActive_ || commandBuffers_.empty()) return;
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (!color && depth && depthIt != resources_.end() && depthIt->second.view != VK_NULL_HANDLE) {
            const auto key = (static_cast<std::uint64_t>(depth.id) << 32u) | 1u;
            auto framebufferIt = dynamicFramebuffers_.find(key);
            if (framebufferIt == dynamicFramebuffers_.end()) {
                const VkImageView attachments[] = {depthIt->second.view};
                VkFramebufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = depthOnlyRenderPass_ != VK_NULL_HANDLE ? depthOnlyRenderPass_ : renderPass_;
                info.attachmentCount = 1;
                info.pAttachments = attachments;
                info.width = depthIt->second.width;
                info.height = depthIt->second.height;
                info.layers = 1;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) == VK_SUCCESS) framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
            }
            if (framebufferIt != dynamicFramebuffers_.end()) {
                if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
                VkClearValue clear{};
                clear.depthStencil = {1.0f, 0};
                VkRenderPassBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass = !clearAttachments && depthOnlyLoadRenderPass_ != VK_NULL_HANDLE ? depthOnlyLoadRenderPass_ :
                    (depthOnlyRenderPass_ != VK_NULL_HANDLE ? depthOnlyRenderPass_ : renderPass_);
                begin.framebuffer = framebufferIt->second;
                begin.renderArea.extent = {depthIt->second.width, depthIt->second.height};
                begin.clearValueCount = clearAttachments ? 1u : 0u;
                begin.pClearValues = clearAttachments ? &clear : nullptr;
                vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(depthIt->second.width), static_cast<float>(depthIt->second.height), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {depthIt->second.width, depthIt->second.height}};
                vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
                vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
                renderPassActive_ = true;
                activeRenderPass_ = begin.renderPass;
                currentRenderTarget_ = depth.id;
            }
            return;
        }
        if (color && depth && colorIt != resources_.end() && depthIt != resources_.end()) {
            const auto key = (static_cast<std::uint64_t>(color.id) << 32u) | depth.id;
            auto framebufferIt = dynamicFramebuffers_.find(key);
            if (framebufferIt == dynamicFramebuffers_.end()) {
                const VkImageView attachments[] = {colorIt->second.view, depthIt->second.view};
                VkFramebufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = renderPass_;
                info.attachmentCount = 2;
                info.pAttachments = attachments;
                info.width = colorIt->second.width;
                info.height = colorIt->second.height;
                info.layers = 1;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) == VK_SUCCESS) framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
            }
            if (framebufferIt != dynamicFramebuffers_.end()) {
                if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
                VkClearValue clear[2]{};
                clear[0].color = {{0, 0, 0, 1}};
                clear[1].depthStencil = {1.0f, 0};
                VkRenderPassBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass = renderPass_;
                begin.framebuffer = framebufferIt->second;
                begin.renderArea.extent = {colorIt->second.width, colorIt->second.height};
                begin.clearValueCount = 2;
                begin.pClearValues = clear;
                vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(colorIt->second.width), static_cast<float>(colorIt->second.height), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {colorIt->second.width, colorIt->second.height}};
                vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
                vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
                renderPassActive_ = true;
                activeRenderPass_ = begin.renderPass;
                currentRenderTarget_ = color.id;
            }
            return;
        }
        set_render_target(color);
    }
    void set_render_targets(const std::vector<ResourceHandle>& colors, ResourceHandle depth, bool clearAttachments) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        const auto singleColor = colors.size() == 1 ? resources_.find(colors.front().id) : resources_.end();
        const bool useDynamicSingleColor = singleColor != resources_.end() && singleColor->second.view != VK_NULL_HANDLE &&
            singleColor->second.framebuffer == VK_NULL_HANDLE;
        if (colors.empty() || (colors.size() == 1 && !useDynamicSingleColor)) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth, clearAttachments);
            return;
        }
        std::vector<ResourceRecord*> colorResources;
        std::vector<VkImageView> attachments;
        std::vector<VkFormat> formats;
        for (const auto handle : colors) {
            const auto it = resources_.find(handle.id);
            if (it == resources_.end() || it->second.view == VK_NULL_HANDLE) continue;
            colorResources.push_back(&it->second);
            attachments.push_back(it->second.view);
            formats.push_back(it->second.format);
        }
        ResourceRecord* depthResource = nullptr;
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (it != resources_.end() && it->second.view != VK_NULL_HANDLE) {
                depthResource = &it->second;
                attachments.push_back(it->second.view);
            }
        }
        if (formats.empty() && !depthResource) {
            set_render_target({});
            return;
        }
        const auto renderPass = get_or_create_color_render_pass(formats,
            depthResource ? depthResource->format : VK_FORMAT_UNDEFINED, clearAttachments);
        if (renderPass == VK_NULL_HANDLE) return;
        const auto renderPassKey = reinterpret_cast<std::uintptr_t>(renderPass);
        std::uint64_t key = static_cast<std::uint64_t>(renderPassKey);
        for (const auto view : attachments) key = key * 257ull + reinterpret_cast<std::uint64_t>(view);
        auto framebufferIt = dynamicFramebuffers_.find(key);
        const auto width = colorResources.empty() ? depthResource->width : colorResources.front()->width;
        const auto height = colorResources.empty() ? depthResource->height : colorResources.front()->height;
        if (framebufferIt == dynamicFramebuffers_.end()) {
            VkFramebufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = renderPass;
            info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.width = width;
            info.height = height;
            info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) return;
            framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
        }
        if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
        renderPassActive_ = false;
        std::vector<VkClearValue> clears(attachments.size());
        for (std::size_t index = 0; index < colorResources.size(); ++index) clears[index].color = {{0, 0, 0, 1}};
        if (depthResource) clears.back().depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass;
        begin.framebuffer = framebufferIt->second;
        begin.renderArea.extent = {width, height};
        begin.clearValueCount = static_cast<std::uint32_t>(clears.size());
        begin.pClearValues = clears.data();
        vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
        renderPassActive_ = true;
        activeRenderPass_ = renderPass;
        currentRenderTarget_ = colors.empty() ? depth.id : colors.front().id;
    }
    void set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        VkViewport viewport{x, y, width, height, minDepth, maxDepth};
        VkRect2D scissor{
            {static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y))},
            {static_cast<std::uint32_t>(std::max(0.0f, std::ceil(width))),
             static_cast<std::uint32_t>(std::max(0.0f, std::ceil(height)))}};
        vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
    }
    void begin_frame() override {
        ++stats_.frames;
        if (!swapchain_) {
            if (frameFence_ != VK_NULL_HANDLE) {
                const auto waitResult = vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, 5'000'000'000ull);
                if (waitResult != VK_SUCCESS) {
                lastError_ = "Vulkan off-screen frame fence wait failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
                }
            }
            for (const auto fence : batchFences_) {
                if (fence == VK_NULL_HANDLE) continue;
                const auto waitResult = vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
                if (waitResult != VK_SUCCESS) {
                    lastError_ = "Vulkan off-screen batch fence wait failed result=" +
                        std::to_string(static_cast<int>(waitResult));
                    capabilities_.deviceState = waitResult == VK_ERROR_DEVICE_LOST
                        ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
                    return;
                }
            }
            read_timestamps();
            collect_garbage();
            for (const auto semaphore : batchSignals_) {
                if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
            }
            batchSignals_.clear();
            for (const auto fence : batchFences_) {
                if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
            }
            batchFences_.clear();
            if (commandPool_ != VK_NULL_HANDLE && vkResetCommandPool(device_, commandPool_, 0) != VK_SUCCESS) {
                lastError_ = "Vulkan off-screen command pool reset failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
            }
            if (computeCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, computeCommandPool_, 0);
            if (copyCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, copyCommandPool_, 0);
            if (frameFence_ != VK_NULL_HANDLE) vkResetFences(device_, 1, &frameFence_);
            timestampCursor_ = 0;
            timestampPoolResetThisFrame_ = false;
            frameActive_ = true;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            transientBatchCommands_.clear();
            currentBatchIndex_ = 0;
            currentRenderTarget_ = 0;
            return;
        }
        const auto frameWaitResult = vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, 5'000'000'000ull);
        if (frameWaitResult != VK_SUCCESS) {
            lastError_ = "Vulkan frame fence wait failed result=" + std::to_string(static_cast<int>(frameWaitResult));
            capabilities_.deviceState = frameWaitResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return;
        }
        for (const auto fence : batchFences_) {
            if (fence != VK_NULL_HANDLE) vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
        }
        // Upload command buffers use the graphics command pool. Reclaim them
        // at the batch boundary before resetting that pool for this frame.
        release_completed_uploads(true);
        read_timestamps();
        collect_garbage();
        for (const auto semaphore : batchSignals_) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        }
        batchSignals_.clear();
        for (const auto fence : batchFences_) {
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        }
        batchFences_.clear();
        if (commandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, commandPool_, 0);
        if (computeCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, computeCommandPool_, 0);
        if (copyCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, copyCommandPool_, 0);
        vkResetFences(device_, 1, &frameFence_);
        const auto acquireResult = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex_);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            lastError_ = "Vulkan acquire failed result=" + std::to_string(static_cast<int>(acquireResult));
            capabilities_.deviceState = acquireResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return;
        }
        timestampCursor_ = 0;
        timestampPoolResetThisFrame_ = false;
        frameActive_ = true;
        activeCommandBuffer_ = VK_NULL_HANDLE;
        transientBatchCommands_.clear();
        currentBatchIndex_ = 0;
        swapchainPrepared_ = false;
        currentBatchWaitsForAcquire_ = false;
        currentRenderTarget_ = 0;
    }
    bool begin_queue(RenderQueue queue) override {
        return begin_queue(queue, currentBatchIndex_, {});
    }
    bool begin_queue(RenderQueue queue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        if (!frameActive_ || device_ == VK_NULL_HANDLE) return false;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) return false;
        currentBatchIndex_ = batchIndex;
        currentWaitBatches_ = waitBatches;
        for (const auto waitBatch : currentWaitBatches_) {
            const bool isPendingProducer = pendingCommandBuffer_ != VK_NULL_HANDLE && waitBatch == pendingBatchIndex_;
            if (!isPendingProducer && (waitBatch >= batchSignals_.size() || batchSignals_[waitBatch] == VK_NULL_HANDLE)) {
                lastError_ = "Vulkan queue batch wait references an unsignaled batch";
                return false;
            }
        }
        const auto pool = pool_for(queue);
        if (pool == VK_NULL_HANDLE) return false;
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &allocation, &command) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, pool, 1, &command);
            return false;
        }
        activeCommandBuffer_ = command;
        activeQueue_ = queue_for(queue);
        activeQueueKind_ = queue;
        currentBatchWaitsForAcquire_ = queue == RenderQueue::Graphics && !swapchainPrepared_ &&
            swapchain_ != VK_NULL_HANDLE && imageAvailable_ != VK_NULL_HANDLE;
        transientBatchCommands_.push_back(command);
        if (timestampPool_ && queue != RenderQueue::Copy && !timestampPoolResetThisFrame_) {
            vkCmdResetQueryPool(command, timestampPool_, 0, 512);
            timestampPoolResetThisFrame_ = true;
        }
        if (queue == RenderQueue::Graphics && !swapchainPrepared_ && swapchain_ != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout = swapchainInitialized_[imageIndex_] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.image = swapchainImages_[imageIndex_];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            swapchainPrepared_ = true;
            swapchainInitialized_[imageIndex_] = true;
        }
        return true;
    }
    bool end_queue(RenderQueue queue, std::uint32_t batchIndex, bool lastBatch, bool present) override {
        if (activeCommandBuffer_ == VK_NULL_HANDLE || activeQueue_ == VK_NULL_HANDLE) return false;
        if (renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
            currentRenderTarget_ = 0;
        }
        if (present && swapchain_ != VK_NULL_HANDLE && activeQueueKind_ == RenderQueue::Graphics) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.image = swapchainImages_[imageIndex_];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        if (!lastBatch && !present) {
            if (!finalize_pending_queue_batch()) return false;
            pendingCommandBuffer_ = activeCommandBuffer_;
            pendingQueue_ = activeQueue_;
            pendingCommandPool_ = pool_for(activeQueueKind_);
            pendingQueueKind_ = activeQueueKind_;
            pendingBatchIndex_ = batchIndex;
            pendingWaitBatches_ = currentWaitBatches_;
            pendingBatchUploadWaitValue_ = pendingUploadWaitValue_;
            pendingBatchWaitsForAcquire_ = currentBatchWaitsForAcquire_;
            pendingUploadWaitValue_ = 0;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            activeQueue_ = VK_NULL_HANDLE;
            currentBatchWaitsForAcquire_ = false;
            (void)queue;
            return capabilities_.deviceState == RenderDeviceState::Ready;
        }
        if (!finalize_pending_queue_batch()) return false;
        const auto endResult = vkEndCommandBuffer(activeCommandBuffer_);
        if (endResult != VK_SUCCESS) {
            lastError_ = "Vulkan command buffer finalization failed result=" + std::to_string(static_cast<int>(endResult));
            capabilities_.deviceState = endResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            return false;
        }
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        if (currentBatchWaitsForAcquire_) {
            waits.push_back(imageAvailable_);
            stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        for (const auto waitBatch : currentWaitBatches_) {
            if (waitBatch < batchSignals_.size() && batchSignals_[waitBatch] != VK_NULL_HANDLE) {
                waits.push_back(batchSignals_[waitBatch]);
                stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        }
        if (pendingUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE) {
            waits.push_back(uploadTimeline_);
            stages.push_back(activeQueueKind_ == RenderQueue::Copy ? VK_PIPELINE_STAGE_TRANSFER_BIT :
                (activeQueueKind_ == RenderQueue::Compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
        }
        VkSemaphore signal = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &signal) != VK_SUCCESS) {
            activeCommandBuffer_ = VK_NULL_HANDLE;
            return false;
        }
        if (batchSignals_.size() <= batchIndex) batchSignals_.resize(batchIndex + 1, VK_NULL_HANDLE);
        batchSignals_[batchIndex] = signal;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &activeCommandBuffer_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        VkTimelineSemaphoreSubmitInfo timelineWait{};
        if (pendingUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE) {
            timelineWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineWait.waitSemaphoreValueCount = static_cast<std::uint32_t>(waits.size());
            std::vector<std::uint64_t> waitValues(waits.size(), 0);
            waitValues.back() = pendingUploadWaitValue_;
            timelineWait.pWaitSemaphoreValues = waitValues.data();
            submit.pNext = &timelineWait;
            if (lastBatch) pendingUploadWaitValue_ = 0;
        }
        VkFence batchFence = frameFence_;
        if (!lastBatch) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(device_, &fenceInfo, nullptr, &batchFence) != VK_SUCCESS) {
                vkDestroySemaphore(device_, signal, nullptr);
                activeCommandBuffer_ = VK_NULL_HANDLE;
                return false;
            }
        }
        const auto submitResult = vkQueueSubmit(activeQueue_, 1, &submit, batchFence);
        if (submitResult != VK_SUCCESS) {
            lastError_ = "Vulkan queue submission failed";
            capabilities_.deviceState = RenderDeviceState::Lost;
            if (!lastBatch) vkDestroyFence(device_, batchFence, nullptr);
            batchSignals_[batchIndex] = VK_NULL_HANDLE;
            vkDestroySemaphore(device_, signal, nullptr);
            activeCommandBuffer_ = VK_NULL_HANDLE;
            activeQueue_ = VK_NULL_HANDLE;
            currentBatchWaitsForAcquire_ = false;
            return false;
        } else {
            ++stats_.queueSubmissions;
            if (!lastBatch) batchFences_.push_back(batchFence);
            if (lastBatch || present) submittedTimestampCount_ = timestampCursor_;
            if (lastBatch) pendingUploadWaitValue_ = 0;
        }
        activeCommandBuffer_ = VK_NULL_HANDLE;
        activeQueue_ = VK_NULL_HANDLE;
        currentBatchWaitsForAcquire_ = false;
        if (present && swapchain_ != VK_NULL_HANDLE) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &signal;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &imageIndex_;
            const auto result = vkQueuePresentKHR(presentQueue_, &present);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                lastError_ = "Vulkan present failed result=" + std::to_string(static_cast<int>(result));
                capabilities_.deviceState = result == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            }
        }
        (void)queue;
        return capabilities_.deviceState == RenderDeviceState::Ready;
    }
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void begin_pass() override {
        if (timestampPool_ && activeQueueKind_ != RenderQueue::Copy && frameActive_ && timestampCursor_ < 511) {
            vkCmdWriteTimestamp(recording_command(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, timestampCursor_++);
        }
    }
    void end_pass() override {
        if (timestampPool_ && activeQueueKind_ != RenderQueue::Copy && frameActive_ && timestampCursor_ < 512) {
            vkCmdWriteTimestamp(recording_command(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, timestampCursor_++);
        }
    }
    void set_debug_name(ResourceHandle handle, std::string_view name) override {
        debugNames_[handle.id] = std::string(name);
        if (!setDebugUtilsObjectName_ || device_ == VK_NULL_HANDLE) return;
        const auto resource = resources_.find(handle.id);
        if (resource == resources_.end()) return;
        VkObjectType objectType = VK_OBJECT_TYPE_UNKNOWN;
        std::uint64_t objectHandle = 0;
        if (resource->second.buffer != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_BUFFER;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.buffer);
        } else if (resource->second.image != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_IMAGE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.image);
        } else if (resource->second.shader != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_SHADER_MODULE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.shader);
        } else if (resource->second.graphicsPipeline != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_PIPELINE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.graphicsPipeline);
        }
        if (objectType == VK_OBJECT_TYPE_UNKNOWN) return;
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = objectType;
        info.objectHandle = objectHandle;
        info.pObjectName = debugNames_[handle.id].c_str();
        setDebugUtilsObjectName_(device_, &info);
    }
    void begin_debug_label(std::string_view name) override {
        ++stats_.debugMarkers;
        if (!cmdBeginDebugUtilsLabel_ || recording_command() == VK_NULL_HANDLE) return;
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        const auto& stored = debugLabelStorage_.emplace_back(name);
        label.pLabelName = stored.c_str();
        label.color[0] = 0.2f;
        label.color[1] = 0.6f;
        label.color[2] = 1.0f;
        label.color[3] = 1.0f;
        cmdBeginDebugUtilsLabel_(recording_command(), &label);
    }
    void end_debug_label() override {
        if (cmdEndDebugUtilsLabel_ && recording_command() != VK_NULL_HANDLE) cmdEndDebugUtilsLabel_(recording_command());
        if (!debugLabelStorage_.empty()) debugLabelStorage_.pop_back();
    }
    bool ensure_pipeline(ResourceRecord& record) {
        if (record.computePipeline != VK_NULL_HANDLE) return true;
        const auto activePass = activeRenderPass_ != VK_NULL_HANDLE ? activeRenderPass_ : renderPass_;
        const bool depthOnlyPass = activePass == depthOnlyRenderPass_ || activePass == depthOnlyLoadRenderPass_;
        if (record.graphicsPipeline != VK_NULL_HANDLE && record.pipelineRenderPass == activePass) return true;
        if (depthOnlyPass && record.depthOnlyPipeline != VK_NULL_HANDLE && record.pipelineRenderPass == activePass) return true;
        if (activeRenderPass_ == VK_NULL_HANDLE && renderPass_ == VK_NULL_HANDLE) return false;
        if (record.graphicsPipeline != VK_NULL_HANDLE || record.depthOnlyPipeline != VK_NULL_HANDLE) {
            std::uint32_t pipelineId = 0;
            for (const auto& [id, resource] : resources_) {
                if (&resource == &record) {
                    pipelineId = id;
                    break;
                }
            }
            if (pipelineId != 0) {
                for (auto& [id, resource] : resources_) {
                    if (resource.kind == ResourceKind::Material && resource.material.pipeline.id == pipelineId) {
                        release_descriptor_sets(resource);
                    }
                }
            }
            vkDestroyPipeline(device_, record.graphicsPipeline, nullptr);
            record.graphicsPipeline = VK_NULL_HANDLE;
            if (record.depthOnlyPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, record.depthOnlyPipeline, nullptr);
                record.depthOnlyPipeline = VK_NULL_HANDLE;
            }
            if (record.pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
                record.pipelineLayout = VK_NULL_HANDLE;
            }
            if (record.descriptorLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, record.descriptorLayout, nullptr);
                record.descriptorLayout = VK_NULL_HANDLE;
            }
            for (const auto layout : record.descriptorLayouts) {
                if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) {
                    vkDestroyDescriptorSetLayout(device_, layout, nullptr);
                }
            }
            record.descriptorLayouts.clear();
        }
        if (record.pipeline.computeShader != 0) {
            const auto compute = resources_.find(record.pipeline.computeShader);
            if (compute == resources_.end() || compute->second.shader == VK_NULL_HANDLE) return false;
            if (!validate_backend_shader_layout(compute->second.shaderBindings, lastError_)) return false;
            const bool usesBindlessSet = descriptorIndexingEnabled_ && std::any_of(compute->second.shaderBindings.begin(),
                compute->second.shaderBindings.end(), [](const auto& binding) {
                    return binding.space == 0 && binding.unbounded;
                });
            std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsBySpace;
            const auto addDescriptorBinding = [&](std::uint32_t space, std::uint32_t slot, VkDescriptorType type,
                                                  std::uint32_t count = 1, bool unbounded = false) {
                if (space >= 16) return false;
                if (bindingsBySpace.size() <= space) bindingsBySpace.resize(space + 1);
                auto& descriptorBindings = bindingsBySpace[space];
                const auto existing = std::find_if(descriptorBindings.begin(), descriptorBindings.end(), [slot](const auto& b) { return b.binding == slot; });
                if (existing != descriptorBindings.end()) return existing->descriptorType == type && existing->descriptorCount == (unbounded ? 1024u : count);
                VkDescriptorSetLayoutBinding binding{};
                binding.binding = slot;
                binding.descriptorCount = unbounded ? 1024u : count;
                binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                binding.descriptorType = type;
                descriptorBindings.push_back(binding);
                return true;
            };
            for (const auto& reflected : compute->second.shaderBindings) {
                const auto type = reflected.type == "texture" ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                    (reflected.type == "storage_texture" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                    (reflected.type == "sampler" ? VK_DESCRIPTOR_TYPE_SAMPLER :
                    (reflected.type == "uniform_buffer" ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)));
                if (!addDescriptorBinding(reflected.space, reflected.slot, type, reflected.count, reflected.unbounded)) return false;
            }
            record.descriptorLayouts.clear();
            const auto setCount = std::max<std::size_t>(bindingsBySpace.size(), usesBindlessSet ? 1u : 0u);
            record.descriptorLayouts.reserve(setCount);
            for (std::size_t set = 0; set < setCount; ++set) {
                if (usesBindlessSet && set == 0) {
                    record.descriptorLayouts.push_back(bindlessLayout_);
                    continue;
                }
                const auto& descriptorBindings = set < bindingsBySpace.size()
                    ? bindingsBySpace[set] : std::vector<VkDescriptorSetLayoutBinding>{};
                VkDescriptorSetLayoutCreateInfo descriptorInfo{};
                descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                descriptorInfo.bindingCount = static_cast<std::uint32_t>(descriptorBindings.size());
                descriptorInfo.pBindings = descriptorBindings.data();
                VkDescriptorSetLayout layout = VK_NULL_HANDLE;
                if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr, &layout) != VK_SUCCESS) return false;
                record.descriptorLayouts.push_back(layout);
            }
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.descriptorLayouts.size());
            layoutInfo.pSetLayouts = record.descriptorLayouts.data();
            if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &record.pipelineLayout) != VK_SUCCESS) return false;
            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = compute->second.shader;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.stage = stage;
            pipelineInfo.layout = record.pipelineLayout;
            const auto result = vkCreateComputePipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.computePipeline);
            if (result != VK_SUCCESS) {
                lastError_ = "Vulkan compute pipeline creation failed VkResult=" + std::to_string(static_cast<int>(result));
                return false;
            }
            return true;
        }
        if (record.pipeline.sampleCount != 1 ||
            (record.pipeline.fillMode != "solid" && record.pipeline.fillMode != "wireframe") ||
            (record.pipeline.cullMode != "back" && record.pipeline.cullMode != "front" && record.pipeline.cullMode != "none") ||
            (record.pipeline.topology != "triangle" && record.pipeline.topology != "line" && record.pipeline.topology != "point") ||
            record.pipeline.colorFormats.size() > 8 ||
            std::any_of(record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string& format) { return !is_supported_color_format(format); }) ||
            ((!record.pipeline.colorFormats.empty() &&
              std::any_of(record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string& format) { return format != "rgba8" && format != "bgra8" &&
                    format != "rgba16f" && format != "r32f"; })) ||
             !is_supported_color_format(record.pipeline.colorFormat)) ||
            !is_supported_pipeline_depth_format(record.pipeline.depthFormat)) {
            lastError_ = "Vulkan pipeline fixed-function state or attachment format is unsupported";
            return false;
        }
        const auto vertex = resources_.find(record.pipeline.vertexShader);
        const auto fragment = resources_.find(record.pipeline.fragmentShader);
        if (vertex == resources_.end() || fragment == resources_.end() ||
            vertex->second.shader == VK_NULL_HANDLE || fragment->second.shader == VK_NULL_HANDLE) return false;
        std::vector<ShaderBinding> mergedShaderBindings;
        if (!merge_shader_bindings(vertex->second.shaderBindings, mergedShaderBindings, lastError_) ||
            !merge_shader_bindings(fragment->second.shaderBindings, mergedShaderBindings, lastError_)) return false;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsBySpace;
        const bool usesBindlessSet = descriptorIndexingEnabled_ && std::any_of(mergedShaderBindings.begin(),
            mergedShaderBindings.end(), [](const auto& binding) {
                return binding.space == 0 && binding.unbounded;
            });
        const auto ensureSpace = [&bindingsBySpace](std::uint32_t space) {
            if (space >= 16) return false;
            if (bindingsBySpace.size() <= space) bindingsBySpace.resize(space + 1);
            return true;
        };
        const auto addDescriptorBinding = [&](std::uint32_t space, std::uint32_t slot, VkDescriptorType type, std::uint32_t count = 1, bool unbounded = false) {
            if (!ensureSpace(space)) return;
            auto& descriptorBindings = bindingsBySpace[space];
            const auto existing = std::find_if(descriptorBindings.begin(), descriptorBindings.end(),
                [slot](const auto& binding) { return binding.binding == slot; });
            if (existing != descriptorBindings.end()) {
                if (existing->descriptorType != type || existing->descriptorCount != (unbounded ? 1024u : count)) lastError_ = "Vulkan descriptor layout conflict";
                return;
            }
            VkDescriptorSetLayoutBinding layoutBinding{};
            layoutBinding.binding = slot;
            layoutBinding.descriptorCount = unbounded ? 1024u : count;
            layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
            layoutBinding.descriptorType = type;
            descriptorBindings.push_back(layoutBinding);
        };
        for (const auto shaderId : {record.pipeline.vertexShader, record.pipeline.fragmentShader}) {
            const auto shader = resources_.find(shaderId);
            if (shader == resources_.end()) continue;
            for (const auto& reflected : shader->second.shaderBindings) {
                if (usesBindlessSet && reflected.space == 0) continue;
                const auto type = reflected.type == "texture" ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                    (reflected.type == "storage_texture" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                    (reflected.type == "sampler" ? VK_DESCRIPTOR_TYPE_SAMPLER :
                    (reflected.type == "uniform_buffer" ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)));
                addDescriptorBinding(reflected.space, reflected.slot, type, reflected.count, reflected.unbounded);
            }
        }
        record.descriptorLayouts.clear();
        const auto setCount = std::max<std::size_t>(bindingsBySpace.size(), usesBindlessSet ? 1u : 0u);
        record.descriptorLayouts.reserve(setCount);
        for (std::size_t set = 0; set < setCount; ++set) {
            if (usesBindlessSet && set == 0) {
                record.descriptorLayouts.push_back(bindlessLayout_);
                continue;
            }
            const auto& descriptorBindings = set < bindingsBySpace.size()
                ? bindingsBySpace[set] : std::vector<VkDescriptorSetLayoutBinding>{};
            VkDescriptorSetLayoutCreateInfo descriptorInfo{};
            descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptorInfo.bindingCount = static_cast<std::uint32_t>(descriptorBindings.size());
            descriptorInfo.pBindings = descriptorBindings.data();
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr, &layout) != VK_SUCCESS) return false;
            record.descriptorLayouts.push_back(layout);
        }
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.descriptorLayouts.size());
        layoutInfo.pSetLayouts = record.descriptorLayouts.data();
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &record.pipelineLayout) != VK_SUCCESS) return false;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex->second.shader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment->second.shader;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vertexBinding{};
        VkVertexInputAttributeDescription vertexAttribute{};
        if (record.pipeline.vertexInput) {
            vertexBinding.binding = 0;
            vertexBinding.stride = sizeof(float) * 3;
            vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            vertexAttribute.location = 0;
            vertexAttribute.binding = 0;
            vertexAttribute.format = VK_FORMAT_R32G32B32_SFLOAT;
            vertexAttribute.offset = 0;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &vertexBinding;
            vertexInput.vertexAttributeDescriptionCount = 1;
            vertexInput.pVertexAttributeDescriptions = &vertexAttribute;
        }
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = topology(record.pipeline.topology);
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = nullptr;
        viewportState.scissorCount = 1;
        viewportState.pScissors = nullptr;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = fill_mode(record.pipeline.fillMode);
        raster.cullMode = cull_mode(record.pipeline.cullMode);
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{};
        depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthState.depthTestEnable = record.pipeline.depthFormat == "none" || !record.pipeline.depthTest
            ? VK_FALSE : VK_TRUE;
        depthState.depthWriteEnable = record.pipeline.depthFormat == "none" || !record.pipeline.depthWrite
            ? VK_FALSE : VK_TRUE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = record.pipeline.alphaBlend ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        const auto colorFormats = record.pipeline.colorFormats.empty()
            ? std::vector<std::string>{record.pipeline.colorFormat} : record.pipeline.colorFormats;
        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(colorFormats.size(), blend);
        VkPipelineColorBlendStateCreateInfo blendState{};
        blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blendState.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
        blendState.pAttachments = blendAttachments.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = depthOnlyPass ? 1u : 2u;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = depthOnlyPass ? nullptr : &blendState;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = record.pipelineLayout;
        pipelineInfo.renderPass = activePass;
        pipelineInfo.subpass = 0;
        if (depthOnlyPass) {
            const auto result = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr,
                &record.depthOnlyPipeline);
            if (result != VK_SUCCESS) {
                lastError_ = "Vulkan depth-only pipeline creation failed VkResult=" + std::to_string(static_cast<int>(result));
                vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
                record.pipelineLayout = VK_NULL_HANDLE;
                return false;
            }
            record.pipelineRenderPass = pipelineInfo.renderPass;
            return true;
        }
        const auto graphicsResult = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.graphicsPipeline);
        if (graphicsResult != VK_SUCCESS) {
            lastError_ = "Vulkan graphics pipeline creation failed VkResult=" + std::to_string(static_cast<int>(graphicsResult));
            vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
            record.pipelineLayout = VK_NULL_HANDLE;
            return false;
        }
        record.pipelineRenderPass = pipelineInfo.renderPass;
        if (depthOnlyRenderPass_ != VK_NULL_HANDLE) {
            pipelineInfo.stageCount = 1;
            pipelineInfo.pColorBlendState = nullptr;
            pipelineInfo.subpass = 0;
            pipelineInfo.renderPass = depthOnlyRenderPass_;
            const auto depthResult = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.depthOnlyPipeline);
            if (depthResult != VK_SUCCESS) {
                lastError_ = "Vulkan secondary depth-only pipeline creation failed VkResult=" + std::to_string(static_cast<int>(depthResult));
                record.depthOnlyPipeline = VK_NULL_HANDLE;
            }
        }
        return true;
    }
    void bind_pipeline(std::string_view name) override {
        for (auto& [id, resource] : resources_) {
            if (resource.kind == ResourceKind::Pipeline && resource.pipeline.name == name) {
                bind_pipeline(ResourceHandle{id, ResourceKind::Pipeline});
                return;
            }
        }
        boundPipeline_ = nullptr;
        lastError_ = "Vulkan pipeline name is invalid: " + std::string(name);
    }
    void bind_pipeline(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        boundPipeline_ = it != resources_.end() && it->second.kind == ResourceKind::Pipeline &&
            handle.kind == ResourceKind::Pipeline ? &it->second : nullptr;
        if (!boundPipeline_) {
            lastError_ = "Vulkan pipeline handle is invalid";
            return;
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_)) {
            if (boundPipeline_->pipeline.computeShader) {
                vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_COMPUTE, boundPipeline_->computePipeline);
            } else {
                const auto target = resources_.find(currentRenderTarget_);
                const bool depthOnly = target != resources_.end() && target->second.kind == ResourceKind::DepthStencil;
                vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    depthOnly && boundPipeline_->depthOnlyPipeline != VK_NULL_HANDLE ? boundPipeline_->depthOnlyPipeline : boundPipeline_->graphicsPipeline);
            }
        } else if (boundPipeline_) {
            if (lastError_.empty()) lastError_ = "Vulkan pipeline binding failed";
        }
    }
    void bind_material(ResourceHandle handle) override {
        const auto material = resources_.find(handle.id);
        if (material == resources_.end() || material->second.kind != ResourceKind::Material ||
            handle.kind != ResourceKind::Material) {
            lastError_ = "Vulkan material handle is invalid";
            return;
        }
        ++stats_.materialBinds;
        boundMaterial_ = &material->second;
        bind_pipeline(material->second.material.pipeline);
        const auto pipeline = resources_.find(material->second.material.pipeline.id);
        if (pipeline == resources_.end() || pipeline->second.pipelineLayout == VK_NULL_HANDLE || descriptorPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan material binding has invalid pipeline layout or descriptor pool";
            return;
        }
        const auto bindlessTable = bindlessTables_.find(boundBindlessTable_);
        if (material->second.material.bindless) {
            if (bindlessTable != bindlessTables_.end()) {
                const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
                vkCmdBindDescriptorSets(recording_command(), bindPoint,
                    pipeline->second.pipelineLayout, 0, 1, &bindlessTable->second.set, 0, nullptr);
            }
            return;
        }
        stats_.descriptorBinds += material->second.material.bindings.size();
        for (const auto& binding : material->second.material.bindings) {
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId : {pipeline->second.pipeline.vertexShader, pipeline->second.pipeline.fragmentShader, pipeline->second.pipeline.computeShader}) {
                const auto shader = resources_.find(shaderId);
                if (shader == resources_.end() || shader->second.shaderBindings.empty()) continue;
                reflectionAvailable = true;
                found |= std::any_of(shader->second.shaderBindings.begin(), shader->second.shaderBindings.end(),
                    [&](const auto& reflected) { return shader_binding_matches(reflected, binding); });
            }
            if (reflectionAvailable && !found) {
                lastError_ = "Vulkan material '" + material->second.material.name + "' binding '" + binding.name +
                    "' does not match shader set=" + std::to_string(binding.space) +
                    " binding=" + std::to_string(binding.slot) + " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        if (pipeline->second.descriptorLayouts.empty()) return;
        if (material->second.descriptorSets.empty()) {
        VkDescriptorSetAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            const bool pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
                pipeline->second.descriptorLayouts.front() == bindlessLayout_;
            const auto firstMaterialSet = pipeline->second.pipeline.computeShader ? 0u : (pipelineUsesBindlessSet ? 1u : 0u);
            if (pipeline->second.descriptorLayouts.size() <= firstMaterialSet) return;
            allocation.descriptorSetCount = static_cast<std::uint32_t>(pipeline->second.descriptorLayouts.size()) - firstMaterialSet;
            allocation.pSetLayouts = pipeline->second.descriptorLayouts.data() + firstMaterialSet;
            material->second.descriptorSets.resize(pipeline->second.descriptorLayouts.size(), VK_NULL_HANDLE);
            if (!allocate_descriptor_sets(allocation.descriptorSetCount, allocation.pSetLayouts,
                material->second.descriptorSets.data() + firstMaterialSet, material->second.descriptorPool)) {
                material->second.descriptorSets.clear();
                return;
            }
            material->second.descriptorSet = material->second.descriptorSets.empty()
                ? VK_NULL_HANDLE : material->second.descriptorSets.back();
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> images;
            std::vector<VkDescriptorBufferInfo> buffers;
            std::size_t imageReserve = 0;
            std::size_t bufferReserve = 0;
            for (const auto& binding : material->second.material.bindings) {
                const auto count = binding.resources.empty() ? (binding.resource ? 1u : 0u)
                    : static_cast<std::uint32_t>(binding.resources.size());
                if (binding.type == DescriptorType::Texture || binding.type == DescriptorType::StorageTexture || binding.type == DescriptorType::Sampler) {
                    imageReserve += count;
                } else {
                    bufferReserve += count;
                }
            }
            images.reserve(imageReserve);
            buffers.reserve(bufferReserve);
            for (const auto& binding : material->second.material.bindings) {
                if (binding.space >= material->second.descriptorSets.size() ||
                    (pipelineUsesBindlessSet && binding.space == 0)) continue;
                const std::vector<ResourceHandle> handles = binding.resources.empty()
                    ? std::vector<ResourceHandle>{binding.resource}
                    : binding.resources;
                if (handles.empty() || (handles.size() == 1 && !handles.front())) continue;
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = material->second.descriptorSets[binding.space];
                write.dstBinding = binding.slot;
                write.descriptorCount = static_cast<std::uint32_t>(handles.size());
                if (binding.type == DescriptorType::Texture) {
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.view == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan texture array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({defaultSampler_, resource->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = images.data() + first;
                } else if (binding.type == DescriptorType::StorageTexture) {
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.view == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan storage texture array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({VK_NULL_HANDLE, resource->second.view, VK_IMAGE_LAYOUT_GENERAL});
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    write.pImageInfo = images.data() + first;
                } else if (binding.type == DescriptorType::Sampler) {
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.sampler == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan sampler array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({resource->second.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                    write.pImageInfo = images.data() + first;
                } else {
                    const auto first = buffers.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.buffer == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan buffer array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        buffers.push_back({resource->second.buffer, 0, VK_WHOLE_SIZE});
                    }
                    write.descriptorType = binding.type == DescriptorType::UniformBuffer ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    write.pBufferInfo = buffers.data() + first;
                }
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        const bool pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
            pipeline->second.descriptorLayouts.front() == bindlessLayout_;
        const auto firstMaterialSet = pipeline->second.pipeline.computeShader ? 0u : (pipelineUsesBindlessSet ? 1u : 0u);
        const auto materialSetCount = static_cast<std::uint32_t>(material->second.descriptorSets.size()) - firstMaterialSet;
        if (materialSetCount > 0) {
            vkCmdBindDescriptorSets(recording_command(), bindPoint,
                pipeline->second.pipelineLayout, firstMaterialSet, materialSetCount,
                material->second.descriptorSets.data() + firstMaterialSet, 0, nullptr);
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() || material->second.kind != ResourceKind::Material) {
            lastError_ = "Vulkan material handle not found";
            return false;
        }
        if (material->second.material.pipeline.id != description.pipeline.id) {
            lastError_ = "Vulkan material pipeline cannot be changed after creation";
            return false;
        }
        material->second.material = description;
        release_descriptor_sets(material->second);
        material->second.descriptorSet = VK_NULL_HANDLE;
        material->second.descriptorSets.clear();
        boundMaterial_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot, std::uint32_t space) override {
        if (!device_ || !boundMaterial_) {
            if (lastError_.empty()) lastError_ = "Vulkan uniform buffer binding has invalid command state";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() || resource->second.kind != ResourceKind::Buffer ||
            resource->second.buffer == VK_NULL_HANDLE) {
            lastError_ = "Vulkan uniform buffer handle is invalid";
            return;
        }
        const auto pipeline = resources_.find(boundMaterial_->material.pipeline.id);
        if (pipeline == resources_.end() || pipeline->second.pipelineLayout == VK_NULL_HANDLE) {
            lastError_ = "Vulkan uniform buffer pipeline layout is invalid";
            return;
        }
        const auto pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
            pipeline->second.descriptorLayouts.front() == bindlessLayout_;
        if (space >= boundMaterial_->descriptorSets.size() ||
            (pipelineUsesBindlessSet && space == 0)) {
            lastError_ = "Vulkan uniform buffer descriptor set or space is invalid";
            return;
        }
        VkDescriptorBufferInfo bufferInfo{resource->second.buffer, 0, resource->second.size};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = boundMaterial_->descriptorSets[space];
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindDescriptorSets(recording_command(), bindPoint,
            pipeline->second.pipelineLayout, space, 1, &boundMaterial_->descriptorSets[space], 0, nullptr);
    }
    void draw_sprite(const SpriteDraw&) override { ++stats_.drawCalls; ++stats_.spriteCalls; if (frameActive_ && boundPipeline_ && boundPipeline_->graphicsPipeline) vkCmdDraw(recording_command(), 6, 1, 0, 0); }
    void draw_mesh(const MeshDraw& draw) override {
        ++stats_.drawCalls; ++stats_.meshCalls;
        if (!frameActive_ || !boundPipeline_ || !boundPipeline_->graphicsPipeline) return;
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        if (vertex != resources_.end() && vertex->second.buffer != VK_NULL_HANDLE) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(recording_command(), 0, 1, &vertex->second.buffer, &offset);
        }
        if (index != resources_.end() && index->second.buffer != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(recording_command(), index->second.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(recording_command(), draw.indexCount, 1, 0, 0, 0);
        }
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (!frameActive_ || !boundPipeline_ || dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) {
            lastError_ = "Vulkan dispatch has invalid command state or zero group count";
            return false;
        }
        if (!boundPipeline_->pipeline.computeShader || !ensure_pipeline(*boundPipeline_)) {
            lastError_ = "Vulkan dispatch requires a compute shader pipeline";
            return false;
        }
        if (renderPassActive_) {
            vkCmdEndRenderPass(recording_command());
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
            currentRenderTarget_ = 0;
        }
        vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_COMPUTE, boundPipeline_->computePipeline);
        vkCmdDispatch(recording_command(), dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
        ++stats_.dispatchCalls;
        return true;
    }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (!frameActive_ || !boundPipeline_ || !boundPipeline_->graphicsPipeline ||
            vertex == resources_.end() || index == resources_.end() || arguments == resources_.end() ||
            vertex->second.buffer == VK_NULL_HANDLE || index->second.buffer == VK_NULL_HANDLE ||
            arguments->second.buffer == VK_NULL_HANDLE || draw.maxDrawCount == 0 ||
            draw.stride < sizeof(std::uint32_t) * 5) {
            lastError_ = "Vulkan indirect draw has invalid command state or buffer";
            return;
        }
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(recording_command(), 0, 1, &vertex->second.buffer, &offset);
        vkCmdBindIndexBuffer(recording_command(), index->second.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(recording_command(), arguments->second.buffer,
            draw.argumentOffset, draw.maxDrawCount, draw.stride);
        ++stats_.indirectDrawCalls;
    }
    void end_frame() override {
        if (!frameActive_) return;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) end_queue(activeQueueKind_, currentBatchIndex_, true, swapchain_ != VK_NULL_HANDLE);
        if (pendingCommandBuffer_ != VK_NULL_HANDLE) finalize_pending_queue_batch();
        if (batchSignals_.empty() && frameFence_ != VK_NULL_HANDLE) {
            vkQueueSubmit(graphicsQueue_, 0, nullptr, frameFence_);
        }
        frameActive_ = false;
    }
    void discard_frame() override {
        if (!frameActive_) return;
        ++stats_.discardedFrames;
        if (pendingCommandBuffer_ != VK_NULL_HANDLE && !finalize_pending_queue_batch()) return;
        const auto discardedQueue = activeQueue_ != VK_NULL_HANDLE ? activeQueue_ : graphicsQueue_;
        const bool discardPresent = swapchain_ != VK_NULL_HANDLE &&
            activeQueueKind_ == RenderQueue::Graphics && imageAvailable_ != VK_NULL_HANDLE;
        VkSemaphore discardSignal = VK_NULL_HANDLE;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) {
            if (renderPassActive_) {
                vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
            }
            if (discardPresent && swapchainPrepared_) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = 0;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.image = swapchainImages_[imageIndex_];
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            vkEndCommandBuffer(activeCommandBuffer_);
            std::vector<VkSemaphore> waits;
            std::vector<VkPipelineStageFlags> stages;
            if (currentBatchWaitsForAcquire_ && imageAvailable_ != VK_NULL_HANDLE) {
                waits.push_back(imageAvailable_);
                stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            for (const auto waitBatch : currentWaitBatches_) {
                if (waitBatch < batchSignals_.size() && batchSignals_[waitBatch] != VK_NULL_HANDLE) {
                    waits.push_back(batchSignals_[waitBatch]);
                    stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                }
            }
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
            submit.pWaitSemaphores = waits.data();
            submit.pWaitDstStageMask = stages.data();
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &activeCommandBuffer_;
            if (discardPresent) {
                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &discardSignal) == VK_SUCCESS) {
                    submit.signalSemaphoreCount = 1;
                    submit.pSignalSemaphores = &discardSignal;
                }
            }
            if (discardedQueue == VK_NULL_HANDLE || frameFence_ == VK_NULL_HANDLE ||
                vkQueueSubmit(discardedQueue, 1, &submit, frameFence_) != VK_SUCCESS) {
                lastError_ = "Vulkan discarded frame submission failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                if (discardSignal != VK_NULL_HANDLE) vkDestroySemaphore(device_, discardSignal, nullptr);
                discardSignal = VK_NULL_HANDLE;
            }
            activeCommandBuffer_ = VK_NULL_HANDLE;
        } else if (discardedQueue != VK_NULL_HANDLE && frameFence_ != VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (discardPresent) vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &discardSignal);
            VkSemaphore waits[] = {imageAvailable_};
            VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = discardPresent ? 1u : 0u;
            submit.pWaitSemaphores = discardPresent ? waits : nullptr;
            submit.pWaitDstStageMask = discardPresent ? stages : nullptr;
            submit.signalSemaphoreCount = discardSignal != VK_NULL_HANDLE ? 1u : 0u;
            submit.pSignalSemaphores = discardSignal != VK_NULL_HANDLE ? &discardSignal : nullptr;
            if (vkQueueSubmit(discardedQueue, 1, &submit, frameFence_) != VK_SUCCESS) {
                lastError_ = "Vulkan discarded empty frame submission failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                if (discardSignal != VK_NULL_HANDLE) vkDestroySemaphore(device_, discardSignal, nullptr);
                discardSignal = VK_NULL_HANDLE;
            }
        }
        if (discardSignal != VK_NULL_HANDLE) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &discardSignal;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &imageIndex_;
            const auto presentResult = vkQueuePresentKHR(presentQueue_, &present);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
                lastError_ = "Vulkan discarded frame present failed result=" + std::to_string(static_cast<int>(presentResult));
                capabilities_.deviceState = presentResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            }
            batchSignals_.push_back(discardSignal);
            discardSignal = VK_NULL_HANDLE;
        }
        activeQueue_ = VK_NULL_HANDLE;
        currentBatchWaitsForAcquire_ = false;
        currentWaitBatches_.clear();
        swapchainPrepared_ = false;
        renderPassActive_ = false;
        activeRenderPass_ = VK_NULL_HANDLE;
        frameActive_ = false;
        boundPipeline_ = nullptr;
        boundMaterial_ = nullptr;
    }
    void wait_idle() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            collect_garbage();
        }
    }
    RenderStats stats() const noexcept override { return stats_; }
};
#endif

std::unique_ptr<IRenderBackend> create_backend(BackendApi api) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (api == BackendApi::DirectX11) return std::make_unique<DirectX11Backend>();
    if (api == BackendApi::DirectX12) return std::make_unique<DirectX12Backend>();
#endif
#if defined(SHINKOU_WITH_VULKAN)
    if (api == BackendApi::Vulkan) return std::make_unique<VulkanBackend>();
#endif
    return std::make_unique<NullBackend>(api);
}
}
