#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#if defined(SHINKOU_WITH_IMGUI)
#include <backends/imgui_impl_dx11.h>
#include <imgui.h>
#endif
#endif
#include "DxgiPipelineState.h"
#include "EditorGpuCapture.h"
#include "NativeTextureCache.h"
#include "shinkou/ui/Performance.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
namespace shinkou::render {
namespace {
class DirectX11Backend final : public IRenderBackend {
    static D3D11_CULL_MODE cull_mode(std::string_view mode) {
        if (mode == "front")
            return D3D11_CULL_FRONT;
        if (mode == "none")
            return D3D11_CULL_NONE;
        return D3D11_CULL_BACK;
    }
    static D3D11_FILL_MODE fill_mode(std::string_view mode) {
        return mode == "wireframe" ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    }
    static D3D11_PRIMITIVE_TOPOLOGY topology(std::string_view mode) {
        if (mode == "line")
            return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        if (mode == "point")
            return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        ID3D11Resource *resource{nullptr};
        ID3D11ShaderResourceView *srv{nullptr};
        ID3D11UnorderedAccessView *uav{nullptr};
        ID3D11RenderTargetView *rtv{nullptr};
        ID3D11DepthStencilView *dsv{nullptr};
        ID3D11SamplerState *sampler{nullptr};
        ID3D11VertexShader *vertexShader{nullptr};
        ID3D11PixelShader *pixelShader{nullptr};
        ID3D11ComputeShader *computeShader{nullptr};
        ID3D11InputLayout *inputLayout{nullptr};
        std::vector<std::uint8_t> shaderBytecode;
        ID3D11BlendState *blendState{nullptr};
        ID3D11DepthStencilState *depthState{nullptr};
        ID3D11RasterizerState *rasterState{nullptr};
        UINT stride{0};
        bool indexBuffer{false};
        bool constantBuffer{false};
        std::size_t logicalSize{0};
        std::size_t size{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t mipLevels{1};
        std::uint32_t layers{1};
        std::uint32_t sampleCount{1};
        bool generateMips{false};
        ResourceUsage usage{ResourceUsage::Unknown};
        bool alias{false};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<ShaderBinding> shaderBindings;
        ResourceRecord() = default;
        ResourceRecord(const ResourceRecord &) = delete;
        ResourceRecord &operator=(const ResourceRecord &) = delete;
        ResourceRecord(ResourceRecord &&other) noexcept
            : kind(other.kind), resource(other.resource), srv(other.srv), uav(other.uav),
              rtv(other.rtv), dsv(other.dsv), sampler(other.sampler),
              vertexShader(other.vertexShader), pixelShader(other.pixelShader),
              computeShader(other.computeShader), inputLayout(other.inputLayout),
              shaderBytecode(std::move(other.shaderBytecode)), blendState(other.blendState),
              depthState(other.depthState), rasterState(other.rasterState), stride(other.stride),
              indexBuffer(other.indexBuffer), constantBuffer(other.constantBuffer),
              logicalSize(other.logicalSize), size(other.size), format(other.format),
              width(other.width), height(other.height), mipLevels(other.mipLevels),
              layers(other.layers), sampleCount(other.sampleCount),
              generateMips(other.generateMips), usage(other.usage), alias(other.alias),
              pipeline(std::move(other.pipeline)), material(std::move(other.material)),
              shaderBindings(std::move(other.shaderBindings)) {
            other.resource = nullptr;
            other.srv = nullptr;
            other.uav = nullptr;
            other.rtv = nullptr;
            other.dsv = nullptr;
            other.sampler = nullptr;
            other.vertexShader = nullptr;
            other.pixelShader = nullptr;
            other.computeShader = nullptr;
            other.inputLayout = nullptr;
            other.blendState = nullptr;
            other.depthState = nullptr;
            other.rasterState = nullptr;
            other.alias = false;
            other.logicalSize = 0;
        }
        ~ResourceRecord() {
            if (computeShader)
                computeShader->Release();
            if (sampler)
                sampler->Release();
            if (depthState)
                depthState->Release();
            if (rasterState)
                rasterState->Release();
            if (inputLayout)
                inputLayout->Release();
            if (blendState)
                blendState->Release();
            if (pixelShader)
                pixelShader->Release();
            if (vertexShader)
                vertexShader->Release();
            if (rtv)
                rtv->Release();
            if (dsv)
                dsv->Release();
            if (srv)
                srv->Release();
            if (uav)
                uav->Release();
            if (resource)
                resource->Release();
        }
    };
    ID3D11Device *device_{nullptr};
    ID3D11DeviceContext *context_{nullptr};
    ID3D11InfoQueue *infoQueue_{nullptr};
    IDXGISwapChain *swapChain_{nullptr};
    ID3D11RenderTargetView *renderTarget_{nullptr};
    ID3D11Texture2D *depthBuffer_{nullptr};
    ID3D11DepthStencilView *depthView_{nullptr};
    bool vsync_{true};
    std::uint32_t width_{1280};
    std::uint32_t height_{720};
    std::uint32_t currentRenderTarget_{0};
    std::uint32_t currentDepthTarget_{0};
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    std::unordered_map<ID3D11Resource *, ResourceUsage> resourceUsages_;
    std::vector<bool> submittedQueueBatches_;
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord *boundPipeline_{nullptr};
    ResourceRecord *boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::string lastError_;
    std::unordered_map<std::uint32_t, std::string> debugNames_;
    bool uiCaptureWritten_{false};
    EditorViewportSeam editorViewport_{};
#if defined(SHINKOU_WITH_IMGUI)
    void (*imguiRenderWindow_)(ImGuiViewport *, void *){nullptr};
#endif
    void capture_ui_surface_once(std::uintptr_t window = 0, ID3D11Texture2D *surface = nullptr) {
        if (uiCaptureWritten_ || !device_ || !context_)
            return;
        const auto request = editor_gpu_capture_request();
        if (!request || request->window != window)
            return;
        CaptureCom<ID3D11Texture2D> source, staging;
        if (surface) {
            surface->AddRef();
            source.value = surface;
        } else if (!swapChain_ || FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(source.put()))))
            return;
        D3D11_TEXTURE2D_DESC description{};
        source.get()->GetDesc(&description);
        const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                          description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if ((!rgba && !bgra) || description.SampleDesc.Count != 1)
            return;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.BindFlags = description.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&description, nullptr, staging.put())))
            return;
        context_->CopyResource(staging.get(), source.get());
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return;
        TextureReadback pixels;
        pixels.width = description.Width;
        pixels.height = description.Height;
        pixels.rowPitch = static_cast<std::size_t>(pixels.width) * 4;
        pixels.data.resize(pixels.rowPitch * pixels.height);
        for (std::uint32_t row = 0; row < pixels.height; ++row)
            std::memcpy(pixels.data.data() + row * pixels.rowPitch,
                        static_cast<const std::uint8_t *>(mapped.pData) + row * mapped.RowPitch,
                        pixels.rowPitch);
        context_->Unmap(staging.get(), 0);
        uiCaptureWritten_ = write_editor_gpu_capture(*request, pixels, rgba);
    }
    void append_debug_messages() {
        if (!infoQueue_)
            return;
        const auto messageCount =
            std::min<std::size_t>(infoQueue_->GetNumStoredMessagesAllowedByRetrievalFilter(), 32u);
        for (std::size_t index = 0; index < messageCount; ++index) {
            SIZE_T messageBytes = 0;
            if (FAILED(infoQueue_->GetMessage(index, nullptr, &messageBytes)) || messageBytes == 0)
                continue;
            std::vector<std::uint8_t> storage(messageBytes);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            if (FAILED(infoQueue_->GetMessage(index, message, &messageBytes)) ||
                !message->pDescription)
                continue;
            if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
                message->Severity == D3D11_MESSAGE_SEVERITY_ERROR) {
                lastError_ += " D3D11 debug: ";
                lastError_ += message->pDescription;
            }
        }
        infoQueue_->ClearStoredMessages();
    }

  public:
    ~DirectX11Backend() override {
        transientTextures_.clear([](auto &) {});
        resources_.clear();
        if (infoQueue_)
            infoQueue_->Release();
        if (context_)
            context_->Release();
        if (depthView_)
            depthView_->Release();
        if (depthBuffer_)
            depthBuffer_->Release();
        if (renderTarget_)
            renderTarget_->Release();
        if (swapChain_)
            swapChain_->Release();
        if (device_)
            device_->Release();
    }
    RenderCapabilities capabilities() const noexcept override {
        return capabilities_;
    }
    std::string last_error() const override {
        return lastError_;
    }
    void clear_error() override {
        lastError_.clear();
    }
