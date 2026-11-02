#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#if defined(SHINKOU_WITH_IMGUI)
#include <backends/imgui_impl_dx12.h>
#include <imgui.h>
#endif
#endif
#include "DxgiPipelineState.h"
#include "EditorGpuCapture.h"
#include "NativeTextureCache.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
namespace shinkou::render {
namespace {
class DirectX12Backend final : public IRenderBackend {
    static D3D12_CULL_MODE cull_mode(std::string_view mode) {
        if (mode == "front")
            return D3D12_CULL_MODE_FRONT;
        if (mode == "none")
            return D3D12_CULL_MODE_NONE;
        return D3D12_CULL_MODE_BACK;
    }
    static D3D12_FILL_MODE fill_mode(std::string_view mode) {
        return mode == "wireframe" ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    }
    static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(std::string_view mode) {
        if (mode == "line")
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        if (mode == "point")
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    static D3D_PRIMITIVE_TOPOLOGY topology(std::string_view mode) {
        if (mode == "line")
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        if (mode == "point")
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    static std::filesystem::path pipeline_cache_path(ID3D12Device *device) {
        LUID luid{};
        if (device)
            luid = device->GetAdapterLuid();
        const auto key = std::to_string(static_cast<unsigned long long>(luid.HighPart)) + "_" +
                         std::to_string(static_cast<unsigned long long>(luid.LowPart));
        // Bump this when the native pipeline/root-signature construction
        // changes. The adapter LUID is part of the filename, so invalidation
        // remains scoped to the exact physical adapter.
        constexpr std::string_view cacheFormat = "v3";
        if (const auto *directory = std::getenv("SHINKOU_PIPELINE_CACHE_DIR")) {
            return std::filesystem::path(directory) /
                   (key + "_" + std::string(cacheFormat) + ".dxc");
        }
        return std::filesystem::temp_directory_path() / "shinkou_pipeline_cache" /
               (key + "_" + std::string(cacheFormat) + ".dxc");
    }
    static std::wstring pipeline_library_name(
        const PipelineDesc &description, const std::vector<std::uint8_t> &vertexBytecode = {},
        const std::vector<std::uint8_t> &fragmentBytecode = {},
        const std::vector<std::uint8_t> &computeBytecode = {}, std::wstring_view suffix = {}) {
        std::string key =
            std::to_string(description.vertexShader) + ":" +
            std::to_string(description.fragmentShader) + ":" +
            std::to_string(description.computeShader) + ":" + (description.depthTest ? "1" : "0") +
            (description.depthWrite ? "1" : "0") + (description.alphaBlend ? "1" : "0") +
            (description.vertexInput ? "1" : "0") + ":" + description.cullMode + ":" +
            description.fillMode + ":" + description.topology + ":" + description.colorFormat +
            ":" + description.depthFormat + ":" + std::to_string(description.sampleCount);
        for (const auto &format : description.colorFormats) {
            key += ":" + format;
        }
        const auto appendBytecode = [&key](const auto &bytecode) {
            for (const auto byte : bytecode)
                key += std::to_string(byte) + ",";
        };
        appendBytecode(vertexBytecode);
        appendBytecode(fragmentBytecode);
        appendBytecode(computeBytecode);
        key += ":" + pipeline_state_key(description);
        const auto hash = std::hash<std::string>{}(key);
        const auto name =
            std::wstring(L"shinkou_pso_") + std::to_wstring(static_cast<unsigned long long>(hash));
        return std::wstring(name) + std::wstring(suffix);
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        ID3D12Resource *resource{nullptr};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<std::uint8_t> shaderBytecode;
        std::vector<ShaderBinding> shaderBindings;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::size_t size{0};
        UINT stride{0};
        bool indexBuffer{false};
        bool uploadHeap{false};
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
        ID3D12PipelineState *pipelineState{nullptr};
        ID3D12PipelineState *depthOnlyPipeline{nullptr};
        ID3D12RootSignature *rootSignature{nullptr};
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
        ResourceRecord(const ResourceRecord &) = delete;
        ResourceRecord &operator=(const ResourceRecord &) = delete;
        ResourceRecord(ResourceRecord &&other) noexcept
            : kind(other.kind), resource(other.resource), pipeline(std::move(other.pipeline)),
              material(std::move(other.material)), shaderBytecode(std::move(other.shaderBytecode)),
              shaderBindings(std::move(other.shaderBindings)), state(other.state), size(other.size),
              stride(other.stride), indexBuffer(other.indexBuffer), uploadHeap(other.uploadHeap),
              format(other.format), width(other.width), height(other.height),
              mipLevels(other.mipLevels), layers(other.layers),
              baseTextureData(std::move(other.baseTextureData)), gpuAllocation(other.gpuAllocation),
              nativeDescription(other.nativeDescription), clearValue(other.clearValue),
              hasClearValue(other.hasClearValue), placedResource(other.placedResource),
              pipelineState(other.pipelineState), depthOnlyPipeline(other.depthOnlyPipeline),
              rootSignature(other.rootSignature), rootParameters(std::move(other.rootParameters)),
              srvGpu(other.srvGpu), srvCpu(other.srvCpu), uavGpu(other.uavGpu),
              uavCpu(other.uavCpu), cbvGpu(other.cbvGpu), cbvCpu(other.cbvCpu),
              rtvCpu(other.rtvCpu), dsvCpu(other.dsvCpu), samplerGpu(other.samplerGpu),
              samplerCpu(other.samplerCpu), srvIndex(other.srvIndex), cbvIndex(other.cbvIndex),
              uavIndex(other.uavIndex), samplerIndex(other.samplerIndex), rtvIndex(other.rtvIndex),
              dsvIndex(other.dsvIndex), srvValid(other.srvValid), cbvValid(other.cbvValid),
              uavValid(other.uavValid), rtvValid(other.rtvValid), dsvValid(other.dsvValid),
              alias(other.alias), samplerValid(other.samplerValid),
              materialRanges(std::move(other.materialRanges)),
              materialRangesValid(other.materialRangesValid) {
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
        ResourceRecord &operator=(ResourceRecord &&other) noexcept {
            if (this == &other)
                return *this;
            if (pipelineState)
                pipelineState->Release();
            if (depthOnlyPipeline)
                depthOnlyPipeline->Release();
            if (rootSignature)
                rootSignature->Release();
            if (resource)
                resource->Release();
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
            uploadHeap = other.uploadHeap;
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
            if (pipelineState)
                pipelineState->Release();
            if (depthOnlyPipeline)
                depthOnlyPipeline->Release();
            if (rootSignature)
                rootSignature->Release();
            if (resource)
                resource->Release();
        }
    };
    ID3D12Device *device_{nullptr};
    ID3D12CommandQueue *queue_{nullptr};
    IDXGISwapChain3 *swapChain_{nullptr};
    ID3D12CommandAllocator *commandAllocator_{nullptr};
    ID3D12GraphicsCommandList *commandList_{nullptr};
    ID3D12CommandSignature *indexedDrawSignature_{nullptr};
    ID3D12DescriptorHeap *rtvHeap_{nullptr};
    ID3D12DescriptorHeap *srvHeap_{nullptr};
    std::vector<ResourceHandle> imguiTextures_;
    ID3D12DescriptorHeap *cpuSrvHeap_{nullptr};
    ID3D12DescriptorHeap *samplerHeap_{nullptr};
    ID3D12DescriptorHeap *dsvHeap_{nullptr};
    ID3D12Resource *depthBuffer_{nullptr};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle_{};
    std::vector<ID3D12Resource *> backBuffers_;
    ID3D12Fence *fence_{nullptr};
    HANDLE fenceEvent_{nullptr};
    std::uint64_t fenceValue_{0};
    std::uint64_t lastSubmittedFrame_{0};
    ID3D12QueryHeap *timestampQueryHeap_{nullptr};
    ID3D12Resource *timestampReadback_{nullptr};
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
    std::uint32_t currentDepthTarget_{0};
    DXGI_FORMAT swapchainFormat_{DXGI_FORMAT_UNKNOWN};
    struct BindlessTableRecord {
        std::uint32_t baseIndex{0};
        std::uint32_t capacity{0};
    };
    std::unordered_map<std::uint32_t, BindlessTableRecord> bindlessTables_;
    std::vector<std::pair<std::uint64_t, BindlessTableRecord>> retiredBindlessTables_;
    std::uint32_t nextBindlessTable_{1};
    std::uint32_t boundBindlessTable_{0};
    std::uint32_t frameIndex_{0};
    bool frameActive_{false};
    bool vsync_{true};
    std::uint32_t width_{1280};
    std::uint32_t height_{720};
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    GpuMemoryAllocator gpuMemoryAllocator_{};
    std::unordered_map<std::uint64_t, ID3D12Heap *> gpuHeaps_;
    std::unordered_map<ID3D12Resource *, D3D12_RESOURCE_STATES> resourceStates_;
    // D3D12 currently uses one direct queue/list as the portable fallback.
    // Keep batch dependency metadata explicit even though execution is
    // serialized on that queue.
    std::vector<bool> recordedQueueBatches_;
    std::vector<std::pair<std::uint64_t, ResourceRecord>> retiredResources_;
    std::vector<std::pair<
        std::uint64_t, std::unordered_map<std::uint64_t, ResourceRecord::MaterialDescriptorRange>>>
        retiredMaterialRanges_;
    struct PendingUpload {
        std::uint64_t fence{0};
        ID3D12CommandAllocator *allocator{nullptr};
        ID3D12GraphicsCommandList *list{nullptr};
        ID3D12Resource *staging{nullptr};
    };
    std::vector<PendingUpload> pendingUploads_;
    bool uploadBatchActive_{false};
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord *boundPipeline_{nullptr};
    ResourceRecord *boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    EditorViewportSeam editorViewport_{};
    bool imguiPlatformPending_{false};
    bool uiCaptureWritten_{false};
#if defined(SHINKOU_WITH_IMGUI)
    void (*imguiRenderWindow_)(ImGuiViewport *, void *){nullptr};
    std::uint64_t imguiWorldReadyFence_{};
#endif
    bool read_native_texture(ID3D12Resource *source, D3D12_RESOURCE_STATES state,
                             const TextureReadbackRequest &request, TextureReadback &output) {
        if (!source || !device_ || !queue_ || !fence_)
            return false;
        const auto desc = source->GetDesc();
        const bool color = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                           desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                           desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const UINT pixelSize = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        if ((!color && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
             desc.Format != DXGI_FORMAT_R32_FLOAT) ||
            desc.SampleDesc.Count != 1 || request.mipLevel >= desc.MipLevels ||
            request.layer >= desc.DepthOrArraySize)
            return false;
        const auto fullWidth = std::max<UINT64>(1, desc.Width >> request.mipLevel);
        const auto fullHeight = std::max<UINT>(1, desc.Height >> request.mipLevel);
        if (request.x >= fullWidth || request.y >= fullHeight)
            return false;
        const auto width = request.width ? request.width : static_cast<UINT>(fullWidth - request.x);
        const auto height = request.height ? request.height : fullHeight - request.y;
        const auto bytes = static_cast<std::uint64_t>(width) * height * pixelSize;
        if (!width || !height || width > fullWidth - request.x || height > fullHeight - request.y ||
            bytes > request.maxBytes)
            return false;
        const UINT pitch = (width * pixelSize + 255u) & ~255u;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(pitch) * height;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        CaptureCom<ID3D12Resource> staging;
        CaptureCom<ID3D12CommandAllocator> allocator;
        CaptureCom<ID3D12GraphicsCommandList> list;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(staging.put()))) ||
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(allocator.put()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(),
                                              nullptr, IID_PPV_ARGS(list.put()))))
            return false;
        const UINT subresource = request.mipLevel + request.layer * desc.MipLevels;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            list.get()->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = source;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = staging.get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {desc.Format, width, height, 1, pitch};
        const D3D12_BOX box{request.x, request.y, 0, request.x + width, request.y + height, 1};
        list.get()->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list.get()->ResourceBarrier(1, &barrier);
        }
        if (FAILED(list.get()->Close()))
            return false;
        ID3D12CommandList *commands[]{list.get()};
        queue_->ExecuteCommandLists(1, commands);
        const auto value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_, value)) ||
            FAILED(fence_->SetEventOnCompletion(value, fenceEvent_)) ||
            WaitForSingleObject(fenceEvent_, INFINITE) != WAIT_OBJECT_0)
            return false;
        lastSubmittedFrame_ = value;
        void *mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
        if (FAILED(staging.get()->Map(0, &range, &mapped)))
            return false;
        TextureReadback result;
        result.width = width;
        result.height = height;
        result.rowPitch = static_cast<std::size_t>(width) * pixelSize;
        result.data.resize(static_cast<std::size_t>(bytes));
        for (UINT y = 0; y < height; ++y)
            std::memcpy(result.data.data() + y * result.rowPitch,
                        static_cast<const std::uint8_t *>(mapped) + y * pitch, result.rowPitch);
        const D3D12_RANGE written{0, 0};
        staging.get()->Unmap(0, &written);
        output = std::move(result);
        return true;
    }
    void capture_ui_surface(ID3D12Resource *source, std::uintptr_t window) {
        if (uiCaptureWritten_)
            return;
        const auto request = editor_gpu_capture_request();
        if (!request || request->window != window)
            return;
        TextureReadback pixels;
        if (read_native_texture(source, D3D12_RESOURCE_STATE_PRESENT, {}, pixels)) {
            const auto format = source->GetDesc().Format;
            uiCaptureWritten_ = write_editor_gpu_capture(
                *request, pixels,
                format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        }
    }
    RenderStats stats_{};
    std::string lastError_;
    std::unordered_map<std::uint32_t, std::string> debugNames_;
    ID3D12PipelineLibrary1 *pipelineLibrary_{nullptr};
    std::filesystem::path pipelineCachePath_{};
    // CreatePipelineLibrary borrows its initial blob for the library lifetime.
    std::vector<std::uint8_t> pipelineCacheData_;
    void read_timestamp_results() {
        if (!timestampReadback_ || submittedTimestampCount_ < 2 || timestampFrequency_ == 0)
            return;
        void *mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(submittedTimestampCount_) * sizeof(UINT64)};
        if (FAILED(timestampReadback_->Map(0, &range, &mapped)) || !mapped)
            return;
        const auto *values = static_cast<const UINT64 *>(mapped);
        stats_.gpuPassNanoseconds.clear();
        stats_.gpuPassNanoseconds.reserve(submittedTimestampCount_ / 2);
        for (UINT i = 0; i + 1 < submittedTimestampCount_; i += 2) {
            const UINT64 ticks = values[i + 1] >= values[i] ? values[i + 1] - values[i] : 0;
            stats_.gpuPassNanoseconds.push_back(
                static_cast<std::uint64_t>((static_cast<long double>(ticks) * 1000000000.0L) /
                                           static_cast<long double>(timestampFrequency_)));
        }
        D3D12_RANGE written{0, 0};
        timestampReadback_->Unmap(0, &written);
    }
    void release_completed_uploads() {
        const auto completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
        pendingUploads_.erase(std::remove_if(pendingUploads_.begin(), pendingUploads_.end(),
                                             [completed](PendingUpload &upload) {
                                                 if (upload.fence != 0 && upload.fence > completed)
                                                     return false;
                                                 if (upload.list)
                                                     upload.list->Release();
                                                 if (upload.allocator)
                                                     upload.allocator->Release();
                                                 if (upload.staging)
                                                     upload.staging->Release();
                                                 return true;
                                             }),
                              pendingUploads_.end());
    }
    void save_pipeline_library() {
        if (!pipelineLibrary_)
            return;
        const auto size = pipelineLibrary_->GetSerializedSize();
        if (size == 0)
            return;
        std::vector<std::uint8_t> data(size);
        if (FAILED(pipelineLibrary_->Serialize(data.data(), data.size())))
            return;
        std::error_code error;
        std::filesystem::create_directories(pipelineCachePath_.parent_path(), error);
        if (error)
            return;
        const auto temporary = pipelineCachePath_.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return;
        stream.write(reinterpret_cast<const char *>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        stream.close();
        if (stream) {
            std::filesystem::remove(pipelineCachePath_, error);
            error.clear();
            std::filesystem::rename(temporary, pipelineCachePath_, error);
        }
        if (error)
            std::filesystem::remove(temporary, error);
    }
    void invalidate_pipeline_cache() {
        if (pipelineLibrary_) {
            pipelineLibrary_->Release();
            pipelineLibrary_ = nullptr;
        }
        pipelineCacheData_.clear();
        if (pipelineCachePath_.empty())
            return;
        std::error_code error;
        std::filesystem::remove(pipelineCachePath_, error);
    }
    void append_d3d12_diagnostics(std::string_view operation, HRESULT result) {
        lastError_ = "D3D12 " + std::string(operation) +
                     " failed hr=" + std::to_string(static_cast<long long>(result));
        if (device_) {
            const auto reason = device_->GetDeviceRemovedReason();
            lastError_ += " deviceReason=" + std::to_string(static_cast<long long>(reason));
            if (result == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
                invalidate_pipeline_cache();
                lastError_ += " pipelineCacheInvalidated=1";
            }
        }
        ID3D12InfoQueue *infoQueue = nullptr;
        if (!device_ || FAILED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue))) || !infoQueue)
            return;
        const auto messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < messageCount; ++index) {
            SIZE_T messageSize = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) || messageSize == 0)
                continue;
            std::vector<std::uint8_t> messageStorage(messageSize);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(messageStorage.data());
            if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)) &&
                message->pDescription) {
                lastError_ += " [" + std::string(message->pDescription) + "]";
            }
        }
        infoQueue->ClearStoredMessages();
        infoQueue->Release();
    }
