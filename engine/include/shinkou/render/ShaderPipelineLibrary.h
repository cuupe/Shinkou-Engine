#pragma once

#include "shinkou/render/ShaderCompiler.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::render {

struct ShaderPipelineCompileRequest {
    std::string name;
    ShaderDesc shader{};
    std::filesystem::path sourcePath{};
    std::vector<std::filesystem::path> includeDirectories;
    std::size_t maxIncludeDepth{64};
};

struct ShaderDependencyInfo {
    std::filesystem::path path;
    std::uint64_t timestamp{0};
    std::uintmax_t size{0};
    std::uint64_t contentHash{0};
    bool exists{false};
};

struct ShaderDependencyManifest {
    std::filesystem::path sourcePath;
    std::vector<ShaderDependencyInfo> files;
    std::uint64_t contentHash{0};
    bool valid{false};
    std::string diagnostics;
};

ShaderDependencyManifest scan_shader_dependencies(const ShaderPipelineCompileRequest& request);

struct ShaderPipelineVariantKey {
    std::uint64_t value{0};
    std::uint64_t compilerHash{0};
    std::uint64_t dependencyHash{0};
    BackendApi backend{BackendApi::Null};

    friend bool operator==(const ShaderPipelineVariantKey& lhs,
                           const ShaderPipelineVariantKey& rhs) noexcept {
        return lhs.value == rhs.value && lhs.backend == rhs.backend;
    }
};

ShaderPipelineVariantKey make_shader_pipeline_variant_key(
    const ShaderPipelineCompileRequest& request, BackendApi backend,
    const ShaderDependencyManifest& dependencies);

struct ShaderArtifactCacheMetadata {
    BackendApi backend{BackendApi::Null};
    ShaderFormat format{ShaderFormat::Source};
    std::uint64_t variantHash{0};
    std::uint64_t sourceHash{0};
    std::uint64_t dependencyHash{0};
    std::uint64_t artifactHash{0};
    std::size_t bytecodeSize{0};
    bool compilerCacheHit{false};
};

struct ShaderPipelineArtifact {
    std::string name;
    BackendApi backend{BackendApi::Null};
    ShaderPipelineVariantKey key{};
    CompiledShader compiled{};
    ShaderDependencyManifest dependencies{};
    ShaderArtifactCacheMetadata cache{};
    std::uint64_t generation{0};
};

struct ShaderPipelineBuildResult {
    bool success{false};
    bool compiled{false};
    bool usedPreviousVersion{false};
    bool changed{false};
    std::string diagnostics;
    ShaderPipelineArtifact artifact{};
};

struct ShaderPipelineChange {
    std::string name;
    BackendApi backend{BackendApi::Null};
    std::vector<std::filesystem::path> files;
};

struct ShaderPipelineSnapshot {
    bool hasCurrentVersion{false};
    ShaderPipelineArtifact current{};
    ShaderPipelineVariantKey lastAttemptedKey{};
    std::string lastError;
    std::vector<ShaderArtifactCacheMetadata> backendArtifacts;
};

class ShaderPipelineLibrary {
    struct RecordKey {
        std::string name;
        BackendApi backend{BackendApi::Null};

        friend bool operator==(const RecordKey& lhs, const RecordKey& rhs) noexcept {
            return lhs.name == rhs.name && lhs.backend == rhs.backend;
        }
    };

    struct RecordKeyHash {
        std::size_t operator()(const RecordKey& key) const noexcept;
    };

    struct Record {
        ShaderPipelineCompileRequest request{};
        ShaderDependencyManifest watchedDependencies{};
        ShaderPipelineVariantKey lastAttemptedKey{};
        ShaderPipelineArtifact current{};
        std::string lastError;
        bool hasCurrentVersion{false};
        std::uint64_t nextGeneration{1};
    };

    std::unique_ptr<IShaderCompiler> compiler_;
    mutable std::mutex mutex_;
    std::unordered_map<RecordKey, Record, RecordKeyHash> records_;

    ShaderPipelineBuildResult compile_locked(const RecordKey& key,
        const ShaderPipelineCompileRequest& request,
        const ShaderDependencyManifest& dependencies);
    ShaderPipelineBuildResult unchanged_result_locked(const Record& record) const;

public:
    explicit ShaderPipelineLibrary(std::unique_ptr<IShaderCompiler> compiler = create_shader_compiler());
    ~ShaderPipelineLibrary() = default;
    ShaderPipelineLibrary(const ShaderPipelineLibrary&) = delete;
    ShaderPipelineLibrary& operator=(const ShaderPipelineLibrary&) = delete;

    ShaderPipelineBuildResult compile(const ShaderPipelineCompileRequest& request, BackendApi backend);
    ShaderPipelineBuildResult reload_if_changed(std::string_view name, BackendApi backend);
    std::vector<ShaderPipelineChange> detect_changes() const;
    bool snapshot(std::string_view name, BackendApi backend, ShaderPipelineSnapshot& output) const;
    std::vector<ShaderArtifactCacheMetadata> cache_metadata(std::string_view name) const;
    void clear();
    std::size_t size() const noexcept;
};

} // namespace shinkou::render