#if defined(SHINKOU_WITH_IMGUI) && defined(SHINKOU_PLATFORM_WINDOWS)
    bool initialize_imgui() override {
        if (!device_ || !context_ || !ImGui::GetCurrentContext() ||
            !ImGui_ImplDX11_Init(device_, context_))
            return false;
        auto &platform = ImGui::GetPlatformIO();
        imguiRenderWindow_ = platform.Renderer_RenderWindow;
        platform.Renderer_RenderWindow = [](ImGuiViewport *viewport, void *renderer) {
            auto *self = static_cast<DirectX11Backend *>(renderer);
            self->imguiRenderWindow_(viewport, nullptr);
            if (!self->uiCaptureWritten_ && std::getenv("SHINKOU_UI_CAPTURE_PATH")) {
                ID3D11RenderTargetView *target = nullptr;
                self->context_->OMGetRenderTargets(1, &target, nullptr);
                if (target) {
                    ID3D11Resource *resource = nullptr;
                    target->GetResource(&resource);
                    target->Release();
                    ID3D11Texture2D *texture = nullptr;
                    if (resource) {
                        resource->QueryInterface(IID_PPV_ARGS(&texture));
                        resource->Release();
                    }
                    if (texture) {
                        self->capture_ui_surface_once(
                            reinterpret_cast<std::uintptr_t>(viewport->PlatformHandleRaw), texture);
                        texture->Release();
                    }
                }
            }
        };
        return true;
    }
    void new_imgui_frame() override {
        ImGui_ImplDX11_NewFrame();
    }
    void invalidate_imgui_fonts() override {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    std::uint64_t imgui_texture(ResourceHandle handle) override {
        const auto found = resources_.find(handle.id);
        return found != resources_.end() ? reinterpret_cast<std::uint64_t>(found->second.srv) : 0;
    }
    void shutdown_imgui() override {
        if (ImGui::GetCurrentContext())
            ImGui_ImplDX11_Shutdown();
    }
    void render_imgui(ImDrawData *drawData) override {
        if (!drawData || !context_ || !renderTarget_)
            return;
        context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
        ImGui_ImplDX11_RenderDrawData(drawData);
        capture_ui_surface_once();
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::RenderPlatformWindowsDefault(nullptr, this);
        }
    }
#endif
    void request_ui_capture() override {
        uiCaptureWritten_ = false;
    }
    bool initialize(const RenderBackendConfig &config) override {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = E_FAIL;
        IDXGIAdapter1 *adapter = select_high_performance_adapter();
        const char *forceWarpValue = std::getenv("SHINKOU_D3D11_WARP");
        const bool forceWarp = forceWarpValue && (_stricmp(forceWarpValue, "1") == 0 ||
                                                  _stricmp(forceWarpValue, "true") == 0 ||
                                                  _stricmp(forceWarpValue, "yes") == 0);
        const char *debugValue = std::getenv("SHINKOU_D3D11_DEBUG");
        const UINT deviceFlags =
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
            ((debugValue && (_stricmp(debugValue, "1") == 0 || _stricmp(debugValue, "true") == 0 ||
                             _stricmp(debugValue, "yes") == 0))
                 ? static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG)
                 : 0u);
        if (forceWarp && adapter) {
            adapter->Release();
            adapter = nullptr;
        }
        if (config.nativeWindow) {
            DXGI_SWAP_CHAIN_DESC swapDesc{};
            swapDesc.BufferCount = 2;
            swapDesc.BufferDesc.Width = config.width;
            swapDesc.BufferDesc.Height = config.height;
            // Match the Win32 swap chain's native BGRA8 presentation layout.
            swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDesc.OutputWindow = static_cast<HWND>(config.nativeWindow);
            swapDesc.SampleDesc.Count = 1;
            swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            swapDesc.Windowed = TRUE;
            result = D3D11CreateDeviceAndSwapChain(
                adapter,
                forceWarp ? D3D_DRIVER_TYPE_WARP
                          : (adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE),
                nullptr, deviceFlags, levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, &swapDesc, &swapChain_, &device_, &selected, &context_);
            if (FAILED(result)) {
                if (context_) {
                    context_->Release();
                    context_ = nullptr;
                }
                if (device_) {
                    device_->Release();
                    device_ = nullptr;
                }
                if (swapChain_) {
                    swapChain_->Release();
                    swapChain_ = nullptr;
                }
                // WARP keeps the editor visible on machines where the
                // hardware adapter cannot create a swapchain (old drivers,
                // remote sessions, or low-end integrated GPUs). A device
                // without a swapchain is not a usable windowed backend.
                result = D3D11CreateDeviceAndSwapChain(
                    nullptr, D3D_DRIVER_TYPE_WARP, nullptr, deviceFlags, levels, 1,
                    D3D11_SDK_VERSION, &swapDesc, &swapChain_, &device_, &selected, &context_);
            }
        } else {
            result = D3D11CreateDevice(
                adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, &device_, &selected, &context_);
        }
        if (adapter)
            adapter->Release();
        if (FAILED(result)) {
            lastError_ = "D3D11 device/swapchain creation failed hr=" +
                         std::to_string(static_cast<long long>(result));
            return false;
        }
        if (debugValue && device_) {
            device_->QueryInterface(__uuidof(ID3D11InfoQueue),
                                    reinterpret_cast<void **>(&infoQueue_));
        }
        vsync_ = config.vsync;
        width_ = config.width;
        height_ = config.height;
        if (swapChain_) {
            ID3D11Texture2D *backBuffer = nullptr;
            if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
                return false;
            result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
            backBuffer->Release();
            if (FAILED(result))
                return false;
            if (!create_depth_target(config.width, config.height))
                return false;
        }
        capabilities_.api = BackendApi::DirectX11;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
        capabilities_.supportsImGui = false;
#if defined(SHINKOU_WITH_IMGUI)
        capabilities_.supportsImGui = true;