#if defined(SHINKOU_WITH_IMGUI)
    static void allocate_imgui_srv(ImGui_ImplDX12_InitInfo *info,
                                   D3D12_CPU_DESCRIPTOR_HANDLE *cpuHandle,
                                   D3D12_GPU_DESCRIPTOR_HANDLE *gpuHandle) {
        if (cpuHandle)
            cpuHandle->ptr = 0;
        if (gpuHandle)
            gpuHandle->ptr = 0;
        if (!info || !cpuHandle || !gpuHandle)
            return;
        auto *backend = static_cast<DirectX12Backend *>(info->UserData);
        if (!backend || !backend->srvHeap_ || backend->srvStride_ == 0)
            return;
        UINT slot = 0;
        if (!backend->take_srv_slot(slot))
            return;
        *cpuHandle = backend->srvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpuHandle->ptr += static_cast<SIZE_T>(slot) * backend->srvStride_;
        *gpuHandle = backend->srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpuHandle->ptr += static_cast<UINT64>(slot) * backend->srvStride_;
    }
    static void release_imgui_srv(ImGui_ImplDX12_InitInfo *info,
                                  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                  D3D12_GPU_DESCRIPTOR_HANDLE) {
        if (!info || cpuHandle.ptr == 0)
            return;
        auto *backend = static_cast<DirectX12Backend *>(info->UserData);
        if (!backend || !backend->srvHeap_ || backend->srvStride_ == 0)
            return;
        const auto heapStart = backend->srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr;
        if (cpuHandle.ptr < heapStart)
            return;
        const auto byteOffset = cpuHandle.ptr - heapStart;
        if (byteOffset % backend->srvStride_ != 0)
            return;
        const auto slot = static_cast<UINT>(byteOffset / backend->srvStride_);
        if (slot < 1024u)
            backend->release_srv_slot(slot);
    }
#endif
  public:
    ~DirectX12Backend() override {
        if (queue_ && fence_ && fenceEvent_ && fenceValue_ > 0) {
            queue_->Signal(fence_, fenceValue_);
            if (fence_->GetCompletedValue() < fenceValue_) {
                fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }
        transientTextures_.clear([&](auto &resource) { release_gpu_allocation(resource); });
        for (auto &resource : resources_)
            release_gpu_allocation(resource.second);
        for (auto &retired : retiredResources_)
            release_gpu_allocation(retired.second);
        resources_.clear();
        retiredResources_.clear();
        save_pipeline_library();
        if (pipelineLibrary_)
            pipelineLibrary_->Release();
        for (auto &upload : pendingUploads_) {
            if (upload.list)
                upload.list->Release();
            if (upload.allocator)
                upload.allocator->Release();
            if (upload.staging)
                upload.staging->Release();
        }
        pendingUploads_.clear();
        for (auto *buffer : backBuffers_)
            if (buffer)
                buffer->Release();
        if (depthBuffer_)
            depthBuffer_->Release();
        if (fenceEvent_)
            CloseHandle(fenceEvent_);
        if (fence_)
            fence_->Release();
        if (timestampReadback_)
            timestampReadback_->Release();
        if (timestampQueryHeap_)
            timestampQueryHeap_->Release();
        if (rtvHeap_)
            rtvHeap_->Release();
        if (srvHeap_)
            srvHeap_->Release();
        if (cpuSrvHeap_)
            cpuSrvHeap_->Release();
        if (samplerHeap_)
            samplerHeap_->Release();
        if (dsvHeap_)
            dsvHeap_->Release();
        if (commandList_)
            commandList_->Release();
        if (indexedDrawSignature_)
            indexedDrawSignature_->Release();
        if (commandAllocator_)
            commandAllocator_->Release();
        if (swapChain_)
            swapChain_->Release();
        if (queue_)
            queue_->Release();
        for (auto &heap : gpuHeaps_)
            if (heap.second)
                heap.second->Release();
        gpuHeaps_.clear();
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
    void request_ui_capture() override {
        uiCaptureWritten_ = false;
    }
    bool read_texture(const TextureReadbackRequest &request, TextureReadback &output) override {
        const auto found = resources_.find(request.texture.id);
        if (frameActive_ || found == resources_.end() ||
            request.texture.kind != ResourceKind::Texture2D)
            return false;
        const auto state = resourceStates_.find(found->second.resource);
        return read_native_texture(
            found->second.resource,
            state == resourceStates_.end() ? found->second.state : state->second, request, output);
    }
#if defined(SHINKOU_WITH_IMGUI)
    bool initialize_imgui() override {
        if (!device_ || !queue_ || !srvHeap_ || !swapChain_ || !ImGui::GetCurrentContext())
            return false;
        ImGui_ImplDX12_InitInfo info{};
        info.Device = device_;
        info.CommandQueue = queue_;
        info.NumFramesInFlight = static_cast<int>(std::max<std::size_t>(2, backBuffers_.size()));
        info.RTVFormat = swapchainFormat_;
        info.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        info.UserData = this;
        info.SrvDescriptorHeap = srvHeap_;
        info.SrvDescriptorAllocFn = &allocate_imgui_srv;
        info.SrvDescriptorFreeFn = &release_imgui_srv;
        if (!ImGui_ImplDX12_Init(&info))
            return false;
        auto &platform = ImGui::GetPlatformIO();
        imguiRenderWindow_ = platform.Renderer_RenderWindow;
        platform.Renderer_RenderWindow = [](ImGuiViewport *viewport, void *renderer) {
            auto *self = static_cast<DirectX12Backend *>(renderer);
            ImGui_ImplDX12_ViewportSync sync{};
            if (!self || !self->imguiRenderWindow_)
                return;
            if (ImGui_ImplDX12_GetViewportSync(viewport, &sync))
                sync.Queue->Wait(self->fence_, self->imguiWorldReadyFence_);
            self->imguiRenderWindow_(viewport, nullptr);
            if (ImGui_ImplDX12_GetViewportSync(viewport, &sync)) {
                self->queue_->Wait(sync.Fence, sync.FenceValue);
                self->capture_ui_surface(
                    sync.BackBuffer, reinterpret_cast<std::uintptr_t>(viewport->PlatformHandleRaw));
            }
        };
        return true;
    }
    void shutdown_imgui() override {
        if (ImGui::GetCurrentContext())
            ImGui_ImplDX12_Shutdown();
    }
    void new_imgui_frame() override {
        imguiTextures_.clear();
        ImGui_ImplDX12_NewFrame();
    }
    void invalidate_imgui_fonts() override {
        ImGui_ImplDX12_InvalidateDeviceObjects();
    }
    std::uint64_t imgui_texture(ResourceHandle handle) override {
        const auto found = resources_.find(handle.id);
        if (found == resources_.end() || !found->second.srvValid)
            return 0;
        imguiTextures_.push_back(handle);
        return found->second.srvGpu.ptr;
    }
    void render_imgui(ImDrawData *drawData) override {
        if (!drawData || !frameActive_ || !commandList_ || !swapChain_)
            return;
        set_render_target({});
        for (const auto texture : imguiTextures_)
            transition_resource(texture, ResourceUsage::ShaderRead);
        imguiTextures_.clear();
        ID3D12DescriptorHeap *heaps[]{srvHeap_};
        commandList_->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(drawData, commandList_);
        imguiPlatformPending_ = true;
    }
#endif
    bool initialize(const RenderBackendConfig &config) override {
        if (std::getenv("SHINKOU_D3D12_DEBUG") != nullptr) {
            ID3D12Debug *debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
                debug->EnableDebugLayer();
                debug->Release();
            }
        }
        IDXGIAdapter1 *adapter = select_high_performance_adapter();
        HRESULT deviceResult =
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (adapter)
            adapter->Release();
        if (FAILED(deviceResult))
            return false;
        pipelineCachePath_ = pipeline_cache_path(device_);
        {
            auto &cacheData = pipelineCacheData_;
            std::ifstream stream(pipelineCachePath_, std::ios::binary | std::ios::ate);
            if (stream) {
                const auto size = stream.tellg();
                if (size > 0 && size <= static_cast<std::streamoff>(1ull << 30)) {
                    cacheData.resize(static_cast<std::size_t>(size));
                    stream.seekg(0);
                    if (!stream.read(reinterpret_cast<char *>(cacheData.data()), size))
                        cacheData.clear();
                }
            }
            ID3D12Device1 *device1 = nullptr;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&device1)))) {
                if (FAILED(device1->CreatePipelineLibrary(
                        cacheData.empty() ? nullptr : cacheData.data(), cacheData.size(),
                        IID_PPV_ARGS(&pipelineLibrary_))) &&
                    !cacheData.empty()) {
                    std::error_code error;
                    std::filesystem::remove(pipelineCachePath_, error);
                    cacheData.clear();
                    device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipelineLibrary_));
                }
                device1->Release();
            }
        }
        vsync_ = config.vsync;
        width_ = config.width;
        height_ = config.height;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_))))
            return false;
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
            if (FAILED(device_->CreateCommittedResource(
                    &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestampReadback_)))) {
                timestampQueryHeap_->Release();
                timestampQueryHeap_ = nullptr;
                timestampFrequency_ = 0;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = 1024;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap_))))
            return false;
        srvStride_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&cpuSrvHeap_))))
            return false;
        cpuSrvStride_ = srvStride_;
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
        samplerHeapDesc.NumDescriptors = 256;
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap_))))
            return false;
        samplerStride_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        if (config.nativeWindow) {
            IDXGIFactory4 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
                DXGI_SWAP_CHAIN_DESC1 swapDesc{};
                swapDesc.Width = config.width;
                swapDesc.Height = config.height;
                // Direct2D DXGI-surface interop requires a BGRA-compatible
                // backbuffer on the Windows path.
                swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                swapchainFormat_ = swapDesc.Format;
                swapDesc.SampleDesc.Count = 1;
                swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapDesc.BufferCount = 2;
                swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                IDXGISwapChain1 *baseSwapChain = nullptr;
                const HRESULT result =
                    factory->CreateSwapChainForHwnd(queue_, static_cast<HWND>(config.nativeWindow),
                                                    &swapDesc, nullptr, nullptr, &baseSwapChain);
                if (SUCCEEDED(result))
                    baseSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain_));
                if (baseSwapChain)
                    baseSwapChain->Release();
                factory->Release();
            }
        }
        if (swapChain_) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.NumDescriptors = 64;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_))))
                return false;
            rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            backBuffers_.resize(2);
            auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            for (std::uint32_t i = 0; i < backBuffers_.size(); ++i) {
                if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]))))
                    return false;
                auto rtv = handle;
                rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
                device_->CreateRenderTargetView(backBuffers_[i], nullptr, rtv);
            }
            if (!create_depth_target(config.width, config.height))
                return false;
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&commandAllocator_))))
                return false;
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  commandAllocator_, nullptr,
                                                  IID_PPV_ARGS(&commandList_))))
                return false;
            commandList_->Close();
            D3D12_COMMAND_SIGNATURE_DESC signature{};
            signature.ByteStride = sizeof(std::uint32_t) * 5;
            signature.NumArgumentDescs = 1;
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            signature.pArgumentDescs = &argument;
            if (FAILED(device_->CreateCommandSignature(&signature, nullptr,
                                                       IID_PPV_ARGS(&indexedDrawSignature_))))
                return false;
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
                return false;
            fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_)
                return false;
        } else {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.NumDescriptors = 64;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_))))
                return false;
            rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_))))
                return false;
            dsvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&commandAllocator_))))
                return false;
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  commandAllocator_, nullptr,
                                                  IID_PPV_ARGS(&commandList_))))
                return false;
            if (FAILED(commandList_->Close()))
                return false;
            D3D12_COMMAND_SIGNATURE_DESC signature{};
            signature.ByteStride = sizeof(std::uint32_t) * 5;
            signature.NumArgumentDescs = 1;
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            signature.pArgumentDescs = &argument;
            if (FAILED(device_->CreateCommandSignature(&signature, nullptr,
                                                       IID_PPV_ARGS(&indexedDrawSignature_))))
                return false;
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
                return false;
            fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_)
                return false;
        }
        capabilities_.api = BackendApi::DirectX12;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
#if defined(SHINKOU_WITH_IMGUI)
        capabilities_.supportsImGui = true;
#else
        capabilities_.supportsImGui = false;
#endif
        capabilities_.supportsEditorViewportScissor = true;
        capabilities_.supportsEditorOffscreenTarget = true;
        capabilities_.supportsEditorModelRendering = true;
        capabilities_.supportsTextureReadback = true;
        capabilities_.supportsCompute = true;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool optionsAvailable = SUCCEEDED(
            device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
        capabilities_.supportsBindless =
            optionsAvailable && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
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
        if (!swapChain_ || !device_)
            return true;
        wait_idle();
        for (auto *buffer : backBuffers_)
            if (buffer)
                buffer->Release();
        backBuffers_.clear();
        if (depthBuffer_) {
            depthBuffer_->Release();
            depthBuffer_ = nullptr;
        }
        const auto resizeResult = swapChain_->ResizeBuffers(2, width, height, swapchainFormat_, 0);
        if (FAILED(resizeResult)) {
            lastError_ = "D3D12 swapchain resize failed hr=" +
                         std::to_string(static_cast<long long>(resizeResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState =
                reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
            return false;
        }
        backBuffers_.resize(2);
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < backBuffers_.size(); ++i) {
            if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]))))
                return false;
            auto rtv = handle;
            rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
            device_->CreateRenderTargetView(backBuffers_[i], nullptr, rtv);
        }
        width_ = width;
        height_ = height;
        const auto resized = create_depth_target(width, height);
        if (resized)
            capabilities_.deviceState = RenderDeviceState::Ready;
        return resized;
    }
    bool create_depth_target(std::uint32_t width, std::uint32_t height) {
        if (!dsvHeap_) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.NumDescriptors = 64;
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsvHeap_))))
                return false;
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
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&depthBuffer_))))
            return false;
        dsvHandle_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateDepthStencilView(depthBuffer_, nullptr, dsvHandle_);
        dsvCursor_ = 1;
        return true;
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
    static bool block_compressed_format(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC3_UNORM ||
               format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC6H_UF16 ||
               format == DXGI_FORMAT_BC7_UNORM;
    }
    bool take_srv_slot(UINT &slot) {
        if (!freeSrvSlots_.empty()) {
            slot = freeSrvSlots_.back();
            freeSrvSlots_.pop_back();
            return true;
        }
        if (srvCursor_ >= 1024)
            return false;
        slot = srvCursor_++;
        return true;
    }
    bool take_srv_range(UINT count, UINT &base) {
        if (count == 0)
            return false;
        std::sort(freeSrvSlots_.begin(), freeSrvSlots_.end());
        for (std::size_t index = 0; index + count <= freeSrvSlots_.size(); ++index) {
            bool contiguous = true;
            for (UINT offset = 1; offset < count; ++offset) {
                if (freeSrvSlots_[index + offset] != freeSrvSlots_[index] + offset) {
                    contiguous = false;
                    break;
                }
            }
            if (!contiguous)
                continue;
            base = freeSrvSlots_[index];
            freeSrvSlots_.erase(freeSrvSlots_.begin() + static_cast<std::ptrdiff_t>(index),
                                freeSrvSlots_.begin() + static_cast<std::ptrdiff_t>(index + count));
            return true;
        }
        if (count > 1024u - srvCursor_)
            return false;
        base = srvCursor_;
        srvCursor_ += count;
        return true;
    }
    void release_srv_slot(UINT slot) {
        if (slot < 1024)
            freeSrvSlots_.push_back(slot);
    }
    bool take_sampler_slot(UINT &slot) {
        if (!freeSamplerSlots_.empty()) {
            slot = freeSamplerSlots_.back();
            freeSamplerSlots_.pop_back();
            return true;
        }
        if (samplerCursor_ >= 256)
            return false;
        slot = samplerCursor_++;
        return true;
    }
    bool take_sampler_range(UINT count, UINT &base) {
        if (count == 0)
            return false;
        std::sort(freeSamplerSlots_.begin(), freeSamplerSlots_.end());
        for (std::size_t index = 0; index + count <= freeSamplerSlots_.size(); ++index) {
            bool contiguous = true;
            for (UINT offset = 1; offset < count; ++offset) {
                if (freeSamplerSlots_[index + offset] != freeSamplerSlots_[index] + offset) {
                    contiguous = false;
                    break;
                }
            }
            if (!contiguous)
                continue;
            base = freeSamplerSlots_[index];
            freeSamplerSlots_.erase(freeSamplerSlots_.begin() + static_cast<std::ptrdiff_t>(index),
                                    freeSamplerSlots_.begin() +
                                        static_cast<std::ptrdiff_t>(index + count));
            return true;
        }
        if (count > 256u - samplerCursor_)
            return false;
        base = samplerCursor_;
        samplerCursor_ += count;
        return true;
    }
    void release_sampler_slot(UINT slot) {
        if (slot < 256)
            freeSamplerSlots_.push_back(slot);
    }
    void release_sampler_range(const ResourceRecord::MaterialDescriptorRange &range) {
        for (UINT index = 0; index < range.count; ++index) {
            if (range.samplerHeap)
                release_sampler_slot(range.base + index);
            else
                release_srv_slot(range.base + index);
        }
    }
    void release_material_ranges(ResourceRecord &record) {
        for (const auto &[key, range] : record.materialRanges) {
            (void)key;
            release_sampler_range(range);
        }
        record.materialRanges.clear();
        record.materialRangesValid = false;
    }
    void defer_material_ranges(ResourceRecord &record) {
        if (!record.materialRanges.empty()) {
            retiredMaterialRanges_.push_back({fenceValue_, std::move(record.materialRanges)});
        }
        record.materialRanges.clear();
        record.materialRangesValid = false;
    }
    static D3D12_DESCRIPTOR_RANGE_TYPE descriptor_range_type(DescriptorType type) {
        if (type == DescriptorType::Sampler)
            return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        if (type == DescriptorType::UniformBuffer)
            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        if (type == DescriptorType::StorageBuffer || type == DescriptorType::StorageTexture)
            return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
    bool build_material_descriptor_ranges(ResourceRecord &material, ResourceRecord &pipeline) {
        release_material_ranges(material);
        for (const auto &binding : material.material.bindings) {
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
                lastError_ =
                    "D3D12 material binding '" + binding.name + "' has no root descriptor range";
                release_material_ranges(material);
                return false;
            }
            ResourceRecord::MaterialDescriptorRange range{};
            range.count = static_cast<UINT>(handles.size());
            range.samplerHeap = rangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            const bool allocated = range.samplerHeap ? take_sampler_range(range.count, range.base)
                                                     : take_srv_range(range.count, range.base);
            if (!allocated) {
                lastError_ =
                    "D3D12 descriptor heap cannot allocate a contiguous material range for '" +
                    binding.name + "'";
                release_material_ranges(material);
                return false;
            }
            material.materialRanges[key] = range;
            for (UINT index = 0; index < range.count; ++index) {
                const auto resource = resources_.find(handles[index].id);
                if (resource == resources_.end()) {
                    lastError_ = "D3D12 material binding '" + binding.name +
                                 "' contains an invalid resource";
                    release_material_ranges(material);
                    return false;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE source{};
                if (binding.type == DescriptorType::Sampler) {
                    if (!resource->second.samplerValid) {
                        lastError_ = "D3D12 sampler binding '" + binding.name +
                                     "' contains an invalid sampler";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.samplerCpu;
                } else if (binding.type == DescriptorType::UniformBuffer) {
                    if (!resource->second.cbvValid) {
                        lastError_ = "D3D12 uniform buffer binding '" + binding.name +
                                     "' contains an invalid buffer";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.cbvCpu;
                } else if (binding.type == DescriptorType::StorageBuffer ||
                           binding.type == DescriptorType::StorageTexture) {
                    if (!resource->second.uavValid) {
                        lastError_ = "D3D12 storage binding '" + binding.name +
                                     "' contains an invalid resource";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.uavCpu;
                } else {
                    if (!resource->second.srvValid) {
                        lastError_ = "D3D12 shader resource binding '" + binding.name +
                                     "' contains an invalid resource";
                        release_material_ranges(material);
                        return false;
                    }
                    source = resource->second.srvCpu;
                }
                if (!source.ptr) {
                    release_material_ranges(material);
                    return false;
                }
                auto destination =
                    (range.samplerHeap ? samplerHeap_->GetCPUDescriptorHandleForHeapStart()
                                       : srvHeap_->GetCPUDescriptorHandleForHeapStart());
                destination.ptr += static_cast<SIZE_T>(range.base + index) *
                                   (range.samplerHeap ? samplerStride_ : srvStride_);
                device_->CopyDescriptorsSimple(1, destination, source,
                                               range.samplerHeap
                                                   ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                                   : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        material.materialRangesValid = true;
        return true;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE
    material_range_gpu(const ResourceRecord::MaterialDescriptorRange &range) const {
        auto gpu = (range.samplerHeap ? samplerHeap_->GetGPUDescriptorHandleForHeapStart()
                                      : srvHeap_->GetGPUDescriptorHandleForHeapStart());
        gpu.ptr +=
            static_cast<UINT64>(range.base) * (range.samplerHeap ? samplerStride_ : srvStride_);
        return gpu;
    }
    bool take_rtv_slot(UINT &slot) {
        if (!freeRtvSlots_.empty()) {
            slot = freeRtvSlots_.back();
            freeRtvSlots_.pop_back();
            return true;
        }
        if (rtvCursor_ >= 64)
            return false;
        slot = rtvCursor_++;
        return true;
    }
    void release_rtv_slot(UINT slot) {
        if (slot >= 2 && slot < 64)
            freeRtvSlots_.push_back(slot);
    }
    bool take_dsv_slot(UINT &slot) {
        if (!freeDsvSlots_.empty()) {
            slot = freeDsvSlots_.back();
            freeDsvSlots_.pop_back();
            return true;
        }
        if (dsvCursor_ >= 64)
            return false;
        slot = dsvCursor_++;
        return true;
    }
    void release_dsv_slot(UINT slot) {
        if (slot >= 1 && slot < 64)
            freeDsvSlots_.push_back(slot);
    }
    bool allocate_srv(ResourceRecord &record, const D3D12_SHADER_RESOURCE_VIEW_DESC &description) {
        if (!srvHeap_ || !cpuSrvHeap_)
            return false;
        UINT slot = 0;
        if (!take_srv_slot(slot))
            return false;
        auto sourceCpu = cpuSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        sourceCpu.ptr += static_cast<SIZE_T>(slot) * cpuSrvStride_;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot) * srvStride_;
        device_->CreateShaderResourceView(record.resource, &description, sourceCpu);
        auto destinationCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destinationCpu.ptr += static_cast<SIZE_T>(slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.srvCpu = sourceCpu;
        record.srvIndex = slot;
        record.srvGpu = gpu;
        record.srvValid = true;
        return true;
    }
    bool allocate_cbv(ResourceRecord &record, std::size_t sizeBytes) {
        if (!srvHeap_ || !cpuSrvHeap_ || !record.resource)
            return false;
        UINT slot = 0;
        if (!take_srv_slot(slot))
            return false;
        const auto requestedSize = std::max<std::size_t>(sizeBytes, 1);
        if (requestedSize > std::numeric_limits<UINT>::max() - 255u) {
            release_srv_slot(slot);
            return false;
        }
        const auto alignedSize = (static_cast<UINT>(requestedSize) + 255u) & ~255u;
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
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.cbvCpu = sourceCpu;
        record.cbvGpu = gpu;
        record.cbvIndex = slot;
        record.cbvValid = true;
        return true;
    }
    bool allocate_uav(ResourceRecord &record) {
        if (!srvHeap_ || !cpuSrvHeap_ || !record.resource)
            return false;
        UINT slot = 0;
        if (!take_srv_slot(slot))
            return false;
        auto sourceCpu = cpuSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        sourceCpu.ptr += static_cast<SIZE_T>(slot) * cpuSrvStride_;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot) * srvStride_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        if (record.kind == ResourceKind::Buffer) {
            // Buffer UAVs cannot infer their element layout from the resource.
            const auto stride = record.stride ? record.stride : 4u;
            if (!record.size || record.size % stride || record.size / stride > UINT_MAX) {
                release_srv_slot(slot);
                lastError_ = "D3D12 buffer UAV requires a valid element stride and count";
                return false;
            }
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Format = record.stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.Buffer.NumElements = static_cast<UINT>(record.size / stride);
            view.Buffer.StructureByteStride = record.stride;
            view.Buffer.Flags =
                record.stride ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
        }
        device_->CreateUnorderedAccessView(record.resource, nullptr,
                                           record.kind == ResourceKind::Buffer ? &view : nullptr,
                                           sourceCpu);
        auto destinationCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destinationCpu.ptr += static_cast<SIZE_T>(slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destinationCpu, sourceCpu,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        record.uavCpu = sourceCpu;
        record.uavGpu = gpu;
        record.uavIndex = slot;
        record.uavValid = true;
        return true;
    }
    ID3D12Heap *ensure_gpu_heap(const GpuAllocation &allocation) {
        if (!allocation)
            return nullptr;
        const auto existing = gpuHeaps_.find(allocation.blockId);
        if (existing != gpuHeaps_.end())
            return existing->second;
        const auto capacity = gpuMemoryAllocator_.block_capacity(allocation.blockId);
        if (capacity == 0)
            return nullptr;
        D3D12_HEAP_DESC description{};
        description.SizeInBytes = capacity;
        description.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        description.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        description.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        description.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap *heap = nullptr;
        const auto result = device_->CreateHeap(&description, IID_PPV_ARGS(&heap));
        if (FAILED(result) || !heap) {
            lastError_ = "D3D12 native GPU heap creation failed hr=" +
                         std::to_string(static_cast<long long>(result));
            if (heap)
                heap->Release();
            return nullptr;
        }
        gpuHeaps_.emplace(allocation.blockId, heap);
        return heap;
    }
    bool try_create_placed_resource(ResourceRecord &record, const D3D12_RESOURCE_DESC &native,
                                    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear) {
        if (!device_)
            return false;
        const auto allocationInfo = device_->GetResourceAllocationInfo(0, 1, &native);
        if (allocationInfo.SizeInBytes == 0 || allocationInfo.SizeInBytes == UINT64_MAX)
            return false;
        const auto allocation = gpuMemoryAllocator_.allocate(
            static_cast<std::size_t>(allocationInfo.SizeInBytes),
            static_cast<std::size_t>(allocationInfo.Alignment), 0, GpuMemoryLifetime::Persistent);
        if (!allocation)
            return false;
        auto *heap = ensure_gpu_heap(allocation);
        if (!heap) {
            gpuMemoryAllocator_.release(allocation);
            return false;
        }
        ID3D12Resource *resource = nullptr;
        const auto result = device_->CreatePlacedResource(heap, allocation.offset, &native, state,
                                                          clear, IID_PPV_ARGS(&resource));
        if (FAILED(result) || !resource) {
            gpuMemoryAllocator_.release(allocation);
            if (resource)
                resource->Release();
            lastError_ =
                "D3D12 placed resource creation failed; falling back to committed resource hr=" +
                std::to_string(static_cast<long long>(result));
            return false;
        }
        record.resource = resource;
        record.gpuAllocation = allocation;
        record.nativeDescription = native;
        record.hasClearValue = clear != nullptr;
        if (clear)
            record.clearValue = *clear;
        record.placedResource = true;
        return true;
    }
    void release_gpu_allocation(ResourceRecord &record) {
        if (!record.alias && record.gpuAllocation)
            gpuMemoryAllocator_.release(record.gpuAllocation);
        record.gpuAllocation = {};
        record.placedResource = false;
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
            lastError_ = "D3D12 resource handle and description kind do not match";
            return false;
        }
        if (const auto *pipeline = std::get_if<PipelineDesc>(&description);
            pipeline && !validate_pipeline_state(*pipeline, lastError_))
            return false;
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        bool descriptorCreationFailed = false;
        bool resourceCreationFailed = false;
        std::visit(
            [&](const auto &desc) {
                using T = std::decay_t<decltype(desc)>;
                if constexpr (std::is_same_v<T, TextureDesc>) {
                    TexturePhysicalImagePlan plan;
                    if (!prepare_backend_texture(desc, BackendApi::DirectX12, plan, lastError_))
                        return;
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
                    const bool depth = handle.kind == ResourceKind::DepthStencil ||
                                       texture_format_is_depth(plan.format);
                    native.Flags =
                        depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                              : ((desc.renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                                    : D3D12_RESOURCE_FLAG_NONE) |
                                 (desc.storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_FLAG_NONE));
                    D3D12_HEAP_PROPERTIES heap{};
                    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_CLEAR_VALUE clear{};
                    clear.Format = record.format;
                    if (depth)
                        native.Format = record.format == DXGI_FORMAT_D32_FLOAT
                                            ? DXGI_FORMAT_R32_TYPELESS
                                            : DXGI_FORMAT_R24G8_TYPELESS;
                    if (depth)
                        clear.DepthStencil = {1.0f, 0};
                    else
                        clear.Color[3] = 1.0f;
                    record.nativeDescription = native;
                    record.hasClearValue = desc.renderTarget || depth;
                    if (record.hasClearValue)
                        record.clearValue = clear;
                    const auto *clearValue = record.hasClearValue ? &clear : nullptr;
                    if (!try_create_placed_resource(record, native, D3D12_RESOURCE_STATE_COMMON,
                                                    clearValue)) {
                        const auto placedError = lastError_;
                        lastError_.clear();
                        device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                                                         D3D12_RESOURCE_STATE_COMMON, clearValue,
                                                         IID_PPV_ARGS(&record.resource));
                        if (!record.resource && !placedError.empty()) {
                            lastError_ = placedError + "; committed-resource fallback also failed";
                        }
                    }
                    if (record.resource) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = depth ? (record.format == DXGI_FORMAT_D32_FLOAT
                                                  ? DXGI_FORMAT_R32_FLOAT
                                                  : DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
                                           : record.format;
                        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        if (native.SampleDesc.Count > 1) {
                            srv.ViewDimension = plan.physicalArrayLayers > 1
                                                    ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                    : D3D12_SRV_DIMENSION_TEXTURE2DMS;
                            if (plan.physicalArrayLayers > 1)
                                srv.Texture2DMSArray.ArraySize = plan.physicalArrayLayers;
                        } else if (plan.physicalArrayLayers > 1) {
                            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                            srv.Texture2DArray.MipLevels = native.MipLevels;
                            srv.Texture2DArray.ArraySize = plan.physicalArrayLayers;
                        } else {
                            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srv.Texture2D.MipLevels = native.MipLevels;
                        }
                        if (!allocate_srv(record, srv))
                            descriptorCreationFailed = true;
                        if (desc.storage && !depth && !allocate_uav(record))
                            descriptorCreationFailed = true;
                        if (desc.renderTarget && !depth && rtvHeap_) {
                            UINT slot = 0;
                            if (!take_rtv_slot(slot)) {
                                resourceCreationFailed = true;
                                return;
                            }
                            auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
                            rtv.ptr += static_cast<SIZE_T>(slot) * rtvStride_;
                            device_->CreateRenderTargetView(record.resource, nullptr, rtv);
                            record.rtvCpu = rtv;
                            record.rtvIndex = slot;
                            record.rtvValid = true;
                        }
                    }
                    if (record.resource && depth && dsvHeap_) {
                        UINT slot = 0;
                        if (!take_dsv_slot(slot)) {
                            resourceCreationFailed = true;
                            return;
                        }
                        auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
                        dsv.ptr += static_cast<SIZE_T>(slot) * dsvStride_;
                        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
                        view.Format = record.format;
                        if (native.SampleDesc.Count > 1) {
                            view.ViewDimension = record.layers > 1
                                                     ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY
                                                     : D3D12_DSV_DIMENSION_TEXTURE2DMS;
                            if (record.layers > 1)
                                view.Texture2DMSArray.ArraySize = record.layers;
                        } else if (record.layers > 1) {
                            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                            view.Texture2DArray.ArraySize = record.layers;
                        } else
                            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                        device_->CreateDepthStencilView(record.resource, &view, dsv);
                        record.dsvCpu = dsv;
                        record.dsvIndex = slot;
                        record.dsvValid = true;
                    }
                } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                    if (!samplerHeap_) {
                        resourceCreationFailed = true;
                        return;
                    }
                    UINT slot = 0;
                    if (!take_sampler_slot(slot)) {
                        resourceCreationFailed = true;
                        return;
                    }
                    D3D12_SAMPLER_DESC sampler{};
                    sampler.Filter = desc.filter == "nearest" ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                                              : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    sampler.AddressU = desc.addressU == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                                : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    sampler.AddressV = desc.addressV == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                                : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    sampler.AddressW = desc.addressW == "clamp" ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                                : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    sampler.MaxAnisotropy = static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy));
                    sampler.ComparisonFunc = desc.compareEnable ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                                : D3D12_COMPARISON_FUNC_ALWAYS;
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
                    if (desc.size > std::numeric_limits<UINT>::max() ||
                        desc.stride > std::numeric_limits<UINT>::max() ||
                        (desc.structuredBuffer && desc.stride != 0 &&
                         desc.size / desc.stride > std::numeric_limits<UINT>::max())) {
                        lastError_ = "D3D12 buffer dimensions exceed view limits";
                        resourceCreationFailed = true;
                        return;
                    }
                    record.size = desc.size;
                    record.stride = static_cast<UINT>(desc.stride);
                    record.indexBuffer = desc.indexBuffer;
                    D3D12_RESOURCE_DESC native{};
                    native.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    const bool constantBuffer = desc.uniformBuffer && !desc.vertexBuffer &&
                                                !desc.indexBuffer && !desc.indirectBuffer &&
                                                !desc.structuredBuffer && !desc.storageBuffer;
                    auto nativeWidth = std::max<std::uint64_t>(desc.size, 1);
                    if (constantBuffer) {
                        if (nativeWidth > std::numeric_limits<std::uint64_t>::max() - 255u) {
                            lastError_ = "D3D12 constant buffer size exceeds alignment limits";
                            resourceCreationFailed = true;
                            return;
                        }
                        nativeWidth = (nativeWidth + 255u) & ~std::uint64_t{255u};
                    }
                    native.Width = nativeWidth;
                    native.Height = 1;
                    native.DepthOrArraySize = 1;
                    native.MipLevels = 1;
                    native.SampleDesc.Count = 1;
                    native.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if (desc.storageBuffer)
                        native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    D3D12_HEAP_PROPERTIES heap{};
                    heap.Type = (desc.initialData.empty() || desc.storageBuffer)
                                    ? D3D12_HEAP_TYPE_DEFAULT
                                    : D3D12_HEAP_TYPE_UPLOAD;
                    record.uploadHeap = heap.Type == D3D12_HEAP_TYPE_UPLOAD;
                    if (desc.indirectBuffer)
                        record.state = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                    else if (!desc.initialData.empty())
                        record.state = D3D12_RESOURCE_STATE_GENERIC_READ;
                    record.nativeDescription = native;
                    // Upload heaps have stricter placement rules and are kept on
                    // the committed path. Default GPU buffers use the pooled
                    // placed-resource path and fall back safely if unavailable.
                    if (heap.Type == D3D12_HEAP_TYPE_DEFAULT) {
                        if (!try_create_placed_resource(record, native, record.state, nullptr)) {
                            const auto placedError = lastError_;
                            lastError_.clear();
                            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                                                             record.state, nullptr,
                                                             IID_PPV_ARGS(&record.resource));
                            if (!record.resource && !placedError.empty()) {
                                lastError_ =
                                    placedError + "; committed-resource fallback also failed";
                            }
                        }
                    } else {
                        const auto result = device_->CreateCommittedResource(
                            &heap, D3D12_HEAP_FLAG_NONE, &native, record.state, nullptr,
                            IID_PPV_ARGS(&record.resource));
                        if (FAILED(result))
                            append_d3d12_diagnostics("buffer committed resource creation", result);
                    }
                    if (record.resource && !desc.initialData.empty()) {
                        void *mapped = nullptr;
                        D3D12_RANGE range{0, 0};
                        if (SUCCEEDED(record.resource->Map(0, &range, &mapped))) {
                            std::memcpy(mapped, desc.initialData.data(),
                                        std::min<std::size_t>(desc.initialData.size(), desc.size));
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
                        if (!allocate_srv(record, srv))
                            descriptorCreationFailed = true;
                    }
                    if (record.resource && desc.storageBuffer && !allocate_uav(record))
                        descriptorCreationFailed = true;
                    if (record.resource && desc.uniformBuffer && !desc.vertexBuffer &&
                        !desc.indexBuffer && !desc.indirectBuffer && !desc.structuredBuffer &&
                        !desc.storageBuffer) {
                        if (!allocate_cbv(record, desc.size))
                            descriptorCreationFailed = true;
                    }
                } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                    const auto compiled = shaderCompiler_->compile(desc, BackendApi::DirectX12);
                    if (compiled.valid) {
                        if (!validate_backend_shader_layout(compiled.bindings, lastError_))
                            return;
                        record.shaderBytecode = compiled.bytecode;
                        record.shaderBindings = compiled.bindings;
                    } else
                        lastError_ = compiled.diagnostics;
                } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                    record.pipeline = desc;
                    record.pipeline.vertexLayout = resolve_vertex_layout(desc);
                } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                    record.material = desc;
                }
            },
            description);
        resources_.emplace(handle.id, std::move(record));
        if (const auto inserted = resources_.find(handle.id);
            inserted != resources_.end() && inserted->second.resource) {
            resourceStates_[inserted->second.resource] = inserted->second.state;
        }
        ++stats_.resourceCreates;
        if (const auto *buffer = std::get_if<BufferDesc>(&description);
            buffer && buffer->storageBuffer && !buffer->initialData.empty()) {
            if (!update_buffer({handle, 0, buffer->initialData})) {
                if (lastError_.empty())
                    lastError_ = "D3D12 initial buffer upload failed";
                else
                    lastError_ = "D3D12 initial buffer upload failed: " + lastError_;
                destroy_resource(handle);
                return false;
            }
        }
        if (const auto *texture = std::get_if<TextureDesc>(&description);
            texture && !texture->initialData.empty()) {
            const auto textureFormat = texture->formatKind == TextureFormat::Unknown
                                           ? texture_format_from_name(texture->format)
                                           : texture->formatKind;
            const auto bytesPerPixel = textureFormat == TextureFormat::RGBA16Float ? 8u : 4u;
            if (!update_texture({handle, 0, 0, texture->width, texture->height,
                                 static_cast<std::size_t>(texture->width) * bytesPerPixel,
                                 texture->initialData})) {
                if (lastError_.empty())
                    lastError_ = "D3D12 initial texture upload failed";
                else
                    lastError_ = "D3D12 initial texture upload failed: " + lastError_;
                destroy_resource(handle);
                return false;
            } else if (texture->generateMips &&
                       (texture->mipLevels == 0 || texture->mipLevels > 1) &&
                       !generate_mips(handle)) {
                if (lastError_.empty())
                    lastError_ = "D3D12 initial texture mip generation failed";
                else
                    lastError_ = "D3D12 initial texture mip generation failed: " + lastError_;
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
                        lastError_ = "D3D12 initial texture subresource upload failed";
                    else
                        lastError_ =
                            "D3D12 initial texture subresource upload failed: " + lastError_;
                    destroy_resource(handle);
                    return false;
                }
            }
        }
        const auto created = resources_.find(handle.id);
        if (resourceCreationFailed || descriptorCreationFailed) {
            lastError_ = resourceCreationFailed ? "D3D12 resource creation failed"
                                                : "D3D12 descriptor creation failed for resource";
            if (created != resources_.end())
                destroy_resource(handle);
            return false;
        }
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil ||
              handle.kind == ResourceKind::Buffer) &&
             !created->second.resource)) {
            if (lastError_.empty())
                lastError_ = "D3D12 resource creation failed";
            return false;
        }
        if (handle.kind == ResourceKind::Shader && created->second.shaderBytecode.empty()) {
            if (lastError_.empty())
                lastError_ = "D3D12 shader compilation failed";
            return false;
        }
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        retire_resource(handle);
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() ||
            source->second.kind != physical.kind || source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D &&
             source->second.kind != ResourceKind::DepthStencil &&
             source->second.kind != ResourceKind::Buffer) ||
            !source->second.resource)
            return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.resource = source->second.resource;
        alias.pipeline = {};
        alias.material = {};
        alias.state = source->second.state;
        alias.size = source->second.size;
        alias.stride = source->second.stride;
        alias.indexBuffer = source->second.indexBuffer;
        alias.nativeDescription = source->second.nativeDescription;
        alias.hasClearValue = source->second.hasClearValue;
        alias.clearValue = source->second.clearValue;
        alias.gpuAllocation = source->second.gpuAllocation;
        alias.placedResource = source->second.placedResource;
        alias.format = source->second.format;
        alias.uploadHeap = source->second.uploadHeap;
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
    bool update_buffer(const BufferUpdate &update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() ||
            it->second.kind != ResourceKind::Buffer) {
            lastError_ = "D3D12 buffer handle or resource kind is invalid";
            return false;
        }
        if (!it->second.resource) {
            lastError_ = "D3D12 buffer has no native resource";
            return false;
        }
        if (update.data.empty() || update.offset > it->second.size ||
            update.data.size() > it->second.size - update.offset) {
            lastError_ = "D3D12 buffer update exceeds resource size";
            return false;
        }
        if (it->second.uploadHeap &&
            (frameActive_ || (!uploadBatchActive_ && !wait_for_frame_writes()))) {
            if (lastError_.empty())
                lastError_ = "D3D12 host-visible updates require an idle frame resource";
            return false;
        }
        void *mapped = nullptr;
        D3D12_RANGE range{0, 0};
        const auto mapResult = it->second.resource->Map(0, &range, &mapped);
        if (SUCCEEDED(mapResult)) {
            std::memcpy(static_cast<std::uint8_t *>(mapped) + update.offset, update.data.data(),
                        update.data.size());
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
        ID3D12Resource *upload = nullptr;
        const HRESULT uploadResult = device_->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(uploadResult)) {
            append_d3d12_diagnostics("staging buffer creation", uploadResult);
            return false;
        }
        if (FAILED(upload->Map(0, &range, &mapped))) {
            upload->Release();
            lastError_ = "D3D12 staging buffer map failed";
            return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        upload->Unmap(0, nullptr);
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        const bool created =
            SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                 nullptr, IID_PPV_ARGS(&list)));
        if (!created) {
            if (list)
                list->Release();
            if (allocator)
                allocator->Release();
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
        if (FAILED(list->Close())) {
            list->Release();
            allocator->Release();
            upload->Release();
            lastError_ = "D3D12 staging buffer command list close failed";
            return false;
        }
        ID3D12CommandList *lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        ++fenceValue_;
        if (FAILED(queue_->Signal(fence_, fenceValue_))) {
            list->Release();
            allocator->Release();
            upload->Release();
            lastError_ = "D3D12 staging buffer fence signal failed";
            return false;
        }
        if (uploadBatchActive_) {
            pendingUploads_.push_back({fenceValue_, allocator, list, upload});
            return true;
        }
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (FAILED(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_)) ||
                WaitForSingleObject(fenceEvent_, INFINITE) != WAIT_OBJECT_0) {
                list->Release();
                allocator->Release();
                upload->Release();
                lastError_ = "D3D12 staging buffer fence wait failed";
                return false;
            }
        }
        list->Release();
        allocator->Release();
        upload->Release();
        return true;
    }
    bool update_texture(const TextureUpdate &update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() ||
            it->second.kind != ResourceKind::Texture2D || !it->second.resource ||
            update.data.empty() || update.width == 0 || update.height == 0 || frameActive_ ||
            !queue_ || !fence_ || !fenceEvent_) {
            lastError_ = "D3D12 texture handle or update data is invalid";
            return false;
        }
        auto &resource = it->second;
        if (resource.kind == ResourceKind::DepthStencil || update.mipLevel >= resource.mipLevels ||
            update.layer >= resource.layers) {
            lastError_ = "D3D12 texture update targets an invalid subresource";
            return false;
        }
        if (block_compressed_format(resource.format)) {
            lastError_ =
                "D3D12 compressed texture updates require block-compressed upload metadata";
            return false;
        }
        const auto bytesPerPixel = resource.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        const auto sourcePitch = update.rowPitch == 0
                                     ? static_cast<std::size_t>(update.width) * bytesPerPixel
                                     : update.rowPitch;
        if (update.x > std::max(1u, resource.width >> update.mipLevel) ||
            update.y > std::max(1u, resource.height >> update.mipLevel) ||
            update.width > std::max(1u, resource.width >> update.mipLevel) - update.x ||
            update.height > std::max(1u, resource.height >> update.mipLevel) - update.y ||
            sourcePitch < static_cast<std::size_t>(update.width) * bytesPerPixel ||
            sourcePitch > std::numeric_limits<std::size_t>::max() / update.height ||
            sourcePitch * update.height > update.data.size()) {
            lastError_ = "D3D12 texture update exceeds resource limits";
            return false;
        }
        std::vector<std::uint8_t> updatedBaseTextureData;
        const bool updatesRetainedBase =
            update.mipLevel == 0 && update.layer == 0 && bytesPerPixel == 4;
        if (updatesRetainedBase) {
            const auto fullSize = static_cast<std::size_t>(resource.width) * resource.height * 4u;
            updatedBaseTextureData = resource.baseTextureData;
            if (updatedBaseTextureData.size() != fullSize)
                updatedBaseTextureData.assign(fullSize, 0);
            for (std::uint32_t row = 0; row < update.height; ++row) {
                const auto destinationOffset =
                    (static_cast<std::size_t>(update.y + row) * resource.width + update.x) * 4u;
                std::memcpy(updatedBaseTextureData.data() + destinationOffset,
                            update.data.data() + row * sourcePitch,
                            static_cast<std::size_t>(update.width) * 4u);
            }
        }
        const auto rowBytes = static_cast<std::size_t>(update.width) * bytesPerPixel;
        if (rowBytes >
            std::numeric_limits<std::size_t>::max() - (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) {
            lastError_ = "D3D12 texture update row pitch overflows";
            return false;
        }
        const auto rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        if (rowPitch > std::numeric_limits<UINT>::max() ||
            update.height > std::numeric_limits<std::size_t>::max() / rowPitch) {
            lastError_ = "D3D12 texture update footprint exceeds resource limits";
            return false;
        }
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
        ID3D12Resource *upload = nullptr;
        if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)))) {
            lastError_ = "D3D12 texture staging buffer creation failed";
            return false;
        }
        void *mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(upload->Map(0, &readRange, &mapped))) {
            upload->Release();
            lastError_ = "D3D12 texture staging buffer map failed";
            return false;
        }
        for (std::uint32_t row = 0; row < update.height; ++row) {
            std::memcpy(static_cast<std::uint8_t *>(mapped) + row * rowPitch,
                        update.data.data() + row * sourcePitch,
                        std::min<std::size_t>(sourcePitch, rowPitch));
        }
        upload->Unmap(0, nullptr);
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&allocator))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                              IID_PPV_ARGS(&list)))) {
            if (list)
                list->Release();
            if (allocator)
                allocator->Release();
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
        if (FAILED(list->Close())) {
            list->Release();
            allocator->Release();
            upload->Release();
            lastError_ = "D3D12 texture staging command list close failed";
            return false;
        }
        ID3D12CommandList *lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        ++fenceValue_;
        if (FAILED(queue_->Signal(fence_, fenceValue_))) {
            list->Release();
            allocator->Release();
            upload->Release();
            lastError_ = "D3D12 texture staging fence signal failed";
            return false;
        }
        if (uploadBatchActive_) {
            if (updatesRetainedBase)
                resource.baseTextureData = std::move(updatedBaseTextureData);
            pendingUploads_.push_back({fenceValue_, allocator, list, upload});
            return true;
        }
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (FAILED(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_)) ||
                WaitForSingleObject(fenceEvent_, INFINITE) != WAIT_OBJECT_0) {
                list->Release();
                allocator->Release();
                upload->Release();
                lastError_ = "D3D12 texture staging fence wait failed";
                return false;
            }
        }
        list->Release();
        allocator->Release();
        upload->Release();
        if (updatesRetainedBase)
            resource.baseTextureData = std::move(updatedBaseTextureData);
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
        const auto &destinationDescription = destinationIt->second.nativeDescription;
        const auto &sourceDescription = sourceIt->second.nativeDescription;
        if (sourceDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            destinationDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            sourceDescription.SampleDesc.Count <= 1 ||
            destinationDescription.SampleDesc.Count != 1 ||
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
        if (const auto state = resourceStates_.find(sourceIt->second.resource);
            state != resourceStates_.end())
            sourceState = state->second;
        auto destinationState = destinationIt->second.state;
        if (const auto state = resourceStates_.find(destinationIt->second.resource);
            state != resourceStates_.end())
            destinationState = state->second;
        const auto addBarrier = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                    D3D12_RESOURCE_STATES after) {
            if (before == after)
                return;
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
        addBarrier(destinationIt->second.resource, destinationState,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST);
        for (UINT layer = 0; layer < sourceDescription.DepthOrArraySize; ++layer) {
            for (UINT mip = 0; mip < sourceDescription.MipLevels; ++mip) {
                const auto subresource = mip + layer * sourceDescription.MipLevels;
                commandList_->ResolveSubresource(destinationIt->second.resource, subresource,
                                                 sourceIt->second.resource, subresource,
                                                 sourceDescription.Format);
            }
        }
        addBarrier(sourceIt->second.resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceState);
        addBarrier(destinationIt->second.resource, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                   destinationState);
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() ||
            it->second.kind != ResourceKind::Texture2D || it->second.mipLevels <= 1) {
            lastError_ = "D3D12 mip generation handle or resource is invalid";
            return false;
        }
        auto &resource = it->second;
        if (resource.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            resource.baseTextureData.size() !=
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
            std::vector<std::uint8_t> destination(static_cast<std::size_t>(destinationWidth) *
                                                  destinationHeight * 4u);
            for (std::uint32_t y = 0; y < destinationHeight; ++y) {
                for (std::uint32_t x = 0; x < destinationWidth; ++x) {
                    for (std::uint32_t channel = 0; channel < 4; ++channel) {
                        std::uint32_t sum = 0;
                        for (std::uint32_t sampleY = 0; sampleY < 2; ++sampleY) {
                            for (std::uint32_t sampleX = 0; sampleX < 2; ++sampleX) {
                                const auto sourceX = std::min(sourceWidth - 1u, x * 2u + sampleX);
                                const auto sourceY = std::min(sourceHeight - 1u, y * 2u + sampleY);
                                sum += source[(static_cast<std::size_t>(sourceY) * sourceWidth +
                                               sourceX) *
                                                  4u +
                                              channel];
                            }
                        }
                        destination[(static_cast<std::size_t>(y) * destinationWidth + x) * 4u +
                                    channel] = static_cast<std::uint8_t>(sum / 4u);
                    }
                }
            }
            if (!update_texture(
                    {texture, level, 0, destinationWidth, destinationHeight, 0, destination}))
                return false;
            source = std::move(destination);
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        return true;
    }
    BindlessTableHandle create_bindless_table(const BindlessTableDesc &description) override {
        if (!capabilities_.supportsBindless || description.type != DescriptorType::Texture ||
            !srvHeap_ || description.capacity == 0 || description.capacity > 1024u) {
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
    bool update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource,
                         DescriptorType type) override {
        const auto tableIt = bindlessTables_.find(table.id);
        const auto resourceIt = resources_.find(resource.id);
        if (tableIt == bindlessTables_.end() || slot >= tableIt->second.capacity ||
            resourceIt == resources_.end() || !resourceIt->second.srvValid ||
            type != DescriptorType::Texture) {
            lastError_ = "D3D12 bindless update requires a texture SRV";
            return false;
        }
        auto destination = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        destination.ptr += static_cast<SIZE_T>(tableIt->second.baseIndex + slot) * srvStride_;
        device_->CopyDescriptorsSimple(1, destination, resourceIt->second.srvCpu,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }
    void destroy_bindless_table(BindlessTableHandle table) override {
        if (boundBindlessTable_ == table.id)
            boundBindlessTable_ = 0;
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end())
            return;
        retiredBindlessTables_.push_back({fenceValue_, it->second});
        bindlessTables_.erase(it);
    }
    void bind_bindless_table(BindlessTableHandle table) override {
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end() || !commandList_ || !srvHeap_)
            return;
        boundBindlessTable_ = table.id;
        ID3D12DescriptorHeap *heaps[] = {srvHeap_};
        commandList_->SetDescriptorHeaps(1, heaps);
        if (!boundPipeline_ || !boundPipeline_->rootSignature || !boundPipeline_->pipelineState)
            return;
        auto gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(it->second.baseIndex) * srvStride_;
        const auto parameter = boundPipeline_->rootParameters.find(
            d3d12_binding_key(0, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV));
        if (parameter == boundPipeline_->rootParameters.end())
            return;
        if (boundPipeline_->pipeline.computeShader)
            commandList_->SetComputeRootDescriptorTable(parameter->second, gpu);
        else
            commandList_->SetGraphicsRootDescriptorTable(parameter->second, gpu);
    }
    void retire_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind)
            return;
        if (boundPipeline_ == &it->second)
            boundPipeline_ = nullptr;
        if (boundMaterial_ == &it->second)
            boundMaterial_ = nullptr;
        if (it->second.kind == ResourceKind::Material)
            defer_material_ranges(it->second);
        if (!it->second.alias && it->second.resource)
            resourceStates_.erase(it->second.resource);
        retiredResources_.push_back({fenceValue_, std::move(it->second)});
        resources_.erase(it);
    }
    void collect_garbage() override {
        release_completed_uploads();
        const auto completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
        retiredBindlessTables_.erase(
            std::remove_if(retiredBindlessTables_.begin(), retiredBindlessTables_.end(),
                           [&](auto &retired) {
                               if (retired.first != 0 && retired.first > completed)
                                   return false;
                               for (std::uint32_t slot = 0; slot < retired.second.capacity;
                                    ++slot) {
                                   release_srv_slot(retired.second.baseIndex + slot);
                               }
                               return true;
                           }),
            retiredBindlessTables_.end());
        retiredMaterialRanges_.erase(
            std::remove_if(retiredMaterialRanges_.begin(), retiredMaterialRanges_.end(),
                           [&](auto &retired) {
                               if (retired.first != 0 && retired.first > completed)
                                   return false;
                               for (const auto &[key, range] : retired.second) {
                                   (void)key;
                                   release_sampler_range(range);
                               }
                               return true;
                           }),
            retiredMaterialRanges_.end());
        retiredResources_.erase(
            std::remove_if(retiredResources_.begin(), retiredResources_.end(),
                           [&](auto &retired) {
                               if (retired.first != 0 && retired.first > completed)
                                   return false;
                               // Alias records share the physical resource's descriptor slots.
                               // They retain the COM resource, but must never return the
                               // physical descriptors to a freelist a second time.
                               if (!retired.second.alias) {
                                   release_gpu_allocation(retired.second);
                                   if (retired.second.srvValid)
                                       release_srv_slot(retired.second.srvIndex);
                                   if (retired.second.cbvValid)
                                       release_srv_slot(retired.second.cbvIndex);
                                   if (retired.second.uavValid)
                                       release_srv_slot(retired.second.uavIndex);
                                   if (retired.second.samplerValid)
                                       release_sampler_slot(retired.second.samplerIndex);
                                   if (retired.second.rtvValid)
                                       release_rtv_slot(retired.second.rtvIndex);
                                   if (retired.second.dsvValid)
                                       release_dsv_slot(retired.second.dsvIndex);
                               }
                               ++stats_.resourceDestroys;
                               return true;
                           }),
            retiredResources_.end());
    }
    bool wait_for_frame_writes() {
        if (capabilities_.deviceState != RenderDeviceState::Ready || frameActive_)
            return false;
        if (lastSubmittedFrame_ && fence_->GetCompletedValue() < lastSubmittedFrame_) {
            if (FAILED(fence_->SetEventOnCompletion(lastSubmittedFrame_, fenceEvent_)) ||
                WaitForSingleObject(fenceEvent_, 5000) != WAIT_OBJECT_0) {
                lastError_ = "D3D12 frame-resource fence wait failed before host upload";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return false;
            }
        }
        return true;
    }
    void begin_upload_batch() override {
        uploadBatchActive_ = wait_for_frame_writes();
        release_completed_uploads();
    }
    bool flush_upload_batch() override {
        uploadBatchActive_ = false;
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        if (!commandList_) {
            lastError_ = "D3D12 resource transition command list is invalid";
            return false;
        }
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind || !it->second.resource) {
            lastError_ = "D3D12 resource transition handle is invalid";
            return false;
        }
        D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
        if (is_color_attachment_usage(usage))
            target = D3D12_RESOURCE_STATE_RENDER_TARGET;
        else if (usage == ResourceUsage::DepthStencil)
            target = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        else if (usage == ResourceUsage::VertexBuffer)
            target = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        else if (usage == ResourceUsage::IndexBuffer)
            target = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        else if (usage == ResourceUsage::UniformBuffer)
            target = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        else if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::StorageRead) {
            target = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        } else if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite) {
            target = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else if (usage == ResourceUsage::CopySource)
            target = D3D12_RESOURCE_STATE_COPY_SOURCE;
        else if (usage == ResourceUsage::CopyDestination)
            target = D3D12_RESOURCE_STATE_COPY_DEST;
        else if (usage == ResourceUsage::IndirectArguments)
            target = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        else if (usage == ResourceUsage::Present)
            target = D3D12_RESOURCE_STATE_PRESENT;
        // Upload-heap resources are permanently required to stay in
        // GENERIC_READ. That state already includes vertex, index, constant,
        // shader-read and indirect-argument reads; issuing a transition for
        // those usages is invalid on D3D12 and can remove the device on some
        // drivers. Writes/copy-destination usages are rejected explicitly
        // instead of recording an illegal barrier.
        if (it->second.uploadHeap) {
            const auto readOnlyState = D3D12_RESOURCE_STATE_GENERIC_READ;
            if ((target & readOnlyState) == target)
                return true;
            lastError_ = "D3D12 upload-heap buffer cannot transition to a write state";
            return false;
        }
        const auto stateIt = resourceStates_.find(it->second.resource);
        const auto currentState =
            stateIt == resourceStates_.end() ? it->second.state : stateIt->second;
        if (currentState == target && target == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = it->second.resource;
            commandList_->ResourceBarrier(1, &barrier);
            ++stats_.barriers;
            return true;
        }
        if (currentState == target || !it->second.resource)
            return true;
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
        if (!commandList_ || !frameActive_)
            return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        if (target) {
            const auto it = resources_.find(target.id);
            if (target.kind != ResourceKind::Texture2D || it == resources_.end() ||
                !it->second.rtvValid) {
                lastError_ = "D3D12 render target handle has no valid render-target view";
                return;
            }
            handle = it->second.rtvCpu;
        } else {
            handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
        }
        commandList_->OMSetRenderTargets(1, &handle, FALSE, dsvHeap_ ? &dsvHandle_ : nullptr);
        currentRenderTarget_ = target.id;
        currentDepthTarget_ = 0;
        if (!target)
            set_editor_viewport(editorViewport_);
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        if (!commandList_ || !frameActive_)
            return;
        D3D12_CPU_DESCRIPTOR_HANDLE colorHandle{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (color && colorIt != resources_.end() && colorIt->second.rtvValid)
            colorHandle = colorIt->second.rtvCpu;
        if (color && (color.kind != ResourceKind::Texture2D || colorIt == resources_.end() ||
                      !colorIt->second.rtvValid)) {
            lastError_ = "D3D12 color target handle has no valid render-target view";
            return;
        }
        if (!color) {
            colorHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            colorHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
        }
        if (depth) {
            if (depth.kind != ResourceKind::DepthStencil || depthIt == resources_.end() ||
                !depthIt->second.dsvValid) {
                lastError_ = "D3D12 depth target handle has no valid depth-stencil view";
                return;
            }
            depthHandle = depthIt->second.dsvCpu;
        }
        commandList_->OMSetRenderTargets(color ? 1u : 0u, color ? &colorHandle : nullptr, FALSE,
                                         depthHandle.ptr ? &depthHandle : nullptr);
        currentRenderTarget_ = color ? color.id : depth.id;
        currentDepthTarget_ = depth.id;
        const auto target = color ? colorIt : depthIt;
        if (target != resources_.end() && target->second.resource) {
            const auto extent = target->second.resource->GetDesc();
            set_viewport(0, 0, static_cast<float>(extent.Width), static_cast<float>(extent.Height),
                         0, 1);
        } else
            set_editor_viewport(editorViewport_);
    }
    void set_render_targets(const std::vector<ResourceHandle> &colors, ResourceHandle depth,
                            bool clearAttachments) override {
        if (!commandList_ || !frameActive_)
            return;
        if (colors.size() <= 1) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth);
            if (clearAttachments) {
                constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (!colors.empty()) {
                    const auto color = resources_.find(colors.front().id);
                    if (color != resources_.end() && color->second.rtvValid)
                        commandList_->ClearRenderTargetView(color->second.rtvCpu, clear, 0,
                                                            nullptr);
                }
                if (depth) {
                    const auto depthResource = resources_.find(depth.id);
                    if (depthResource != resources_.end() && depthResource->second.dsvValid) {
                        commandList_->ClearDepthStencilView(
                            depthResource->second.dsvCpu,
                            depthResource->second.format == DXGI_FORMAT_D24_UNORM_S8_UINT
                                ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL
                                : D3D12_CLEAR_FLAG_DEPTH,
                            1.0f, 0, 0, nullptr);
                    }
                }
            }
            return;
        }
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> colorHandles{};
        UINT count = static_cast<UINT>(std::min<std::size_t>(colors.size(), colorHandles.size()));
        for (UINT index = 0; index < count; ++index) {
            const auto it = resources_.find(colors[index].id);
            if (colors[index].kind != ResourceKind::Texture2D || it == resources_.end() ||
                !it->second.rtvValid) {
                lastError_ = "D3D12 color attachment has no valid render-target view";
                return;
            }
            colorHandles[index] = it->second.rtvCpu;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (depth.kind != ResourceKind::DepthStencil || it == resources_.end() ||
                !it->second.dsvValid) {
                lastError_ = "D3D12 depth attachment has no valid depth-stencil view";
                return;
            }
            depthHandle = it->second.dsvCpu;
        }
        commandList_->OMSetRenderTargets(count, count ? colorHandles.data() : nullptr, FALSE,
                                         depthHandle.ptr ? &depthHandle : nullptr);
        if (clearAttachments) {
            constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (UINT index = 0; index < count; ++index) {
                if (colorHandles[index].ptr)
                    commandList_->ClearRenderTargetView(colorHandles[index], clear, 0, nullptr);
            }
            if (depth && depthHandle.ptr) {
                const auto depthIt = resources_.find(depth.id);
                commandList_->ClearDepthStencilView(
                    depthHandle,
                    depthIt != resources_.end() &&
                            depthIt->second.format == DXGI_FORMAT_D24_UNORM_S8_UINT
                        ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL
                        : D3D12_CLEAR_FLAG_DEPTH,
                    1.0f, 0, 0, nullptr);
            }
        }
        currentRenderTarget_ = colors.empty() ? depth.id : colors.front().id;
        currentDepthTarget_ = depth.id;
        const auto target = resources_.find(currentRenderTarget_);
        if (target != resources_.end() && target->second.resource) {
            const auto extent = target->second.resource->GetDesc();
            set_viewport(0, 0, static_cast<float>(extent.Width), static_cast<float>(extent.Height),
                         0, 1);
        }
    }
    bool clear_render_attachments(const std::vector<ResourceHandle> &colors, ResourceHandle depth,
                                  bool clearColors, bool clearDepth) override {
        if (!commandList_)
            return false;
        if (clearColors)
            for (const auto color : colors) {
                const auto found = resources_.find(color.id);
                if (found == resources_.end() || !found->second.rtvValid)
                    return false;
                constexpr float clear[]{0, 0, 0, 1};
                commandList_->ClearRenderTargetView(found->second.rtvCpu, clear, 0, nullptr);
            }
        if (clearDepth && depth) {
            const auto found = resources_.find(depth.id);
            if (found == resources_.end() || !found->second.dsvValid)
                return false;
            commandList_->ClearDepthStencilView(
                found->second.dsvCpu,
                found->second.format == DXGI_FORMAT_D24_UNORM_S8_UINT
                    ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL
                    : D3D12_CLEAR_FLAG_DEPTH,
                1, 0, 0, nullptr);
        }
        return true;
    }
    bool bind_editor_render_target(ResourceHandle color, ResourceHandle depth,
                                   bool clearAttachments) override {
        if (!commandList_ || !frameActive_)
            return false;
        if (!color && !depth) {
            set_render_target({});
            if (clearAttachments) {
                auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvStride_;
                constexpr float clear[]{0, 0, 0, 1};
                commandList_->ClearRenderTargetView(handle, clear, 0, nullptr);
            }
        } else {
            set_render_targets(color ? std::vector<ResourceHandle>{color}
                                     : std::vector<ResourceHandle>{},
                               depth, clearAttachments);
        }
        return lastError_.empty();
    }
    void set_viewport(float x, float y, float width, float height, float minDepth,
                      float maxDepth) override {
        if (!commandList_ || !frameActive_)
            return;
        D3D12_VIEWPORT viewport{x, y, width, height, minDepth, maxDepth};
        D3D12_RECT scissor{static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y)),
                           static_cast<LONG>(std::ceil(x + width)),
                           static_cast<LONG>(std::ceil(y + height))};
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
    }
    bool set_editor_viewport(const EditorViewportSeam &seam) override {
        if (seam.enabled &&
            (!seam.viewport.valid() || (seam.scissor.width != 0 && !seam.scissor.valid()))) {
            lastError_ = "D3D12 editor viewport or scissor is invalid";
            return false;
        }
        editorViewport_ = seam;
        if (frameActive_ && commandList_ && currentRenderTarget_ == 0) {
            const auto viewport = seam.enabled
                                      ? seam.viewport
                                      : EditorViewportRect{0, 0, static_cast<float>(width_),
                                                           static_cast<float>(height_)};
            set_viewport(viewport.x, viewport.y, viewport.width, viewport.height, 0, 1);
            const auto clip = seam.enabled && seam.scissor.valid() ? seam.scissor : viewport;
            const D3D12_RECT rect{static_cast<LONG>(std::max(0.0f, std::floor(clip.x))),
                                  static_cast<LONG>(std::max(0.0f, std::floor(clip.y))),
                                  static_cast<LONG>(std::min(static_cast<float>(width_),
                                                             std::ceil(clip.x + clip.width))),
                                  static_cast<LONG>(std::min(static_cast<float>(height_),
                                                             std::ceil(clip.y + clip.height)))};
            commandList_->RSSetScissorRects(1, &rect);
        }
        return true;
    }
    bool ensure_pipeline(ResourceRecord &record) {
        if (record.pipelineState)
            return true;
        if (!device_ || !commandList_)
            return false;
        if (record.pipeline.computeShader != 0) {
            const auto compute = resources_.find(record.pipeline.computeShader);
            if (compute == resources_.end() || compute->second.shaderBytecode.empty())
                return false;
            std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
            std::vector<std::uint64_t> keys;
            const auto addBinding = [&](std::uint32_t space, std::uint32_t slot,
                                        D3D12_DESCRIPTOR_RANGE_TYPE type, std::uint32_t count = 1,
                                        bool unbounded = false) {
                const auto key = d3d12_binding_key(space, slot, type);
                const auto existing = std::find(keys.begin(), keys.end(), key);
                if (existing != keys.end()) {
                    const auto index =
                        static_cast<std::size_t>(std::distance(keys.begin(), existing));
                    return ranges[index].RangeType == type &&
                           ranges[index].NumDescriptors == (unbounded ? UINT_MAX : count);
                }
                if (keys.size() >= 64)
                    return false;
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = type;
                range.BaseShaderRegister = slot;
                range.RegisterSpace = space;
                range.NumDescriptors = unbounded ? UINT_MAX : count;
                ranges.push_back(range);
                keys.push_back(key);
                return true;
            };
            const auto addReflected = [&](const ShaderBinding &binding) {
                if (binding.count == 0 && !binding.unbounded)
                    return false;
                if (binding.type == "sampler")
                    return addBinding(binding.space, binding.slot,
                                      D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, binding.count,
                                      binding.unbounded);
                if (binding.type == "uniform_buffer")
                    return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                                      binding.count, binding.unbounded);
                if (binding.type == "storage_buffer" || binding.type == "storage_texture")
                    return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                      binding.count, binding.unbounded);
                return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                  binding.count, binding.unbounded);
            };
            for (const auto &reflected : compute->second.shaderBindings)
                if (!addReflected(reflected))
                    return false;
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
            ID3DBlob *serialized = nullptr;
            ID3DBlob *errors = nullptr;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &serialized, &errors))) {
                lastError_ = "D3D12 compute root signature serialization failed";
                if (errors && errors->GetBufferPointer()) {
                    lastError_ += " [" +
                                  std::string(static_cast<const char *>(errors->GetBufferPointer()),
                                              errors->GetBufferSize()) +
                                  "]";
                }
                if (errors)
                    errors->Release();
                return false;
            }
            const auto rootResult = device_->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&record.rootSignature));
            serialized->Release();
            if (FAILED(rootResult)) {
                append_d3d12_diagnostics("compute root signature creation", rootResult);
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = record.rootSignature;
            pipeline.CS = {compute->second.shaderBytecode.data(),
                           compute->second.shaderBytecode.size()};
            const auto cacheName = pipeline_library_name(
                record.pipeline, {}, {}, compute->second.shaderBytecode, L":compute");
            if (pipelineLibrary_ &&
                SUCCEEDED(pipelineLibrary_->LoadComputePipeline(
                    cacheName.c_str(), &pipeline, IID_PPV_ARGS(&record.pipelineState)))) {
                ++stats_.pipelineCacheHits;
                return true;
            }
            if (FAILED(device_->CreateComputePipelineState(&pipeline,
                                                           IID_PPV_ARGS(&record.pipelineState)))) {
                record.rootSignature->Release();
                record.rootSignature = nullptr;
                return false;
            }
            if (pipelineLibrary_)
                pipelineLibrary_->StorePipeline(cacheName.c_str(), record.pipelineState);
            return true;
        }
        if (!is_supported_sample_count(record.pipeline.sampleCount) ||
            (record.pipeline.fillMode != "solid" && record.pipeline.fillMode != "wireframe") ||
            (record.pipeline.cullMode != "back" && record.pipeline.cullMode != "front" &&
             record.pipeline.cullMode != "none") ||
            (record.pipeline.topology != "triangle" && record.pipeline.topology != "line" &&
             record.pipeline.topology != "point") ||
            record.pipeline.colorFormats.size() > 8 ||
            std::any_of(
                record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string &format) { return !is_supported_color_format(format); }) ||
            !is_supported_color_format(record.pipeline.colorFormat) ||
            !is_supported_pipeline_depth_format(record.pipeline.depthFormat)) {
            lastError_ = "D3D12 pipeline fixed-function state or attachment format is unsupported";
            return false;
        }
        const auto vertex = resources_.find(record.pipeline.vertexShader);
        const auto fragment = resources_.find(record.pipeline.fragmentShader);
        if (vertex == resources_.end() || fragment == resources_.end() ||
            vertex->second.kind != ResourceKind::Shader ||
            fragment->second.kind != ResourceKind::Shader ||
            vertex->second.shaderBytecode.empty() || fragment->second.shaderBytecode.empty()) {
            lastError_ = "D3D12 graphics pipeline references missing or empty shader bytecode";
            return false;
        }
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
        std::vector<std::uint64_t> keys;
        const auto addBinding = [&](std::uint32_t space, std::uint32_t slot,
                                    D3D12_DESCRIPTOR_RANGE_TYPE type, std::uint32_t count = 1,
                                    bool unbounded = false) {
            const auto key = d3d12_binding_key(space, slot, type);
            const auto existing = std::find(keys.begin(), keys.end(), key);
            if (existing != keys.end()) {
                const auto index = static_cast<std::size_t>(std::distance(keys.begin(), existing));
                return ranges[index].RangeType == type &&
                       ranges[index].NumDescriptors == (unbounded ? UINT_MAX : count);
            }
            if (keys.size() >= 64)
                return false;
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = type;
            range.BaseShaderRegister = slot;
            range.RegisterSpace = space;
            range.NumDescriptors = unbounded ? UINT_MAX : count;
            ranges.push_back(range);
            keys.push_back(key);
            return true;
        };
        const auto addReflected = [&](const ShaderBinding &binding) {
            if (binding.count == 0 && !binding.unbounded)
                return false;
            if (binding.type == "sampler")
                return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                                  binding.count, binding.unbounded);
            if (binding.type == "uniform_buffer")
                return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                                  binding.count, binding.unbounded);
            if (binding.type == "storage_buffer" || binding.type == "storage_texture")
                return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                  binding.count, binding.unbounded);
            return addBinding(binding.space, binding.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                              binding.count, binding.unbounded);
        };
        for (const auto shaderId : {record.pipeline.vertexShader, record.pipeline.fragmentShader}) {
            const auto shader = resources_.find(shaderId);
            if (shader == resources_.end())
                continue;
            for (const auto &reflected : shader->second.shaderBindings)
                if (!addReflected(reflected))
                    return false;
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
        ID3DBlob *serialized = nullptr;
        ID3DBlob *errors = nullptr;
        const auto serializeResult = D3D12SerializeRootSignature(
            &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (FAILED(serializeResult)) {
            lastError_ = "D3D12 graphics root signature serialization failed hr=" +
                         std::to_string(static_cast<long long>(serializeResult));
            if (errors && errors->GetBufferPointer()) {
                lastError_ += " [" +
                              std::string(static_cast<const char *>(errors->GetBufferPointer()),
                                          errors->GetBufferSize()) +
                              "]";
            }
            if (errors)
                errors->Release();
            return false;
        }
        const HRESULT rootResult = device_->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&record.rootSignature));
        serialized->Release();
        if (FAILED(rootResult)) {
            append_d3d12_diagnostics("graphics root signature creation", rootResult);
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = record.rootSignature;
        pipeline.VS = {vertex->second.shaderBytecode.data(), vertex->second.shaderBytecode.size()};
        pipeline.PS = {fragment->second.shaderBytecode.data(),
                       fragment->second.shaderBytecode.size()};
        const auto layout = resolve_vertex_layout(record.pipeline);
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
        for (const auto &attribute : layout.attributes) {
            const auto binding =
                std::find_if(layout.bindings.begin(), layout.bindings.end(),
                             [&](const auto &value) { return value.binding == attribute.binding; });
            inputElements.push_back(
                {attribute.semantic.c_str(), attribute.semanticIndex,
                 dxgi_vertex_format(attribute.format), attribute.binding, attribute.offset,
                 binding->perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                      : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                 binding->perInstance ? binding->stepRate : 0});
        }
        pipeline.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};
        pipeline.RasterizerState.FillMode = fill_mode(record.pipeline.fillMode);
        pipeline.RasterizerState.CullMode = cull_mode(record.pipeline.cullMode);
        fill_dxgi_raster(pipeline.RasterizerState, record.pipeline.rasterizerState);
        const auto attachments = resolve_blend_attachments(record.pipeline);
        pipeline.BlendState.IndependentBlendEnable = attachments.size() > 1;
        for (std::size_t index = 0; index < attachments.size(); ++index)
            fill_dxgi_blend(pipeline.BlendState.RenderTarget[index], attachments[index]);
        fill_dxgi_depth(pipeline.DepthStencilState, resolve_depth_stencil(record.pipeline));
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = topology_type(record.pipeline.topology);
        const auto colorFormats = record.pipeline.colorFormats.empty()
                                      ? std::vector<std::string>{record.pipeline.colorFormat}
                                      : record.pipeline.colorFormats;
        pipeline.NumRenderTargets = static_cast<UINT>(colorFormats.size());
        for (UINT index = 0; index < pipeline.NumRenderTargets; ++index) {
            pipeline.RTVFormats[index] = format_of(colorFormats[index]);
        }
        pipeline.DSVFormat = record.pipeline.depthFormat == "none"
                                 ? DXGI_FORMAT_UNKNOWN
                                 : format_of(record.pipeline.depthFormat);
        if (record.pipeline.depthFormat == "none") {
            pipeline.DepthStencilState.DepthEnable = FALSE;
            pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        pipeline.SampleDesc.Count = record.pipeline.sampleCount;
        const auto cacheName = pipeline_library_name(record.pipeline, vertex->second.shaderBytecode,
                                                     fragment->second.shaderBytecode);
        if (pipelineLibrary_ &&
            SUCCEEDED(pipelineLibrary_->LoadGraphicsPipeline(
                cacheName.c_str(), &pipeline, IID_PPV_ARGS(&record.pipelineState)))) {
            ++stats_.pipelineCacheHits;
            return true;
        }
        const auto pipelineResult =
            device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&record.pipelineState));
        if (FAILED(pipelineResult)) {
            record.rootSignature->Release();
            record.rootSignature = nullptr;
            lastError_ = "D3D12 graphics pipeline creation failed hr=" +
                         std::to_string(static_cast<long long>(pipelineResult));
            ID3D12InfoQueue *infoQueue = nullptr;
            if (device_ && SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue))) &&
                infoQueue) {
                const auto messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
                for (UINT64 index = 0; index < messageCount; ++index) {
                    SIZE_T messageSize = 0;
                    if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) ||
                        messageSize == 0)
                        continue;
                    std::vector<std::uint8_t> messageStorage(messageSize);
                    auto *message = reinterpret_cast<D3D12_MESSAGE *>(messageStorage.data());
                    if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)) &&
                        message->pDescription) {
                        lastError_ += " [" + std::string(message->pDescription) + "]";
                    }
                }
                infoQueue->ClearStoredMessages();
                infoQueue->Release();
            }
            return false;
        }
        if (pipelineLibrary_)
            pipelineLibrary_->StorePipeline(cacheName.c_str(), record.pipelineState);
        auto depthOnly = pipeline;
        depthOnly.NumRenderTargets = 0;
        depthOnly.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        if (FAILED(device_->CreateGraphicsPipelineState(&depthOnly,
                                                        IID_PPV_ARGS(&record.depthOnlyPipeline))) &&
            record.depthOnlyPipeline) {
            record.depthOnlyPipeline->Release();
            record.depthOnlyPipeline = nullptr;
        }
        return true;
    }
    void begin_frame() override {
        ++stats_.frames;
        recordedQueueBatches_.clear();
        currentRenderTarget_ = 0;
        currentDepthTarget_ = 0;
        if (!commandList_ || !commandAllocator_)
            return;
        const auto deviceReason = device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
        if (deviceReason == DXGI_ERROR_DEVICE_REMOVED || deviceReason == DXGI_ERROR_DEVICE_RESET) {
            lastError_ = "D3D12 device lost before frame begin hr=" +
                         std::to_string(static_cast<long long>(deviceReason));
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
        if (swapChain_)
            frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
        if (FAILED(commandAllocator_->Reset()) ||
            FAILED(commandList_->Reset(commandAllocator_, nullptr))) {
            lastError_ = "D3D12 command recording reset failed";
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState =
                reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
            return;
        }
        if (!swapChain_) {
            timestampCursor_ = 0;
            frameActive_ = true;
            return;
        }
        if (frameIndex_ >= backBuffers_.size() || !backBuffers_[frameIndex_] || !rtvHeap_) {
            lastError_ = "D3D12 swapchain backbuffer is unavailable after resize";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
            return;
        }
        auto *buffer = backBuffers_[frameIndex_];
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
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_),
                                0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
        constexpr float clear[4] = {0.035f, 0.045f, 0.065f, 1.0f};
        commandList_->ClearRenderTargetView(handle, clear, 0, nullptr);
        if (dsvHeap_)
            commandList_->ClearDepthStencilView(
                dsvHandle_, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        frameActive_ = true;
        set_editor_viewport(editorViewport_);
    }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t> &waitBatches) override {
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
        if (recordedQueueBatches_.size() <= batchIndex)
            recordedQueueBatches_.resize(batchIndex + 1, false);
        recordedQueueBatches_[batchIndex] = true;
        return true;
    }
    void begin_pass() override {
        if (timestampQueryHeap_ && commandList_ && frameActive_ && timestampCursor_ < 512) {
            commandList_->EndQuery(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP,
                                   timestampCursor_++);
        }
    }
    void execute(const RenderPassContext &) override {
        ++stats_.passes;
    }
    void end_pass() override {
        if (timestampQueryHeap_ && commandList_ && frameActive_ && timestampCursor_ < 512) {
            commandList_->EndQuery(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP,
                                   timestampCursor_++);
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
            commandList_->BeginEvent(0, wide.c_str(),
                                     static_cast<UINT>(wide.size() * sizeof(wchar_t)));
        }
    }
    void end_debug_label() override {
        if (commandList_)
            commandList_->EndEvent();
    }
    void bind_pipeline(std::string_view name) override {
        for (auto &[id, resource] : resources_) {
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
                                 handle.kind == ResourceKind::Pipeline
                             ? &it->second
                             : nullptr;
        if (!boundPipeline_) {
            lastError_ = "D3D12 pipeline handle is invalid";
            return;
        }
        if (!boundPipeline_->pipeline.computeShader) {
            const auto target = resources_.find(currentRenderTarget_);
            const auto expectedColorFormat =
                format_of(boundPipeline_->pipeline.colorFormats.empty()
                              ? std::string_view(boundPipeline_->pipeline.colorFormat)
                              : std::string_view(boundPipeline_->pipeline.colorFormats.front()));
            const auto expectedDepthFormat = format_of(boundPipeline_->pipeline.depthFormat);
            const auto expectedSamples = boundPipeline_->pipeline.sampleCount;
            if (target != resources_.end()) {
                const auto actualSamples = target->second.nativeDescription.SampleDesc.Count;
                if (actualSamples != expectedSamples) {
                    lastError_ =
                        "D3D12 pipeline sample count does not match the active render target";
                    boundPipeline_ = nullptr;
                    return;
                }
                if (target->second.kind == ResourceKind::DepthStencil) {
                    if (expectedDepthFormat != target->second.format) {
                        lastError_ =
                            "D3D12 pipeline depth format does not match the active depth target";
                        boundPipeline_ = nullptr;
                        return;
                    }
                } else if (target->second.kind != ResourceKind::Texture2D ||
                           target->second.format != expectedColorFormat) {
                    lastError_ =
                        "D3D12 pipeline color format does not match the active render target";
                    boundPipeline_ = nullptr;
                    return;
                }
            } else if (swapChain_ &&
                       (expectedSamples != 1 || swapchainFormat_ != expectedColorFormat)) {
                lastError_ = "D3D12 pipeline color format does not match the swapchain";
                boundPipeline_ = nullptr;
                return;
            }
            if (currentDepthTarget_ != 0) {
                const auto depthTarget = resources_.find(currentDepthTarget_);
                if (depthTarget == resources_.end() ||
                    depthTarget->second.kind != ResourceKind::DepthStencil ||
                    depthTarget->second.format != expectedDepthFormat ||
                    depthTarget->second.nativeDescription.SampleDesc.Count != expectedSamples) {
                    lastError_ = "D3D12 pipeline depth format or sample count does not match the "
                                 "active depth target";
                    boundPipeline_ = nullptr;
                    return;
                }
            }
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_) && commandList_) {
            if (boundPipeline_->pipeline.computeShader) {
                commandList_->SetComputeRootSignature(boundPipeline_->rootSignature);
                commandList_->SetPipelineState(boundPipeline_->pipelineState);
            } else {
                commandList_->SetGraphicsRootSignature(boundPipeline_->rootSignature);
                const auto target = resources_.find(currentRenderTarget_);
                const bool depthOnly =
                    target != resources_.end() && target->second.kind == ResourceKind::DepthStencil;
                const auto constants = boundPipeline_->pipeline.blendConstants;
                const float blendConstants[]{constants.x, constants.y, constants.z, constants.w};
                commandList_->OMSetBlendFactor(blendConstants);
                commandList_->OMSetStencilRef(
                    resolve_depth_stencil(boundPipeline_->pipeline).stencilReference);
                commandList_->SetPipelineState(depthOnly && boundPipeline_->depthOnlyPipeline
                                                   ? boundPipeline_->depthOnlyPipeline
                                                   : boundPipeline_->pipelineState);
                commandList_->IASetPrimitiveTopology(topology(boundPipeline_->pipeline.topology));
            }
            const auto table = bindlessTables_.find(boundBindlessTable_);
            if (table != bindlessTables_.end()) {
                bind_bindless_table(
                    {boundBindlessTable_, table->second.baseIndex, table->second.capacity});
            }
        } else if (boundPipeline_) {
            if (lastError_.empty())
                lastError_ = "D3D12 pipeline binding failed";
            boundPipeline_ = nullptr;
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
        if (!commandList_ || !srvHeap_ || !boundPipeline_ || !boundPipeline_->rootSignature ||
            !boundPipeline_->pipelineState) {
            if (lastError_.empty())
                lastError_ = "D3D12 material binding has invalid command or pipeline state";
            return;
        }
        if (material->second.material.bindless) {
            if (boundBindlessTable_ != 0) {
                const auto table = bindlessTables_.find(boundBindlessTable_);
                if (table != bindlessTables_.end()) {
                    bind_bindless_table(
                        {boundBindlessTable_, table->second.baseIndex, table->second.capacity});
                }
            }
            return;
        }
        for (const auto &binding : material->second.material.bindings) {
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId :
                 {boundPipeline_->pipeline.vertexShader, boundPipeline_->pipeline.fragmentShader,
                  boundPipeline_->pipeline.computeShader}) {
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
                lastError_ = "D3D12 material '" + material->second.material.name + "' binding '" +
                             binding.name +
                             "' does not match shader slot=" + std::to_string(binding.slot) +
                             " space=" + std::to_string(binding.space) +
                             " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        ID3D12DescriptorHeap *heaps[] = {srvHeap_, samplerHeap_};
        commandList_->SetDescriptorHeaps(samplerHeap_ ? 2 : 1, heaps);
        if (!material->second.materialRangesValid &&
            !build_material_descriptor_ranges(material->second, *boundPipeline_))
            return;
        stats_.descriptorBinds += material->second.material.bindings.size();
        for (const auto &binding : material->second.material.bindings) {
            if (binding.slot < 16) {
                const auto rangeType = descriptor_range_type(binding.type);
                const auto parameter = boundPipeline_->rootParameters.find(
                    d3d12_binding_key(binding.space, binding.slot, rangeType));
                const auto setTable = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
                    if (parameter == boundPipeline_->rootParameters.end())
                        return;
                    if (boundPipeline_->pipeline.computeShader)
                        commandList_->SetComputeRootDescriptorTable(parameter->second, gpu);
                    else
                        commandList_->SetGraphicsRootDescriptorTable(parameter->second, gpu);
                };
                const auto range = material->second.materialRanges.find(
                    d3d12_binding_key(binding.space, binding.slot, rangeType));
                if (range != material->second.materialRanges.end())
                    setTable(material_range_gpu(range->second));
            }
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc &description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() ||
            material->second.kind != ResourceKind::Material) {
            lastError_ = "D3D12 material handle not found";
            return false;
        }
        if (material->second.material.pipeline.id != description.pipeline.id) {
            lastError_ = "D3D12 material pipeline cannot be changed after creation";
            return false;
        }
        defer_material_ranges(material->second);
        material->second.material = description;
        if (boundMaterial_ == &material->second)
            boundMaterial_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot,
                             std::uint32_t space) override {
        if (!commandList_ || !srvHeap_ || slot >= 16) {
            lastError_ = "D3D12 uniform buffer binding has invalid command state or slot";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() ||
            resource->second.kind != ResourceKind::Buffer || !resource->second.cbvValid) {
            lastError_ = "D3D12 uniform buffer handle is invalid";
            return;
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_)) {
            const auto parameter = boundPipeline_->rootParameters.find(
                d3d12_binding_key(space, slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV));
            if (parameter == boundPipeline_->rootParameters.end()) {
                lastError_ = "D3D12 uniform buffer binding is absent from the pipeline layout";
                return;
            }
            if (boundPipeline_->pipeline.computeShader)
                commandList_->SetComputeRootDescriptorTable(parameter->second,
                                                            resource->second.cbvGpu);
            else
                commandList_->SetGraphicsRootDescriptorTable(parameter->second,
                                                             resource->second.cbvGpu);
        }
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
                lastError_ = "missing D3D12 vertex buffer";
                return false;
            }
            D3D12_VERTEX_BUFFER_VIEW view{
                found->second.resource->GetGPUVirtualAddress() + binding.offset,
                static_cast<UINT>(found->second.size - binding.offset),
                definition == layout.bindings.end() ? found->second.stride : definition->stride};
            commandList_->IASetVertexBuffers(binding.binding, 1, &view);
        }
        return true;
    }
    void draw_sprite(const SpriteDraw &) override {
        ++stats_.drawCalls;
        ++stats_.spriteCalls;
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState) {
            if (lastError_.empty())
                lastError_ = "D3D12 sprite draw has invalid command state";
            return;
        }
        commandList_->DrawInstanced(6, 1, 0, 0);
    }
    void draw_mesh(const MeshDraw &draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState ||
            !draw.indexCount) {
            if (lastError_.empty())
                lastError_ = "D3D12 mesh draw has invalid command state";
            return;
        }
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        if (vertex == resources_.end() || index == resources_.end() ||
            draw.vertexBuffer.kind != ResourceKind::Buffer ||
            draw.indexBuffer.kind != ResourceKind::Buffer ||
            vertex->second.kind != ResourceKind::Buffer ||
            index->second.kind != ResourceKind::Buffer || !vertex->second.resource ||
            !index->second.resource ||
            !indexed_range_fits(index->second.size, draw.firstIndex, draw.indexCount)) {
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
        if (!bind_mesh_vertices(draw.vertexBuffer, draw.vertexBindings))
            return;
        (void)vertexView;
        commandList_->IASetIndexBuffer(&indexView);
        if (!draw.instanceCount) {
            lastError_ = "instance count must be non-zero";
            return;
        }
        commandList_->DrawIndexedInstanced(draw.indexCount, draw.instanceCount, draw.firstIndex,
                                           draw.vertexOffset, draw.firstInstance);
    }
    bool dispatch(const DispatchDesc &dispatch) override {
        if (!commandList_ || !frameActive_ || dispatch.groupCountX == 0 ||
            dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) {
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
    void draw_mesh_indirect(const IndirectMeshDraw &draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (!commandList_ || !frameActive_ || !boundPipeline_ || !boundPipeline_->pipelineState ||
            !indexedDrawSignature_ || vertex == resources_.end() || index == resources_.end() ||
            arguments == resources_.end() || !vertex->second.resource || !index->second.resource ||
            !arguments->second.resource || draw.vertexBuffer.kind != ResourceKind::Buffer ||
            draw.indexBuffer.kind != ResourceKind::Buffer ||
            draw.argumentBuffer.kind != ResourceKind::Buffer ||
            vertex->second.kind != ResourceKind::Buffer ||
            index->second.kind != ResourceKind::Buffer ||
            arguments->second.kind != ResourceKind::Buffer ||
            !indirect_arguments_fit(arguments->second.size, draw)) {
            if (lastError_.empty())
                lastError_ = "D3D12 indirect draw has invalid command state or buffer";
            return;
        }
        D3D12_VERTEX_BUFFER_VIEW vertexView{vertex->second.resource->GetGPUVirtualAddress(),
                                            static_cast<UINT>(vertex->second.size),
                                            vertex->second.stride};
        D3D12_INDEX_BUFFER_VIEW indexView{index->second.resource->GetGPUVirtualAddress(),
                                          static_cast<UINT>(index->second.size),
                                          DXGI_FORMAT_R32_UINT};
        if (!bind_mesh_vertices(draw.vertexBuffer, draw.vertexBindings))
            return;
        (void)vertexView;
        commandList_->IASetIndexBuffer(&indexView);
        commandList_->ExecuteIndirect(indexedDrawSignature_, draw.maxDrawCount,
                                      arguments->second.resource, draw.argumentOffset, nullptr, 0);
        ++stats_.indirectDrawCalls;
    }
    void end_frame() override {
        if (!commandList_ || !frameActive_)
            return;
        if (timestampQueryHeap_ && timestampReadback_ && timestampCursor_ > 0) {
            commandList_->ResolveQueryData(timestampQueryHeap_, D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                           timestampCursor_, timestampReadback_, 0);
            submittedTimestampCount_ = timestampCursor_;
        }
        if (!swapChain_) {
            const auto closeResult = commandList_->Close();
            if (FAILED(closeResult)) {
                lastError_ = "D3D12 off-screen command list close failed hr=" +
                             std::to_string(static_cast<long long>(closeResult));
                capabilities_.deviceState = RenderDeviceState::Lost;
                frameActive_ = false;
                return;
            }
            ID3D12CommandList *lists[] = {commandList_};
            queue_->ExecuteCommandLists(1, lists);
            ++stats_.queueSubmissions;
            ++fenceValue_;
            const auto signalResult = queue_->Signal(fence_, fenceValue_);
            if (SUCCEEDED(signalResult))
                lastSubmittedFrame_ = fenceValue_;
            if (FAILED(signalResult)) {
                lastError_ = "D3D12 off-screen fence signal failed hr=" +
                             std::to_string(static_cast<long long>(signalResult));
                capabilities_.deviceState = RenderDeviceState::Lost;
            }
            frameActive_ = false;
            return;
        }
        if (frameIndex_ >= backBuffers_.size() || !backBuffers_[frameIndex_] || !rtvHeap_) {
            lastError_ = "D3D12 swapchain backbuffer is unavailable during present";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
            frameActive_ = false;
            return;
        }
        auto *buffer = backBuffers_[frameIndex_];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);
        const auto closeResult = commandList_->Close();
        if (FAILED(closeResult)) {
            lastError_ = "D3D12 command list close failed hr=" +
                         std::to_string(static_cast<long long>(closeResult));
            capabilities_.deviceState = RenderDeviceState::Lost;
            frameActive_ = false;
            return;
        }
        ID3D12CommandList *lists[] = {commandList_};
        queue_->ExecuteCommandLists(1, lists);
        ++stats_.queueSubmissions;
        capture_ui_surface(buffer, 0);
        const auto presentResult = swapChain_->Present(vsync_ ? 1 : 0, 0);
        if (FAILED(presentResult)) {
            lastError_ =
                "D3D12 present failed hr=" + std::to_string(static_cast<long long>(presentResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState =
                reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
        }
#if defined(SHINKOU_WITH_IMGUI)
        if (imguiPlatformPending_ &&
            (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
            // Upstream creates a queue per viewport. Bracket those reads with
            // GPU fence waits in both directions; all world work precedes UI,
            // and the final engine fence also covers secondary-window reads.
            imguiWorldReadyFence_ = ++fenceValue_;
            queue_->Signal(fence_, imguiWorldReadyFence_);
            ImGui::RenderPlatformWindowsDefault(nullptr, this);
        }
#endif
        imguiPlatformPending_ = false;
        ++fenceValue_;
        const auto signalResult = queue_->Signal(fence_, fenceValue_);
        if (SUCCEEDED(signalResult))
            lastSubmittedFrame_ = fenceValue_;
        if (FAILED(signalResult)) {
            lastError_ = "D3D12 frame fence signal failed hr=" +
                         std::to_string(static_cast<long long>(signalResult));
            const auto reason = device_->GetDeviceRemovedReason();
            capabilities_.deviceState =
                reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET
                    ? RenderDeviceState::Lost
                    : RenderDeviceState::NeedsResize;
        }
        frameActive_ = false;
    }
    void discard_frame() override {
        if (!frameActive_ || !commandList_ || !commandAllocator_)
            return;
        ++stats_.discardedFrames;
        commandList_->Close();
        commandAllocator_->Reset();
        frameActive_ = false;
        boundPipeline_ = nullptr;
        boundMaterial_ = nullptr;
    }
    void wait_idle() override {
        if (!queue_ || !fence_ || !fenceEvent_ || fenceValue_ == 0)
            return;
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }
    RenderStats stats() const noexcept override {
        return stats_;
    }
};
} // namespace
std::unique_ptr<IRenderBackend> create_directx12_backend() {
    return std::make_unique<DirectX12Backend>();
}
} // namespace shinkou::render
#endif