#endif
        capabilities_.supportsEditorViewportScissor = true;
        capabilities_.supportsEditorOffscreenTarget = true;
        capabilities_.supportsEditorModelRendering = true;
        capabilities_.supportsTextureReadback = true;
        capabilities_.supportsInPassTextureReadback = true;
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
        if (!swapChain_ || !device_ || !context_)
            return true;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (renderTarget_) {
            renderTarget_->Release();
            renderTarget_ = nullptr;
        }
        if (depthView_) {
            depthView_->Release();
            depthView_ = nullptr;
        }
        if (depthBuffer_) {
            depthBuffer_->Release();
            depthBuffer_ = nullptr;
        }
        const auto resizeResult =
            swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeResult)) {
            lastError_ = "D3D11 swapchain resize failed hr=" +
                         std::to_string(static_cast<long long>(resizeResult));
            capabilities_.deviceState =
                resizeResult == DXGI_ERROR_DEVICE_REMOVED || resizeResult == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
            return false;
        }
        ID3D11Texture2D *backBuffer = nullptr;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return false;
        const auto result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
        if (SUCCEEDED(result)) {
            width_ = width;
            height_ = height;
        }
        const auto resized = SUCCEEDED(result) && create_depth_target(width, height);
        if (resized)
            capabilities_.deviceState = RenderDeviceState::Ready;
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
        if (FAILED(device_->CreateTexture2D(&depth, nullptr, &depthBuffer_)))
            return false;
        return SUCCEEDED(device_->CreateDepthStencilView(depthBuffer_, nullptr, &depthView_));
    }
    static DXGI_FORMAT format_of(std::string_view format) {
        if (format == "bgra8")
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        if (format == "rgba16f")
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (format == "r32f")
            return DXGI_FORMAT_R32_FLOAT;
        if (format == "d24s8")
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (format == "d32f")
            return DXGI_FORMAT_D32_FLOAT;
        if (format == "bc1")
            return DXGI_FORMAT_BC1_UNORM;
        if (format == "bc3")
            return DXGI_FORMAT_BC3_UNORM;
        if (format == "bc5")
            return DXGI_FORMAT_BC5_UNORM;
        if (format == "bc6h")
            return DXGI_FORMAT_BC6H_UF16;
        if (format == "bc7")
            return DXGI_FORMAT_BC7_UNORM;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    static std::size_t bytes_per_pixel(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    }
    static bool block_compressed_format(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC3_UNORM ||
               format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC6H_UF16 ||
               format == DXGI_FORMAT_BC7_UNORM;
    }
    static UINT depth_clear_flags(DXGI_FORMAT format) noexcept {
        return static_cast<UINT>(D3D11_CLEAR_DEPTH) |
               (format == DXGI_FORMAT_D24_UNORM_S8_UINT ? static_cast<UINT>(D3D11_CLEAR_STENCIL)
                                                        : 0u);
    }
    NativeTextureCache<ResourceRecord> transientTextures_;
    bool create_transient_resource(ResourceHandle handle,
                                   const ResourceDesc &description) override {
        if (auto reused = transientTextures_.take(handle, description)) {
            destroy_resource(handle);
            resources_.emplace(handle.id, std::move(*reused));
            ++stats_.transientTextureReuses;
            return true;
        }
        if (!create_resource(handle, description))
            return false;
        transientTextures_.track(handle, description);
        return true;
    }
    void retire_transient_resource(ResourceHandle handle, bool reusable) override {
        const auto found = resources_.find(handle.id);
        if (reusable && found != resources_.end() && !found->second.alias &&
            transientTextures_.recycle(handle, found->second)) {
            resources_.erase(found);
            return;
        }
        transientTextures_.forget(handle);
        retire_resource(handle);
    }
    bool create_resource(ResourceHandle handle, const ResourceDesc &description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "D3D11 resource handle and description kind do not match";
            return false;
        }
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        bool pipelineValidationFailed = false;
        bool resourceCreationFailed = false;
        std::visit(
            [&](const auto &desc) {
                using T = std::decay_t<decltype(desc)>;
                if constexpr (std::is_same_v<T, TextureDesc>) {
                    TexturePhysicalImagePlan plan;
                    if (!prepare_backend_texture(desc, BackendApi::DirectX11, plan, lastError_))
                        return;
                    D3D11_TEXTURE2D_DESC texture{};
                    texture.Width = plan.width;
                    texture.Height = plan.height;
                    texture.MipLevels = plan.mipLevels;
                    texture.ArraySize = plan.physicalArrayLayers;
                    texture.Format = format_of(texture_format_name(plan.format));
                    texture.SampleDesc.Count = plan.sampleCount;
                    texture.Usage = D3D11_USAGE_DEFAULT;
                    const bool depth = handle.kind == ResourceKind::DepthStencil ||
                                       texture_format_is_depth(plan.format);
                    const bool generateMips = desc.generateMips && plan.mipLevels > 1 && !depth;
                    if (generateMips)
                        texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                    const auto viewFormat = texture.Format;
                    if (depth)
                        texture.Format = viewFormat == DXGI_FORMAT_D32_FLOAT
                                             ? DXGI_FORMAT_R32_TYPELESS
                                             : DXGI_FORMAT_R24G8_TYPELESS;
                    texture.BindFlags = depth
                                            ? D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE
                                            : D3D11_BIND_SHADER_RESOURCE;
                    if (desc.storage && !depth)
                        texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                    if (desc.renderTarget)
                        texture.BindFlags |= D3D11_BIND_RENDER_TARGET;
                    if (generateMips)
                        texture.BindFlags |= D3D11_BIND_RENDER_TARGET;
                    record.generateMips = generateMips;
                    record.sampleCount = plan.sampleCount;
                    record.format = viewFormat;
                    record.width = texture.Width;
                    record.height = texture.Height;
                    record.mipLevels = texture.MipLevels;
                    record.layers = texture.ArraySize;
                    // Do not pass a single D3D11_SUBRESOURCE_DATA record here.
                    // CreateTexture2D expects one record per mip/layer, while the
                    // public TextureDesc upload represents the base subresource.
                    // Upload it through update_texture after the resource exists;
                    // this avoids native reads past the caller's byte vector for
                    // arrays and mipmapped textures.
                    ID3D11Texture2D *native = nullptr;
                    if (SUCCEEDED(device_->CreateTexture2D(&texture, nullptr, &native))) {
                        record.resource = native;
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = depth ? (viewFormat == DXGI_FORMAT_D32_FLOAT
                                                  ? DXGI_FORMAT_R32_FLOAT
                                                  : DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
                                           : viewFormat;
                        if (texture.SampleDesc.Count > 1) {
                            srv.ViewDimension = texture.ArraySize > 1
                                                    ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                    : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                            if (texture.ArraySize > 1)
                                srv.Texture2DMSArray.ArraySize = texture.ArraySize;
                        } else if (texture.ArraySize > 1) {
                            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                            srv.Texture2DArray.ArraySize = texture.ArraySize;
                            srv.Texture2DArray.MipLevels = texture.MipLevels;
                        } else {
                            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            srv.Texture2D.MipLevels = texture.MipLevels;
                        }
                        if (FAILED(device_->CreateShaderResourceView(native, &srv, &record.srv)))
                            resourceCreationFailed = true;
                        if (desc.storage && !depth)
                            device_->CreateUnorderedAccessView(native, nullptr, &record.uav);
                        if (desc.renderTarget && !depth)
                            device_->CreateRenderTargetView(native, nullptr, &record.rtv);
                        if (depth) {
                            D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
                            dsv.Format = viewFormat;
                            if (texture.SampleDesc.Count > 1) {
                                dsv.ViewDimension = texture.ArraySize > 1
                                                        ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY
                                                        : D3D11_DSV_DIMENSION_TEXTURE2DMS;
                                if (texture.ArraySize > 1)
                                    dsv.Texture2DMSArray.ArraySize = texture.ArraySize;
                            } else if (texture.ArraySize > 1) {
                                dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                                dsv.Texture2DArray.ArraySize = texture.ArraySize;
                            } else
                                dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                            if (FAILED(device_->CreateDepthStencilView(native, &dsv, &record.dsv)))
                                resourceCreationFailed = true;
                        }
                    }
                } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                    D3D11_SAMPLER_DESC sampler{};
                    sampler.Filter = desc.filter == "nearest" ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                                              : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    sampler.AddressU = desc.addressU == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP
                                                                : D3D11_TEXTURE_ADDRESS_WRAP;
                    sampler.AddressV = desc.addressV == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP
                                                                : D3D11_TEXTURE_ADDRESS_WRAP;
                    sampler.AddressW = desc.addressW == "clamp" ? D3D11_TEXTURE_ADDRESS_CLAMP
                                                                : D3D11_TEXTURE_ADDRESS_WRAP;
                    sampler.MaxAnisotropy = static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy));
                    sampler.ComparisonFunc =
                        desc.compareEnable ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
                    sampler.MinLOD = 0.0f;
                    sampler.MaxLOD = D3D11_FLOAT32_MAX;
                    device_->CreateSamplerState(&sampler, &record.sampler);
                } else if constexpr (std::is_same_v<T, BufferDesc>) {
                    if (desc.size > std::numeric_limits<UINT>::max() ||
                        desc.stride > std::numeric_limits<UINT>::max()) {
                        lastError_ = "D3D11 buffer dimensions exceed API limits";
                        resourceCreationFailed = true;
                        return;
                    }
                    record.stride = static_cast<UINT>(desc.stride);
                    record.indexBuffer = desc.indexBuffer;
                    record.logicalSize = desc.size;
                    record.size = desc.size;
                    record.constantBuffer = desc.uniformBuffer && !desc.vertexBuffer &&
                                            !desc.indexBuffer && !desc.indirectBuffer &&
                                            !desc.structuredBuffer && !desc.storageBuffer;
                    D3D11_BUFFER_DESC buffer{};
                    buffer.ByteWidth = static_cast<UINT>(std::max<std::size_t>(desc.size, 1));
                    buffer.Usage = D3D11_USAGE_DEFAULT;
                    if (desc.vertexBuffer)
                        buffer.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
                    if (desc.indexBuffer)
                        buffer.BindFlags |= D3D11_BIND_INDEX_BUFFER;
                    if (desc.indirectBuffer)
                        buffer.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                    if (desc.structuredBuffer && desc.stride != 0) {
                        buffer.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                        buffer.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                        buffer.StructureByteStride = static_cast<UINT>(desc.stride);
                    }
                    if (desc.storageBuffer)
                        buffer.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                    // D3D11 needs an explicit CONSTANT_BUFFER bind flag for
                    // buffers passed to bind_uniform_buffer(). Keep ordinary
                    // untyped upload buffers as ordinary buffers.
                    if (record.constantBuffer) {
                        buffer.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
                        if (buffer.ByteWidth > std::numeric_limits<UINT>::max() - 15u) {
                            lastError_ = "D3D11 constant buffer size exceeds API limits";
                            resourceCreationFailed = true;
                            return;
                        }
                        buffer.ByteWidth = static_cast<UINT>((buffer.ByteWidth + 15u) & ~15u);
                        record.size = buffer.ByteWidth;
                    }
                    D3D11_SUBRESOURCE_DATA data{};
                    std::vector<std::uint8_t> alignedInitialData;
                    if (!desc.initialData.empty()) {
                        if (desc.initialData.size() > desc.size) {
                            lastError_ =
                                "D3D11 buffer initial data does not cover the native resource";
                            resourceCreationFailed = true;
                            return;
                        }
                        if (desc.initialData.size() < buffer.ByteWidth) {
                            alignedInitialData.resize(buffer.ByteWidth, 0);
                            std::copy(desc.initialData.begin(), desc.initialData.end(),
                                      alignedInitialData.begin());
                            data.pSysMem = alignedInitialData.data();
                        } else {
                            data.pSysMem = desc.initialData.data();
                        }
                    }
                    ID3D11Buffer *native = nullptr;
                    if (SUCCEEDED(device_->CreateBuffer(&buffer, data.pSysMem ? &data : nullptr,
                                                        &native)) &&
                        native) {
                        record.resource = native;
                        if (desc.structuredBuffer && desc.stride != 0) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                            view.Format = DXGI_FORMAT_UNKNOWN;
                            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                            view.Buffer.NumElements = static_cast<UINT>(desc.size / desc.stride);
                            device_->CreateShaderResourceView(native, &view, &record.srv);
                        }
                        if (desc.storageBuffer)
                            device_->CreateUnorderedAccessView(native, nullptr, &record.uav);
                    }
                } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                    const auto compiled = shaderCompiler_->compile(desc, BackendApi::DirectX11);
                    if (!compiled.valid) {
                        lastError_ = compiled.diagnostics;
                        return;
                    }
                    if (!validate_backend_shader_layout(compiled.bindings, lastError_))
                        return;
                    record.shaderBindings = compiled.bindings;
                    record.shaderBytecode = compiled.bytecode;
                    if (desc.stage == ShaderStage::Vertex) {
                        device_->CreateVertexShader(compiled.bytecode.data(),
                                                    compiled.bytecode.size(), nullptr,
                                                    &record.vertexShader);
                    } else if (desc.stage == ShaderStage::Fragment) {
                        device_->CreatePixelShader(compiled.bytecode.data(),
                                                   compiled.bytecode.size(), nullptr,
                                                   &record.pixelShader);
                    } else {
                        device_->CreateComputeShader(compiled.bytecode.data(),
                                                     compiled.bytecode.size(), nullptr,
                                                     &record.computeShader);
                    }
                } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                    if (!is_supported_sample_count(desc.sampleCount) ||
                        desc.colorFormats.size() > 8 ||
                        !is_supported_color_format(desc.colorFormat) ||
                        !is_supported_pipeline_depth_format(desc.depthFormat) ||
                        std::any_of(desc.colorFormats.begin(), desc.colorFormats.end(),
                                    [](const std::string &format) {
                                        return !is_supported_color_format(format);
                                    }) ||
                        (desc.fillMode != "solid" && desc.fillMode != "wireframe") ||
                        (desc.cullMode != "back" && desc.cullMode != "front" &&
                         desc.cullMode != "none") ||
                        (desc.topology != "triangle" && desc.topology != "line" &&
                         desc.topology != "point")) {
                        lastError_ = "D3D11 pipeline fixed-function state is unsupported";
                        pipelineValidationFailed = true;
                        return;
                    }
                    if (!validate_pipeline_state(desc, lastError_)) {
                        pipelineValidationFailed = true;
                        return;
                    }
                    record.pipeline = desc;
                    record.pipeline.vertexLayout = resolve_vertex_layout(desc);
                    D3D11_RASTERIZER_DESC raster{};
                    raster.FillMode = fill_mode(desc.fillMode);
                    raster.CullMode = cull_mode(desc.cullMode);
                    fill_dxgi_raster(raster, desc.rasterizerState);
                    // Keep scissor enabled for every graphics pipeline. The
                    // editor viewport seam changes the active rectangle without
                    // replacing the pipeline's rasterizer state; begin_frame()
                    // restores a full-client scissor for normal game passes.
                    raster.ScissorEnable = TRUE;
                    device_->CreateRasterizerState(&raster, &record.rasterState);
                    D3D11_BLEND_DESC blend{};
                    const auto attachments = resolve_blend_attachments(desc);
                    blend.IndependentBlendEnable = attachments.size() > 1;
                    for (std::size_t index = 0; index < attachments.size(); ++index)
                        fill_dxgi_blend(blend.RenderTarget[index], attachments[index]);
                    device_->CreateBlendState(&blend, &record.blendState);
                    D3D11_DEPTH_STENCIL_DESC depth{};
                    fill_dxgi_depth(depth, resolve_depth_stencil(desc));
                    device_->CreateDepthStencilState(&depth, &record.depthState);
                    const auto layout = resolve_vertex_layout(desc);
                    if (!layout.attributes.empty()) {
                        const auto vertex = resources_.find(desc.vertexShader);
                        if (vertex != resources_.end() && !vertex->second.shaderBytecode.empty()) {
                            std::vector<D3D11_INPUT_ELEMENT_DESC> input;
                            for (const auto &attribute : layout.attributes) {
                                const auto binding =
                                    std::find_if(layout.bindings.begin(), layout.bindings.end(),
                                                 [&](const auto &value) {
                                                     return value.binding == attribute.binding;
                                                 });
                                input.push_back(
                                    {attribute.semantic.c_str(), attribute.semanticIndex,
                                     dxgi_vertex_format(attribute.format), attribute.binding,
                                     attribute.offset,
                                     binding->perInstance ? D3D11_INPUT_PER_INSTANCE_DATA
                                                          : D3D11_INPUT_PER_VERTEX_DATA,
                                     binding->perInstance ? binding->stepRate : 0});
                            }
                            const auto hr = device_->CreateInputLayout(
                                input.data(), static_cast<UINT>(input.size()),
                                vertex->second.shaderBytecode.data(),
                                vertex->second.shaderBytecode.size(), &record.inputLayout);
                            if (FAILED(hr)) {
                                lastError_ = "D3D11 vertex layout does not match the vertex shader";
                                pipelineValidationFailed = true;
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                    record.material = desc;
                }
            },
            description);
        if (pipelineValidationFailed || resourceCreationFailed)
            return false;
        resources_.emplace(handle.id, std::move(record));
        ++stats_.resourceCreates;
        if (const auto *texture = std::get_if<TextureDesc>(&description);
            texture && !texture->initialData.empty()) {
            const auto textureFormat = texture->formatKind == TextureFormat::Unknown
                                           ? texture_format_from_name(texture->format)
                                           : texture->formatKind;
            const auto uploaded =
                update_texture({handle, 0, 0, texture->width, texture->height,
                                static_cast<std::size_t>(texture->width) *
                                    bytes_per_pixel(format_of(texture_format_name(textureFormat))),
                                texture->initialData});
            if (!uploaded) {
                if (lastError_.empty())
                    lastError_ = "D3D11 initial texture upload failed";
                else
                    lastError_ = "D3D11 initial texture upload failed: " + lastError_;
                destroy_resource(handle);
                return false;
            } else if (texture->generateMips &&
                       (texture->mipLevels == 0 || texture->mipLevels > 1) &&
                       !generate_mips(handle)) {
                if (lastError_.empty())
                    lastError_ = "D3D11 initial texture mip generation failed";
                else
                    lastError_ = "D3D11 initial texture mip generation failed: " + lastError_;
                destroy_resource(handle);
                return false;
            }
        }
        if (const auto *texture = std::get_if<TextureDesc>(&description); texture) {
            for (const auto &subresource : texture->initialSubresources) {
                if (!update_texture({handle, subresource.mipLevel, subresource.layer,
                                     subresource.width, subresource.height, subresource.rowPitch,
                                     subresource.data})) {
                    if (lastError_.empty())
                        lastError_ = "D3D11 initial texture subresource upload failed";
                    else
                        lastError_ =
                            "D3D11 initial texture subresource upload failed: " + lastError_;
                    destroy_resource(handle);
                    return false;
                }
            }
        }
        const auto created = resources_.find(handle.id);
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil ||
              handle.kind == ResourceKind::Buffer) &&
             !created->second.resource)) {
            if (lastError_.empty())
                lastError_ = "D3D11 resource creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Sampler && !created->second.sampler) {
            if (lastError_.empty())
                lastError_ = "D3D11 sampler creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Shader && !created->second.vertexShader &&
            !created->second.pixelShader && !created->second.computeShader) {
            if (lastError_.empty())
                lastError_ = "D3D11 shader creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil) {
            const auto *texture = std::get_if<TextureDesc>(&description);
            const auto format = texture ? (texture->formatKind == TextureFormat::Unknown
                                               ? texture_format_from_name(texture->format)
                                               : texture->formatKind)
                                        : TextureFormat::Unknown;
            const bool depth =
                handle.kind == ResourceKind::DepthStencil || texture_format_is_depth(format);
            if (!created->second.resource || (depth && !created->second.dsv) ||
                (!depth && !created->second.srv) ||
                (texture && texture->storage && !depth && !created->second.uav) ||
                (texture && texture->renderTarget && !created->second.rtv)) {
                lastError_ = "D3D11 texture view creation failed";
                return false;
            }
        }
        if (handle.kind == ResourceKind::Pipeline) {
            const auto *pipeline = std::get_if<PipelineDesc>(&description);
            if (!created->second.rasterState || !created->second.blendState ||
                !created->second.depthState ||
                (pipeline && !created->second.pipeline.vertexLayout.attributes.empty() &&
                 !created->second.inputLayout)) {
                lastError_ = "D3D11 pipeline state creation failed";
                return false;
            }
        }
        if (created->second.resource)
            resourceUsages_[created->second.resource] = ResourceUsage::Unknown;
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind)
            return;
        if (boundPipeline_ == &it->second)
            boundPipeline_ = nullptr;
        if (!it->second.alias && it->second.resource)
            resourceUsages_.erase(it->second.resource);
        resources_.erase(it);
        ++stats_.resourceDestroys;
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() ||
            source->second.kind != physical.kind || source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D &&
             source->second.kind != ResourceKind::DepthStencil &&
             source->second.kind != ResourceKind::Buffer))
            return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.resource = source->second.resource;
        alias.srv = source->second.srv;
        alias.uav = source->second.uav;
        alias.rtv = source->second.rtv;
        alias.dsv = source->second.dsv;
        alias.stride = source->second.stride;
        alias.indexBuffer = source->second.indexBuffer;
        alias.constantBuffer = source->second.constantBuffer;
        alias.logicalSize = source->second.logicalSize;
        alias.size = source->second.size;
        alias.format = source->second.format;
        alias.width = source->second.width;
        alias.height = source->second.height;
        alias.mipLevels = source->second.mipLevels;
        alias.layers = source->second.layers;
        alias.generateMips = source->second.generateMips;
        alias.usage = source->second.usage;
        alias.alias = true;
        if (alias.resource)
            alias.resource->AddRef();
        if (alias.srv)
            alias.srv->AddRef();
        if (alias.uav)
            alias.uav->AddRef();
        if (alias.rtv)
            alias.rtv->AddRef();
        if (alias.dsv)
            alias.dsv->AddRef();
        resources_.emplace(logical.id, std::move(alias));
        return true;
    }
    bool update_buffer(const BufferUpdate &update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() ||
            it->second.kind != ResourceKind::Buffer) {
            lastError_ = "D3D11 buffer handle or resource kind is invalid";
            return false;
        }
        if (!it->second.resource) {
            lastError_ = "D3D11 buffer has no native resource";
            return false;
        }
        const auto logicalSize =
            it->second.constantBuffer ? it->second.logicalSize : it->second.size;
        if (update.data.empty() || update.offset > logicalSize ||
            update.data.size() > logicalSize - update.offset) {
            lastError_ = "D3D11 buffer update exceeds resource size";
            return false;
        }
        if (it->second.constantBuffer) {
            // D3D11 drops constant-buffer updates that specify a destination
            // region. Constant buffers must be replaced as one aligned block.
            if (update.offset != 0 || update.data.size() != it->second.logicalSize) {
                lastError_ = "D3D11 constant-buffer updates must cover the complete logical buffer";
                return false;
            }
            if (it->second.logicalSize == it->second.size) {
                context_->UpdateSubresource(it->second.resource, 0, nullptr, update.data.data(), 0,
                                            0);
            } else {
                std::vector<std::uint8_t> aligned(it->second.size, 0);
                std::copy(update.data.begin(), update.data.end(), aligned.begin());
                context_->UpdateSubresource(it->second.resource, 0, nullptr, aligned.data(), 0, 0);
            }
            return true;
        }
        D3D11_BOX box{};
        box.left = static_cast<UINT>(update.offset);
        box.right = static_cast<UINT>(update.offset + update.data.size());
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;
        context_->UpdateSubresource(it->second.resource, 0, &box, update.data.data(),
                                    static_cast<UINT>(update.data.size()), 0);
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() ||
            it->second.kind != ResourceKind::Texture2D || !it->second.generateMips ||
            !it->second.srv || !context_) {
            lastError_ = "D3D11 mip generation handle or resource is invalid";
            return false;
        }
        context_->GenerateMips(it->second.srv);
        return true;
    }
    bool update_texture(const TextureUpdate &update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() ||
            it->second.kind != ResourceKind::Texture2D || !it->second.resource ||
            update.data.empty() || update.width == 0 || update.height == 0) {
            lastError_ = "D3D11 texture handle or update data is invalid";
            return false;
        }
        auto *texture = static_cast<ID3D11Texture2D *>(it->second.resource);
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (block_compressed_format(description.Format)) {
            lastError_ =
                "D3D11 compressed texture updates require block-compressed upload metadata";
            return false;
        }
        if (update.mipLevel >= description.MipLevels || update.layer >= description.ArraySize ||
            update.x > std::max(1u, description.Width >> update.mipLevel) ||
            update.y > std::max(1u, description.Height >> update.mipLevel) ||
            update.width > std::max(1u, description.Width >> update.mipLevel) - update.x ||
            update.height > std::max(1u, description.Height >> update.mipLevel) - update.y)
            return false;
        D3D11_BOX box{update.x, update.y, 0, update.x + update.width, update.y + update.height, 1};
        const auto pixelSize = bytes_per_pixel(description.Format);
        const auto pitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * pixelSize
                                                : update.rowPitch;
        if (pitch < static_cast<std::size_t>(update.width) * pixelSize ||
            pitch > std::numeric_limits<std::size_t>::max() / update.height ||
            pitch * update.height > update.data.size() ||
            pitch > std::numeric_limits<UINT>::max()) {
            lastError_ = "D3D11 texture update pitch exceeds resource limits";
            return false;
        }
        context_->UpdateSubresource(
            it->second.resource,
            D3D11CalcSubresource(update.mipLevel, update.layer, description.MipLevels), &box,
            update.data.data(), static_cast<UINT>(pitch), 0);
        return true;
    }
    bool resolve_texture(ResourceHandle destination, ResourceHandle source) override {
        const auto destinationIt = resources_.find(destination.id);
        const auto sourceIt = resources_.find(source.id);
        if (!context_ || destination.kind != ResourceKind::Texture2D ||
            source.kind != ResourceKind::Texture2D || destinationIt == resources_.end() ||
            sourceIt == resources_.end() || !destinationIt->second.resource ||
            !sourceIt->second.resource || destination.id == source.id) {
            lastError_ = "D3D11 resolve requires two valid texture resources";
            return false;
        }
        auto *destinationTexture = static_cast<ID3D11Texture2D *>(destinationIt->second.resource);
        auto *sourceTexture = static_cast<ID3D11Texture2D *>(sourceIt->second.resource);
        D3D11_TEXTURE2D_DESC destinationDescription{};
        D3D11_TEXTURE2D_DESC sourceDescription{};
        destinationTexture->GetDesc(&destinationDescription);
        sourceTexture->GetDesc(&sourceDescription);
        if (sourceDescription.SampleDesc.Count <= 1 ||
            destinationDescription.SampleDesc.Count != 1 ||
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
                const auto subresource =
                    D3D11CalcSubresource(mip, layer, sourceDescription.MipLevels);
                context_->ResolveSubresource(destinationIt->second.resource, subresource,
                                             sourceIt->second.resource, subresource,
                                             sourceDescription.Format);
            }
        }
        return true;
    }
    bool read_texture(const TextureReadbackRequest &request, TextureReadback &result) override {
        result = {};
        const auto it = resources_.find(request.texture.id);
        if (!context_ || request.texture.kind != ResourceKind::Texture2D ||
            it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            !it->second.resource || request.maxBytes == 0) {
            lastError_ = "D3D11 texture readback handle or request is invalid";
            return false;
        }
        auto *source = static_cast<ID3D11Texture2D *>(it->second.resource);
        D3D11_TEXTURE2D_DESC sourceDescription{};
        source->GetDesc(&sourceDescription);
        const auto supportedFormat = sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                     sourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                     sourceDescription.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                     sourceDescription.Format == DXGI_FORMAT_R32_FLOAT;
        if (sourceDescription.SampleDesc.Count != 1 ||
            request.mipLevel >= sourceDescription.MipLevels ||
            request.layer >= sourceDescription.ArraySize || !supportedFormat) {
            lastError_ = "D3D11 texture readback requires a single-sample supported color texture";
            return false;
        }
        const auto mipWidth = std::max(1u, sourceDescription.Width >> request.mipLevel);
        const auto mipHeight = std::max(1u, sourceDescription.Height >> request.mipLevel);
        const auto readWidth = request.width == 0 ? mipWidth : request.width;
        const auto readHeight = request.height == 0 ? mipHeight : request.height;
        if (request.x > mipWidth || request.y > mipHeight || readWidth == 0 || readHeight == 0 ||
            readWidth > mipWidth - request.x || readHeight > mipHeight - request.y) {
            lastError_ = "D3D11 texture readback region is outside the selected mip";
            return false;
        }
        const auto pixelSize = bytes_per_pixel(sourceDescription.Format);
        if (readWidth > std::numeric_limits<std::size_t>::max() / pixelSize ||
            readWidth * pixelSize > std::numeric_limits<std::size_t>::max() / readHeight ||
            readWidth * pixelSize * readHeight > request.maxBytes) {
            lastError_ = "D3D11 texture readback exceeds the requested byte limit";
            return false;
        }
        D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
        stagingDescription.Width = readWidth;
        stagingDescription.Height = readHeight;
        stagingDescription.MipLevels = 1;
        stagingDescription.ArraySize = 1;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.SampleDesc.Quality = 0;
        ID3D11Texture2D *staging = nullptr;
        if (FAILED(device_->CreateTexture2D(&stagingDescription, nullptr, &staging)) || !staging) {
            lastError_ = "D3D11 staging texture creation failed for readback";
            return false;
        }
        const D3D11_BOX sourceBox{
            request.x, request.y, 0, request.x + readWidth, request.y + readHeight, 1};
        const auto sourceSubresource =
            D3D11CalcSubresource(request.mipLevel, request.layer, sourceDescription.MipLevels);
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, source, sourceSubresource, &sourceBox);
        // A readback is an explicit synchronization point. Flush the
        // immediate context before Map so diagnostics never observe a stale
        // staging allocation on a deferred driver/WARP path.
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto mapResult = context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult) || !mapped.pData || mapped.RowPitch < readWidth * pixelSize) {
            if (SUCCEEDED(mapResult))
                context_->Unmap(staging, 0);
            staging->Release();
            lastError_ = "D3D11 staging texture map failed for readback";
            return false;
        }
        const auto packedPitch = static_cast<std::size_t>(readWidth) * pixelSize;
        result.width = readWidth;
        result.height = readHeight;
        result.rowPitch = packedPitch;
        result.data.resize(packedPitch * readHeight);
        for (std::uint32_t row = 0; row < readHeight; ++row) {
            std::memcpy(result.data.data() + static_cast<std::size_t>(row) * packedPitch,
                        static_cast<const std::uint8_t *>(mapped.pData) +
                            static_cast<std::size_t>(row) * mapped.RowPitch,
                        packedPitch);
        }
        context_->Unmap(staging, 0);
        staging->Release();
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind || !context_ ||
            !it->second.resource) {
            lastError_ = "D3D11 resource transition handle or device is invalid";
            return false;
        }
        const auto state = resourceUsages_.find(it->second.resource);
        const auto previous =
            state == resourceUsages_.end() ? ResourceUsage::Unknown : state->second;
        if (previous == usage)
            return true;
        const bool write =
            is_color_attachment_usage(usage) || usage == ResourceUsage::DepthStencil ||
            usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite ||
            usage == ResourceUsage::CopyDestination;
        ID3D11ShaderResourceView *nullSrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D11UnorderedAccessView *nullUavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        if (write) {
            context_->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                                           nullSrvs);
            context_->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                                           nullSrvs);
            context_->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                                           nullSrvs);
            context_->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, nullUavs,
                                                nullptr);
        } else {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, nullUavs,
                                                nullptr);
        }
        resourceUsages_[it->second.resource] = usage;
        ++stats_.barriers;
        return true;
    }
    void set_render_target(ResourceHandle target) override {
        if (!context_)
            return;
        if (target) {
            const auto it = resources_.find(target.id);
            if (target.kind != ResourceKind::Texture2D || it == resources_.end() ||
                !it->second.rtv) {
                lastError_ = "D3D11 render target handle has no valid render-target view";
                return;
            }
            context_->OMSetRenderTargets(1, &it->second.rtv, nullptr);
            currentRenderTarget_ = target.id;
            currentDepthTarget_ = 0;
            return;
        }
        if (renderTarget_) {
            context_->OMSetRenderTargets(1, &renderTarget_, depthView_);
            currentRenderTarget_ = 0;
            currentDepthTarget_ = 0;
        }
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        if (!context_)
            return;
        const bool editorBackbuffer = editorViewport_.enabled && editorViewport_.strictTarget;
        if (editorViewport_.enabled && !editorBackbuffer && color) {
            // The native editor composites an explicit offscreen scene into
            // its docked viewport. Keep the graph target and full texture extent.
            bind_editor_render_target(color, depth, false);
            return;
        }
        ID3D11RenderTargetView *rtv =
            editorBackbuffer ? renderTarget_ : (color ? renderTarget_ : nullptr);
        ID3D11DepthStencilView *dsv = nullptr;
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (!editorBackbuffer && color && colorIt != resources_.end() && colorIt->second.rtv)
            rtv = colorIt->second.rtv;
        if (!editorBackbuffer && color &&
            (color.kind != ResourceKind::Texture2D || colorIt == resources_.end() ||
             !colorIt->second.rtv)) {
            lastError_ = "D3D11 color target handle has no valid render-target view";
            return;
        }
        if (depth) {
            if (depth.kind != ResourceKind::DepthStencil || depthIt == resources_.end() ||
                !depthIt->second.dsv) {
                lastError_ = "D3D11 depth target handle has no valid depth-stencil view";
                return;
            }
            dsv = depthIt->second.dsv;
        }
        currentRenderTarget_ = color ? color.id : 0;
        currentDepthTarget_ = depth ? depth.id : 0;
        context_->OMSetRenderTargets(rtv ? 1u : 0u, rtv ? &rtv : nullptr, dsv);
        if (!editorBackbuffer) {
            const auto target = colorIt != resources_.end() ? colorIt : depthIt;
            if (target != resources_.end()) {
                D3D11_VIEWPORT viewport{0,
                                        0,
                                        static_cast<float>(target->second.width),
                                        static_cast<float>(target->second.height),
                                        0,
                                        1};
                D3D11_RECT scissor{0, 0, static_cast<LONG>(target->second.width),
                                   static_cast<LONG>(target->second.height)};
                context_->RSSetViewports(1, &viewport);
                context_->RSSetScissorRects(1, &scissor);
            }
        }
    }
    bool bind_editor_render_target(ResourceHandle color, ResourceHandle depth,
                                   bool clearAttachments) override {
        if (!context_) {
            lastError_ = "D3D11 editor target bind has no device context";
            return false;
        }
        ID3D11RenderTargetView *rtv = renderTarget_;
        if (color) {
            const auto colorIt = resources_.find(color.id);
            if (color.kind != ResourceKind::Texture2D || colorIt == resources_.end() ||
                !colorIt->second.rtv) {
                lastError_ = "D3D11 editor target bind color resource is invalid";
                return false;
            }
            rtv = colorIt->second.rtv;
        }
        ID3D11DepthStencilView *dsv = nullptr;
        auto depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (depth) {
            const auto depthIt = resources_.find(depth.id);
            if (depth.kind != ResourceKind::DepthStencil || depthIt == resources_.end() ||
                !depthIt->second.dsv) {
                lastError_ = "D3D11 editor target bind depth resource is invalid";
                return false;
            }
            dsv = depthIt->second.dsv;
            depthFormat = depthIt->second.format;
        }
        context_->OMSetRenderTargets(rtv ? 1u : 0u, rtv ? &rtv : nullptr, dsv);
        currentRenderTarget_ = color ? color.id : 0;
        currentDepthTarget_ = depth ? depth.id : 0;
        if (color) {
            const auto &target = resources_.at(color.id);
            const D3D11_VIEWPORT viewport{
                0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
            const D3D11_RECT scissor{0, 0, static_cast<LONG>(target.width),
                                     static_cast<LONG>(target.height)};
            context_->RSSetViewports(1, &viewport);
            context_->RSSetScissorRects(1, &scissor);
        } else if (editorViewport_.enabled) {
            set_editor_viewport(editorViewport_);
        }
        if (clearAttachments && rtv) {
            constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            context_->ClearRenderTargetView(rtv, clear);
        }
        if (clearAttachments && dsv) {
            context_->ClearDepthStencilView(dsv, depth_clear_flags(depthFormat), 1.0f, 0);
        }
        return true;
    }
    void set_render_targets(const std::vector<ResourceHandle> &colors, ResourceHandle depth,
                            bool clearAttachments) override {
        if (!context_)
            return;
        const bool editorBackbuffer = editorViewport_.enabled && editorViewport_.strictTarget;
        if (colors.size() <= 1) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth);
            if (clearAttachments) {
                constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (!colors.empty()) {
                    if (editorBackbuffer) {
                        if (renderTarget_)
                            context_->ClearRenderTargetView(renderTarget_, clear);
                    } else {
                        const auto color = resources_.find(colors.front().id);
                        if (color != resources_.end() && color->second.rtv)
                            context_->ClearRenderTargetView(color->second.rtv, clear);
                    }
                }
                if (depth) {
                    const auto depthResource = resources_.find(depth.id);
                    if (depthResource != resources_.end() && depthResource->second.dsv) {
                        context_->ClearDepthStencilView(
                            depthResource->second.dsv,
                            depth_clear_flags(depthResource->second.format), 1.0f, 0);
                    }
                }
            }
            return;
        }
        std::array<ID3D11RenderTargetView *, 8> rtvs{};
        if (editorBackbuffer && colors.size() > 1) {
            lastError_ = "D3D11 editor viewport supports one color attachment at a time";
            return;
        }
        for (std::size_t index = 0; index < colors.size() && index < rtvs.size(); ++index) {
            const auto it = resources_.find(colors[index].id);
            if (editorBackbuffer && index == 0) {
                rtvs[index] = renderTarget_;
            } else {
                if (colors[index].kind != ResourceKind::Texture2D || it == resources_.end() ||
                    !it->second.rtv) {
                    lastError_ = "D3D11 color attachment has no valid render-target view";
                    return;
                }
                rtvs[index] = it->second.rtv;
            }
        }
        ID3D11DepthStencilView *dsv = nullptr;
        auto depthFormat = DXGI_FORMAT_UNKNOWN;
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (depth.kind != ResourceKind::DepthStencil || it == resources_.end() ||
                !it->second.dsv) {
                lastError_ = "D3D11 depth attachment has no valid depth-stencil view";
                return;
            }
            dsv = it->second.dsv;
            depthFormat = it->second.format;
        }
        context_->OMSetRenderTargets(
            static_cast<UINT>(std::min<std::size_t>(colors.size(), rtvs.size())), rtvs.data(), dsv);
        currentRenderTarget_ = colors.empty() ? 0 : colors.front().id;
        currentDepthTarget_ = depth ? depth.id : 0;
        if (!editorBackbuffer && !colors.empty()) {
            const auto &target = resources_.at(colors.front().id);
            D3D11_VIEWPORT viewport{
                0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
            D3D11_RECT scissor{0, 0, static_cast<LONG>(target.width),
                               static_cast<LONG>(target.height)};
            context_->RSSetViewports(1, &viewport);
            context_->RSSetScissorRects(1, &scissor);
        }
        if (clearAttachments) {
            constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (const auto rtv : rtvs)
                if (rtv)
                    context_->ClearRenderTargetView(rtv, clear);
            if (depth && dsv)
                context_->ClearDepthStencilView(dsv, depth_clear_flags(depthFormat), 1.0f, 0);
        }
    }
    bool clear_render_attachments(const std::vector<ResourceHandle> &colors, ResourceHandle depth,
                                  bool clearColors, bool clearDepth) override {
        if (!context_)
            return false;
        if (clearColors)
            for (const auto color : colors) {
                const auto found = resources_.find(color.id);
                if (found == resources_.end() || !found->second.rtv)
                    return false;
                constexpr float clear[]{0, 0, 0, 1};
                context_->ClearRenderTargetView(found->second.rtv, clear);
            }
        if (clearDepth && depth) {
            const auto found = resources_.find(depth.id);
            if (found == resources_.end() || !found->second.dsv)
                return false;
            context_->ClearDepthStencilView(found->second.dsv,
                                            depth_clear_flags(found->second.format), 1, 0);
        }
        return true;
    }
    void set_viewport(float x, float y, float width, float height, float minDepth,
                      float maxDepth) override {
        if (!context_)
            return;
        D3D11_VIEWPORT viewport{x, y, width, height, minDepth, maxDepth};
        context_->RSSetViewports(1, &viewport);
    }
    bool set_editor_viewport(const EditorViewportSeam &seam) override {
        if (!context_) {
            lastError_ = "D3D11 editor viewport seam has no device context";
            return false;
        }
        const auto clamp_long = [](float value, bool upper) -> LONG {
            if (!std::isfinite(value))
                return 0;
            const auto rounded =
                static_cast<long long>(upper ? std::ceil(value) : std::floor(value));
            return static_cast<LONG>(
                std::clamp<long long>(rounded, 0, std::numeric_limits<LONG>::max()));
        };
        if (!seam.enabled) {
            editorViewport_ = {};
            set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f,
                         1.0f);
            const D3D11_RECT full{
                0, 0,
                static_cast<LONG>(std::min<std::uint32_t>(
                    width_, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))),
                static_cast<LONG>(std::min<std::uint32_t>(
                    height_, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())))};
            context_->RSSetScissorRects(1, &full);
            return true;
        }
        if (!seam.viewport.valid()) {
            lastError_ = "D3D11 editor viewport seam has an invalid viewport";
            return false;
        }
        const auto scissor = seam.scissor.valid() ? seam.scissor : seam.viewport;
        if (!scissor.valid()) {
            lastError_ = "D3D11 editor viewport seam has an invalid scissor";
            return false;
        }
        set_viewport(seam.viewport.x, seam.viewport.y, seam.viewport.width, seam.viewport.height,
                     0.0f, 1.0f);
        const D3D11_RECT nativeScissor{clamp_long(scissor.x, false), clamp_long(scissor.y, false),
                                       clamp_long(scissor.x + scissor.width, true),
                                       clamp_long(scissor.y + scissor.height, true)};
        if (nativeScissor.right <= nativeScissor.left ||
            nativeScissor.bottom <= nativeScissor.top) {
            lastError_ = "D3D11 editor viewport seam scissor is empty";
            return false;
        }
        context_->RSSetScissorRects(1, &nativeScissor);
        editorViewport_ = seam;
        return true;
    }
    void begin_frame() override {
        ++stats_.frames;
        submittedQueueBatches_.clear();
        currentRenderTarget_ = 0;
        currentDepthTarget_ = 0;
        if (device_) {
            const auto reason = device_->GetDeviceRemovedReason();
            if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET) {
                lastError_ = "D3D11 device lost before frame begin hr=" +
                             std::to_string(static_cast<long long>(reason));
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
            }
        }
        if (context_) {
            D3D11_VIEWPORT viewport{
                0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
            context_->RSSetViewports(1, &viewport);
            const D3D11_RECT scissor{
                0, 0,
                static_cast<LONG>(std::min<std::uint32_t>(
                    width_, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))),
                static_cast<LONG>(std::min<std::uint32_t>(
                    height_, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())))};
            context_->RSSetScissorRects(1, &scissor);
            if (editorViewport_.enabled)
                set_editor_viewport(editorViewport_);
            if (!renderTarget_)
                return;
            constexpr float clear[4] = {0.035f, 0.045f, 0.065f, 1.0f};
            context_->OMSetRenderTargets(1, &renderTarget_, depthView_);
            context_->ClearRenderTargetView(renderTarget_, clear);
            if (depthView_)
                context_->ClearDepthStencilView(depthView_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                1.0f, 0);
            if (editorViewport_.enabled)
                set_editor_viewport(editorViewport_);
        }
    }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t> &waitBatches) override {
        for (const auto waitBatch : waitBatches) {
            if (waitBatch >= submittedQueueBatches_.size() || !submittedQueueBatches_[waitBatch]) {
                lastError_ = "D3D11 queue batch wait references an unsignaled batch";
                return false;
            }
        }
        if (submittedQueueBatches_.size() <= batchIndex)
            submittedQueueBatches_.resize(batchIndex + 1, false);
        submittedQueueBatches_[batchIndex] = true;
        ++stats_.queueSubmissions;
        return context_ != nullptr;
    }
    void set_debug_name(ResourceHandle handle, std::string_view name) override {
        debugNames_[handle.id] = std::string(name);
    }
    void begin_debug_label(std::string_view) override {
        ++stats_.debugMarkers;
    }
    void end_debug_label() override {
    }
    void execute(const RenderPassContext &) override {
        ++stats_.passes;
    }
    void end_pass() override {
        if (!device_)
            return;
        const auto reason = device_->GetDeviceRemovedReason();
        if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET) {
            lastError_ =
                "D3D11 device lost after pass hr=" + std::to_string(static_cast<long long>(reason));
            capabilities_.deviceState = RenderDeviceState::Lost;
        }
    }
    void bind_pipeline(std::string_view name) override {
        for (auto &[id, resource] : resources_) {
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
        if (!boundPipeline_->pipeline.computeShader) {
            const auto colorTarget = resources_.find(currentRenderTarget_);
            const auto depthTarget = resources_.find(currentDepthTarget_);
            const auto expectedColorFormat =
                format_of(boundPipeline_->pipeline.colorFormats.empty()
                              ? std::string_view(boundPipeline_->pipeline.colorFormat)
                              : std::string_view(boundPipeline_->pipeline.colorFormats.front()));
            const auto expectedDepthFormat = format_of(boundPipeline_->pipeline.depthFormat);
            if (colorTarget != resources_.end() &&
                (colorTarget->second.kind != ResourceKind::Texture2D ||
                 colorTarget->second.format != expectedColorFormat ||
                 colorTarget->second.sampleCount != boundPipeline_->pipeline.sampleCount)) {
                lastError_ = "D3D11 pipeline color format or sample count does not match the "
                             "active render target";
                boundPipeline_ = nullptr;
                return;
            }
            if (depthTarget != resources_.end() &&
                (depthTarget->second.kind != ResourceKind::DepthStencil ||
                 depthTarget->second.format != expectedDepthFormat ||
                 depthTarget->second.sampleCount != boundPipeline_->pipeline.sampleCount)) {
                lastError_ = "D3D11 pipeline depth format or sample count does not match the "
                             "active depth target";
                boundPipeline_ = nullptr;
                return;
            }
        }
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11ComputeShader *cs = nullptr;
        if (const auto shader = resources_.find(boundPipeline_->pipeline.vertexShader);
            shader != resources_.end())
            vs = shader->second.vertexShader;
        if (const auto shader = resources_.find(boundPipeline_->pipeline.fragmentShader);
            shader != resources_.end())
            ps = shader->second.pixelShader;
        if (const auto shader = resources_.find(boundPipeline_->pipeline.computeShader);
            shader != resources_.end())
            cs = shader->second.computeShader;
        context_->VSSetShader(vs, nullptr, 0);
        context_->PSSetShader(ps, nullptr, 0);
        context_->CSSetShader(cs, nullptr, 0);
        const auto constants = boundPipeline_->pipeline.blendConstants;
        const float blendFactor[4] = {constants.x, constants.y, constants.z, constants.w};
        context_->OMSetBlendState(boundPipeline_->blendState, blendFactor, 0xffffffffu);
        context_->OMSetDepthStencilState(
            boundPipeline_->depthState,
            resolve_depth_stencil(boundPipeline_->pipeline).stencilReference);
        context_->RSSetState(boundPipeline_->rasterState);
        context_->IASetInputLayout(boundPipeline_->inputLayout);
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
        for (const auto &binding : material->second.material.bindings) {
            if (binding.space != 0) {
                lastError_ = "D3D11 does not support non-zero shader register spaces; binding '" +
                             binding.name + "' requested space=" + std::to_string(binding.space);
                return;
            }
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId :
                 {pipeline->second.pipeline.vertexShader, pipeline->second.pipeline.fragmentShader,
                  pipeline->second.pipeline.computeShader}) {
                const auto shader = resources_.find(shaderId);
                if (shader == resources_.end() || shader->second.shaderBindings.empty())
                    continue;
                reflectionAvailable = true;
                found |=
                    std::any_of(shader->second.shaderBindings.begin(),
                                shader->second.shaderBindings.end(), [&](const auto &reflected) {
                                    return shader_binding_matches(reflected, binding);
                                });
            }
            if (reflectionAvailable && !found) {
                lastError_ = "D3D11 material '" + material->second.material.name + "' binding '" +
                             binding.name +
                             "' does not match shader slot=" + std::to_string(binding.slot) +
                             " space=" + std::to_string(binding.space) +
                             " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        for (const auto &binding : material->second.material.bindings) {
            const std::vector<ResourceHandle> handles =
                binding.resources.empty() ? std::vector<ResourceHandle>{binding.resource}
                                          : binding.resources;
            if (handles.empty() || (handles.size() == 1 && !handles.front()))
                continue;
            if (binding.type == DescriptorType::Texture) {
                std::vector<ID3D11ShaderResourceView *> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.srv) {
                        lastError_ = "D3D11 texture array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.srv);
                }
                context_->PSSetShaderResources(binding.slot, static_cast<UINT>(views.size()),
                                               views.data());
                context_->CSSetShaderResources(binding.slot, static_cast<UINT>(views.size()),
                                               views.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StorageTexture) {
                std::vector<ID3D11UnorderedAccessView *> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.uav) {
                        lastError_ = "D3D11 storage texture array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.uav);
                }
                context_->CSSetUnorderedAccessViews(binding.slot, static_cast<UINT>(views.size()),
                                                    views.data(), nullptr);
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StorageBuffer) {
                std::vector<ID3D11UnorderedAccessView *> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.uav) {
                        lastError_ = "D3D11 storage buffer array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.uav);
                }
                context_->CSSetUnorderedAccessViews(binding.slot, static_cast<UINT>(views.size()),
                                                    views.data(), nullptr);
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::StructuredBuffer) {
                std::vector<ID3D11ShaderResourceView *> views;
                views.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.srv) {
                        lastError_ = "D3D11 structured buffer array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    views.push_back(resource->second.srv);
                }
                context_->VSSetShaderResources(binding.slot, static_cast<UINT>(views.size()),
                                               views.data());
                context_->PSSetShaderResources(binding.slot, static_cast<UINT>(views.size()),
                                               views.data());
                context_->CSSetShaderResources(binding.slot, static_cast<UINT>(views.size()),
                                               views.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::UniformBuffer) {
                std::vector<ID3D11Buffer *> buffers;
                buffers.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    auto *buffer = resource == resources_.end()
                                       ? nullptr
                                       : static_cast<ID3D11Buffer *>(resource->second.resource);
                    if (!buffer) {
                        lastError_ = "D3D11 uniform buffer array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    buffers.push_back(buffer);
                }
                context_->VSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()),
                                               buffers.data());
                context_->PSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()),
                                               buffers.data());
                context_->CSSetConstantBuffers(binding.slot, static_cast<UINT>(buffers.size()),
                                               buffers.data());
                ++stats_.descriptorBinds;
            } else if (binding.type == DescriptorType::Sampler) {
                std::vector<ID3D11SamplerState *> samplers;
                samplers.reserve(handles.size());
                for (const auto handle : handles) {
                    const auto resource = resources_.find(handle.id);
                    if (resource == resources_.end() || !resource->second.sampler) {
                        lastError_ = "D3D11 sampler array binding '" + binding.name +
                                     "' contains an invalid resource";
                        return;
                    }
                    samplers.push_back(resource->second.sampler);
                }
                context_->PSSetSamplers(binding.slot, static_cast<UINT>(samplers.size()),
                                        samplers.data());
                context_->CSSetSamplers(binding.slot, static_cast<UINT>(samplers.size()),
                                        samplers.data());
                ++stats_.descriptorBinds;
            }
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc &description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() ||
            material->second.kind != ResourceKind::Material) {
            lastError_ = "D3D11 material handle not found";
            return false;
        }
        material->second.material = description;
        if (boundPipeline_ == &material->second)
            boundPipeline_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot,
                             std::uint32_t space) override {
        if (space != 0) {
            lastError_ = "D3D11 does not support non-zero shader register spaces";
            return;
        }
        if (!context_ || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
            lastError_ = "D3D11 uniform buffer binding has invalid command state or slot";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() ||
            resource->second.kind != ResourceKind::Buffer || !resource->second.resource) {
            lastError_ = "D3D11 uniform buffer handle is invalid";
            return;
        }
        auto *buffer = static_cast<ID3D11Buffer *>(resource->second.resource);
        context_->VSSetConstantBuffers(slot, 1, &buffer);
        context_->PSSetConstantBuffers(slot, 1, &buffer);
        context_->CSSetConstantBuffers(slot, 1, &buffer);
    }
    bool bind_mesh_vertices(ResourceHandle primary,
                            const std::vector<VertexBufferBinding> &explicitBindings) {
        DrawVertexBindings bindings;
        if (!draw_vertex_bindings(boundPipeline_->pipeline, primary, explicitBindings, bindings,
                                  lastError_))
            return false;
        const auto &layout = boundPipeline_->pipeline.vertexLayout;
        for (const auto &binding : bindings) {
            const auto found = resources_.find(binding.buffer.id);
            if (found == resources_.end() || found->second.kind != ResourceKind::Buffer ||
                binding.offset >= found->second.size ||
                found->second.size - binding.offset > UINT_MAX) {
                lastError_ = "vertex stream buffer or offset is invalid";
                return false;
            }
            const auto definition =
                std::find_if(layout.bindings.begin(), layout.bindings.end(),
                             [&](const auto &value) { return value.binding == binding.binding; });
            if (!found->second.resource) {
                lastError_ = "missing D3D11 vertex buffer";
                return false;
            }
            const UINT stride =
                definition == layout.bindings.end() ? found->second.stride : definition->stride;
            const UINT offset = static_cast<UINT>(binding.offset);
            auto *buffer = static_cast<ID3D11Buffer *>(found->second.resource);
            context_->IASetVertexBuffers(binding.binding, 1, &buffer, &stride, &offset);
        }
        return true;
    }
    void draw_sprite(const SpriteDraw &draw) override {
        ++stats_.drawCalls;
        ++stats_.spriteCalls;
        if (!context_ || !boundPipeline_) {
            lastError_ = "D3D11 sprite draw has invalid command state";
            return;
        }
        if (draw.texture) {
            const auto texture = resources_.find(draw.texture.id);
            if (draw.texture.kind != ResourceKind::Texture2D || texture == resources_.end() ||
                !texture->second.srv) {
                lastError_ = "D3D11 sprite draw texture is invalid";
                return;
            }
            auto *srv = texture->second.srv;
            context_->PSSetShaderResources(0, 1, &srv);
        }
        context_->Draw(6, 0);
    }
    void draw_mesh(const MeshDraw &draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        if (!context_ || !boundPipeline_) {
            lastError_ = "D3D11 mesh draw has invalid command state";
            return;
        }
        auto vertex = resources_.find(draw.vertexBuffer.id);
        auto index = resources_.find(draw.indexBuffer.id);
        if (vertex != resources_.end() && index != resources_.end() &&
            draw.vertexBuffer.kind == ResourceKind::Buffer &&
            draw.indexBuffer.kind == ResourceKind::Buffer &&
            vertex->second.kind == ResourceKind::Buffer &&
            index->second.kind == ResourceKind::Buffer && vertex->second.resource &&
            index->second.resource &&
            indexed_range_fits(index->second.size, draw.firstIndex, draw.indexCount)) {
            auto *vb = static_cast<ID3D11Buffer *>(vertex->second.resource);
            auto *ib = static_cast<ID3D11Buffer *>(index->second.resource);
            if (!bind_mesh_vertices(draw.vertexBuffer, draw.vertexBindings))
                return;
            (void)vb;
            if (ib)
                context_->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
            if (!draw.instanceCount) {
                lastError_ = "instance count must be non-zero";
                return;
            }
            context_->DrawIndexedInstanced(draw.indexCount, draw.instanceCount, draw.firstIndex,
                                           draw.vertexOffset, draw.firstInstance);
            return;
        }
        lastError_ = "D3D11 mesh draw buffers are invalid";
    }
    bool dispatch(const DispatchDesc &dispatch) override {
        if (!context_ || dispatch.groupCountX == 0 || dispatch.groupCountY == 0 ||
            dispatch.groupCountZ == 0) {
            lastError_ = "D3D11 dispatch has invalid command state or zero group count";
            return false;
        }
        const auto shader = boundPipeline_ ? resources_.find(boundPipeline_->pipeline.computeShader)
                                           : resources_.end();
        if (shader == resources_.end() || !shader->second.computeShader) {
            lastError_ = "D3D11 dispatch requires a compute shader pipeline";
            return false;
        }
        ++stats_.dispatchCalls;
        context_->Dispatch(dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
        return true;
    }
    void draw_mesh_indirect(const IndirectMeshDraw &draw) override {
        if (!context_ || !boundPipeline_ || !draw.argumentBuffer || !draw.vertexBuffer ||
            !draw.indexBuffer || draw.maxDrawCount == 0 ||
            draw.stride < sizeof(std::uint32_t) * 5) {
            lastError_ = "D3D11 indirect draw has invalid command state";
            return;
        }
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (vertex == resources_.end() || index == resources_.end() ||
            arguments == resources_.end() || draw.vertexBuffer.kind != ResourceKind::Buffer ||
            draw.indexBuffer.kind != ResourceKind::Buffer ||
            draw.argumentBuffer.kind != ResourceKind::Buffer ||
            vertex->second.kind != ResourceKind::Buffer ||
            index->second.kind != ResourceKind::Buffer ||
            arguments->second.kind != ResourceKind::Buffer || !vertex->second.resource ||
            !index->second.resource || !arguments->second.resource ||
            !indirect_arguments_fit(arguments->second.size, draw)) {
            lastError_ = "D3D11 indirect draw references an invalid buffer";
            return;
        }
        auto *vb = static_cast<ID3D11Buffer *>(vertex->second.resource);
        auto *ib = static_cast<ID3D11Buffer *>(index->second.resource);
        auto *args = static_cast<ID3D11Buffer *>(arguments->second.resource);
        if (!bind_mesh_vertices(draw.vertexBuffer, draw.vertexBindings))
            return;
        (void)vb;
        context_->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        for (std::uint32_t index = 0; index < draw.maxDrawCount; ++index) {
            const auto byteOffset =
                draw.argumentOffset + static_cast<std::size_t>(index) * draw.stride;
            context_->DrawIndexedInstancedIndirect(args, static_cast<UINT>(byteOffset));
            ++stats_.drawCalls;
            ++stats_.meshCalls;
            ++stats_.indirectDrawCalls;
        }
    }
    void end_frame() override {
        if (!swapChain_)
            return;
        ui::UiTimer presentTimer(ui::UiStage::Present);
        const auto result = swapChain_->Present(vsync_ ? 1 : 0, 0);
        if (FAILED(result)) {
            lastError_ =
                "D3D11 present failed hr=" + std::to_string(static_cast<long long>(result));
#if defined(SHINKOU_PLATFORM_WINDOWS)
            append_debug_messages();
#endif
            capabilities_.deviceState =
                result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
        }
    }
    void discard_frame() override {
        ++stats_.discardedFrames;
        // D3D11 records directly into the immediate context. Unbind the
        // render targets so a failed graph cannot accidentally present them.
        if (context_)
            context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void wait_idle() override {
        if (context_)
            context_->Flush();
    }
    RenderStats stats() const noexcept override {
        return stats_;
    }
};

} // namespace
std::unique_ptr<IRenderBackend> create_directx11_backend() {
    return std::make_unique<DirectX11Backend>();
}
} // namespace shinkou::render
#endif
