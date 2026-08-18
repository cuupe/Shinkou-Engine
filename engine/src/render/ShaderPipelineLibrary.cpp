#include "shinkou/render/ShaderPipelineLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace shinkou::render {
namespace {

constexpr std::uint64_t FnvOffset = 1469598103934665603ull;
constexpr std::uint64_t FnvPrime = 1099511628211ull;

std::uint64_t hash_bytes(std::uint64_t seed, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        seed ^= bytes[index];
        seed *= FnvPrime;
    }
    return seed;
}

std::uint64_t hash_string(std::uint64_t seed, std::string_view value) noexcept {
    seed = hash_bytes(seed, value.data(), value.size());
    const char separator = '\0';
    return hash_bytes(seed, &separator, sizeof(separator));
}

std::uint64_t hash_file_bytes(const std::vector<std::uint8_t>& bytes) noexcept {
    return hash_bytes(FnvOffset, bytes.data(), bytes.size());
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return path.lexically_normal();
    const auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return (error ? absolute : canonical).lexically_normal();
}

std::uint64_t file_timestamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    return static_cast<std::uint64_t>(time.time_since_epoch().count());
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto end = file.tellg();
    if (end < 0) return false;
    bytes.resize(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    return bytes.empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())));
}

bool parse_include(std::string_view line, std::string& includeName, bool& angled) {
    const auto hash = line.find('#');
    if (hash == std::string_view::npos) return false;
    line.remove_prefix(hash + 1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) line.remove_prefix(1);
    constexpr std::string_view keyword = "include";
    if (line.substr(0, keyword.size()) != keyword) return false;
    line.remove_prefix(keyword.size());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) line.remove_prefix(1);
    if (line.empty() || (line.front() != '"' && line.front() != '<')) return false;
    angled = line.front() == '<';
    const char closing = angled ? '>' : '"';
    line.remove_prefix(1);
    const auto end = line.find(closing);
    if (end == std::string_view::npos || end == 0) return false;
    includeName.assign(line.substr(0, end));
    return true;
}

struct DependencyScanner {
    const ShaderPipelineCompileRequest& request;
    ShaderDependencyManifest result;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;

    void diagnostic(const std::string& message) {
        if (!result.diagnostics.empty()) result.diagnostics += "\n";
        result.diagnostics += message;
    }

    std::filesystem::path resolve_include(const std::filesystem::path& including,
                                          std::string_view includeName, bool angled) {
        std::vector<std::filesystem::path> candidates;
        if (!angled) candidates.push_back(including.parent_path() / std::filesystem::path(includeName));
        for (const auto& directory : request.includeDirectories) {
            candidates.push_back(directory / std::filesystem::path(includeName));
        }
        if (angled) candidates.push_back(including.parent_path() / std::filesystem::path(includeName));
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) return normalized_path(candidate);
        }
        return candidates.empty() ? std::filesystem::path(includeName) : normalized_path(candidates.front());
    }

    bool visit(const std::filesystem::path& path, std::string_view sourceOverride,
               std::size_t depth) {
        if (depth > request.maxIncludeDepth) {
            diagnostic("shader include depth exceeded at " + path.string());
            return false;
        }
        const auto normalized = normalized_path(path);
        const auto key = normalized.generic_string();
        if (visiting.find(key) != visiting.end()) {
            diagnostic("shader include cycle detected at " + normalized.string());
            return false;
        }
        if (visited.find(key) != visited.end()) return true;

        std::vector<std::uint8_t> bytes;
        std::string source;
        if (!sourceOverride.empty()) {
            source.assign(sourceOverride.begin(), sourceOverride.end());
            bytes.assign(source.begin(), source.end());
        } else if (!read_file(normalized, bytes)) {
            diagnostic("shader dependency cannot be read: " + normalized.string());
            result.files.push_back({normalized, 0, 0, 0, false});
            return false;
        } else {
            source.assign(bytes.begin(), bytes.end());
        }

        std::error_code sizeError;
        const auto size = std::filesystem::file_size(normalized, sizeError);
        result.files.push_back({normalized, file_timestamp(normalized), sizeError ? bytes.size() : size,
            hash_file_bytes(bytes), true});
        visiting.insert(key);
        std::istringstream lines(source);
        std::string line;
        bool valid = true;
        while (std::getline(lines, line)) {
            std::string includeName;
            bool angled = false;
            if (!parse_include(line, includeName, angled)) continue;
            const auto dependency = resolve_include(normalized, includeName, angled);
            if (!visit(dependency, {}, depth + 1)) valid = false;
        }
        visiting.erase(key);
        visited.insert(key);
        return valid;
    }

    ShaderDependencyManifest run() {
        if (request.sourcePath.empty()) {
            result.sourcePath.clear();
            const auto sourceBytes = std::vector<std::uint8_t>(request.shader.source.begin(), request.shader.source.end());
            result.contentHash = hash_file_bytes(sourceBytes);
            result.valid = true;
            return result;
        }
        result.sourcePath = normalized_path(request.sourcePath);
        std::string sourceOverride;
        if (!request.shader.source.empty()) sourceOverride = request.shader.source;
        const bool valid = visit(request.sourcePath, sourceOverride, 0);
        std::sort(result.files.begin(), result.files.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.path.generic_string() < rhs.path.generic_string();
        });
        result.contentHash = FnvOffset;
        for (const auto& file : result.files) {
            result.contentHash = hash_string(result.contentHash, file.path.generic_string());
            result.contentHash = hash_bytes(result.contentHash, &file.contentHash, sizeof(file.contentHash));
        }
        result.valid = valid && result.diagnostics.empty();
        return result;
    }
};

std::uint64_t artifact_hash(const CompiledShader& shader) noexcept {
    auto hash = hash_bytes(FnvOffset, shader.bytecode.data(), shader.bytecode.size());
    std::vector<ShaderBinding> bindings = shader.bindings;
    std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.space != rhs.space) return lhs.space < rhs.space;
        if (lhs.slot != rhs.slot) return lhs.slot < rhs.slot;
        return lhs.name < rhs.name;
    });
    for (const auto& binding : bindings) {
        hash = hash_string(hash, binding.name);
        hash = hash_string(hash, binding.type);
        hash = hash_bytes(hash, &binding.slot, sizeof(binding.slot));
        hash = hash_bytes(hash, &binding.space, sizeof(binding.space));
        hash = hash_bytes(hash, &binding.count, sizeof(binding.count));
        hash = hash_bytes(hash, &binding.unbounded, sizeof(binding.unbounded));
    }
    return hash;
}

} // namespace

ShaderDependencyManifest scan_shader_dependencies(const ShaderPipelineCompileRequest& request) {
    return DependencyScanner{request, {}, {}, {}}.run();
}

ShaderPipelineVariantKey make_shader_pipeline_variant_key(
    const ShaderPipelineCompileRequest& request, BackendApi backend,
    const ShaderDependencyManifest& dependencies) {
    ShaderPipelineVariantKey key;
    key.backend = backend;
    key.compilerHash = shader_variant_hash(request.shader, backend);
    key.dependencyHash = dependencies.contentHash;
    key.value = hash_bytes(FnvOffset, &key.compilerHash, sizeof(key.compilerHash));
    key.value = hash_bytes(key.value, &key.dependencyHash, sizeof(key.dependencyHash));
    key.value = hash_string(key.value, request.name);
    return key;
}

std::size_t ShaderPipelineLibrary::RecordKeyHash::operator()(const RecordKey& key) const noexcept {
    auto hash = std::hash<std::string>{}(key.name);
    hash ^= static_cast<std::size_t>(key.backend) + static_cast<std::size_t>(0x9e3779b9u) +
        (hash << 6u) + (hash >> 2u);
    return hash;
}

ShaderPipelineLibrary::ShaderPipelineLibrary(std::unique_ptr<IShaderCompiler> compiler)
    : compiler_(std::move(compiler)) {}

ShaderPipelineBuildResult ShaderPipelineLibrary::compile_locked(const RecordKey& key,
    const ShaderPipelineCompileRequest& request, const ShaderDependencyManifest& dependencies) {
    auto& record = records_[key];
    record.request = request;
    auto compilerRequest = request;
    if (dependencies.valid && compilerRequest.shader.source.empty() && !request.sourcePath.empty()) {
        std::vector<std::uint8_t> bytes;
        if (read_file(request.sourcePath, bytes)) {
            compilerRequest.shader.source.assign(bytes.begin(), bytes.end());
        }
    }
    if (!request.sourcePath.empty()) compilerRequest.shader.name = request.sourcePath.string();
    const auto variant = make_shader_pipeline_variant_key(
        dependencies.valid ? compilerRequest : request, key.backend, dependencies);
    record.lastAttemptedKey = variant;
    const bool changed = !record.hasCurrentVersion || !(record.current.key == variant) ||
        record.current.dependencies.contentHash != dependencies.contentHash;
    if (record.hasCurrentVersion && !changed && record.lastError.empty()) return unchanged_result_locked(record);

    ShaderPipelineBuildResult result;
    result.changed = changed;
    result.diagnostics = dependencies.diagnostics;
    if (!dependencies.valid) {
        result.usedPreviousVersion = record.hasCurrentVersion;
        if (record.hasCurrentVersion) result.artifact = record.current;
        record.lastError = dependencies.diagnostics;
        if (!record.hasCurrentVersion) record.watchedDependencies = dependencies;
        return result;
    }

    if (compilerRequest.shader.source.empty() && !request.sourcePath.empty()) {
        std::vector<std::uint8_t> bytes;
        if (!read_file(request.sourcePath, bytes)) {
            result.diagnostics = "shader source cannot be read: " + request.sourcePath.string();
            record.lastError = result.diagnostics;
            result.usedPreviousVersion = record.hasCurrentVersion;
            if (record.hasCurrentVersion) result.artifact = record.current;
            return result;
        }
        compilerRequest.shader.source.assign(bytes.begin(), bytes.end());
    }
    if (!request.sourcePath.empty()) compilerRequest.shader.name = request.sourcePath.string();
    auto compiled = compiler_->compile(compilerRequest.shader, key.backend);
    if (!compiled.valid) {
        result.diagnostics = compiled.diagnostics.empty() ? "shader compilation failed" : compiled.diagnostics;
        record.lastError = result.diagnostics;
        result.usedPreviousVersion = record.hasCurrentVersion;
        if (record.hasCurrentVersion) result.artifact = record.current;
        return result;
    }

    ShaderPipelineArtifact artifact;
    artifact.name = request.name;
    artifact.backend = key.backend;
    artifact.key = variant;
    artifact.compiled = std::move(compiled);
    artifact.dependencies = dependencies;
    artifact.cache = {key.backend, artifact.compiled.format, artifact.compiled.variantHash,
        variant.compilerHash, variant.dependencyHash, artifact_hash(artifact.compiled),
        artifact.compiled.bytecode.size(), artifact.compiled.cacheHit};
    artifact.generation = record.nextGeneration++;
    record.current = artifact;
    record.hasCurrentVersion = true;
    record.watchedDependencies = dependencies;
    record.lastError.clear();
    result.success = true;
    result.compiled = true;
    result.artifact = std::move(artifact);
    return result;
}

ShaderPipelineBuildResult ShaderPipelineLibrary::unchanged_result_locked(const Record& record) const {
    ShaderPipelineBuildResult result;
    result.success = record.hasCurrentVersion;
    result.usedPreviousVersion = record.hasCurrentVersion;
    if (record.hasCurrentVersion) result.artifact = record.current;
    result.diagnostics = record.lastError;
    return result;
}

ShaderPipelineBuildResult ShaderPipelineLibrary::compile(const ShaderPipelineCompileRequest& request,
                                                         BackendApi backend) {
    ShaderPipelineBuildResult result;
    if (request.name.empty()) {
        result.diagnostics = "shader pipeline name is empty";
        return result;
    }
    const auto dependencies = scan_shader_dependencies(request);
    const RecordKey key{request.name, backend};
    std::lock_guard<std::mutex> lock(mutex_);
    return compile_locked(key, request, dependencies);
}

ShaderPipelineBuildResult ShaderPipelineLibrary::reload_if_changed(std::string_view name, BackendApi backend) {
    const RecordKey key{std::string(name), backend};
    ShaderPipelineCompileRequest request;
    ShaderDependencyManifest watched;
    bool retryFailed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(key);
        if (found == records_.end()) return {};
        request = found->second.request;
        watched = found->second.watchedDependencies;
        retryFailed = !found->second.lastError.empty();
    }
    const auto current = scan_shader_dependencies(request);
    if (!retryFailed && current.valid && watched.valid && current.contentHash == watched.contentHash) {
        std::lock_guard<std::mutex> lock(mutex_);
        return unchanged_result_locked(records_.at(key));
    }
    return compile(request, backend);
}

std::vector<ShaderPipelineChange> ShaderPipelineLibrary::detect_changes() const {
    struct WatchedRecord { RecordKey key; ShaderPipelineCompileRequest request; ShaderDependencyManifest manifest; };
    std::vector<WatchedRecord> watched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        watched.reserve(records_.size());
        for (const auto& entry : records_) watched.push_back({entry.first, entry.second.request, entry.second.watchedDependencies});
    }
    std::vector<ShaderPipelineChange> changes;
    for (const auto& entry : watched) {
        const auto current = scan_shader_dependencies(entry.request);
        if (current.valid && entry.manifest.valid && current.contentHash == entry.manifest.contentHash) continue;
        ShaderPipelineChange change{entry.key.name, entry.key.backend, {}};
        for (const auto& file : current.files) {
            const auto old = std::find_if(entry.manifest.files.begin(), entry.manifest.files.end(),
                [&](const auto& candidate) { return candidate.path == file.path; });
            if (old == entry.manifest.files.end() || old->contentHash != file.contentHash ||
                old->timestamp != file.timestamp || old->size != file.size) change.files.push_back(file.path);
        }
        if (change.files.empty() && !current.diagnostics.empty() && !entry.manifest.sourcePath.empty()) {
            change.files.push_back(entry.manifest.sourcePath);
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

bool ShaderPipelineLibrary::snapshot(std::string_view name, BackendApi backend,
                                     ShaderPipelineSnapshot& output) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find({std::string(name), backend});
    if (found == records_.end()) return false;
    const auto& record = found->second;
    output = {};
    output.hasCurrentVersion = record.hasCurrentVersion;
    output.current = record.current;
    output.lastAttemptedKey = record.lastAttemptedKey;
    output.lastError = record.lastError;
    for (const auto& entry : records_) {
        if (entry.first.name == name && entry.second.hasCurrentVersion) output.backendArtifacts.push_back(entry.second.current.cache);
    }
    std::sort(output.backendArtifacts.begin(), output.backendArtifacts.end(), [](const auto& lhs, const auto& rhs) {
        return static_cast<int>(lhs.backend) < static_cast<int>(rhs.backend);
    });
    return true;
}

std::vector<ShaderArtifactCacheMetadata> ShaderPipelineLibrary::cache_metadata(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShaderArtifactCacheMetadata> result;
    for (const auto& entry : records_) {
        if (entry.first.name == name && entry.second.hasCurrentVersion) result.push_back(entry.second.current.cache);
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return static_cast<int>(lhs.backend) < static_cast<int>(rhs.backend);
    });
    return result;
}

void ShaderPipelineLibrary::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
}

std::size_t ShaderPipelineLibrary::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

} // namespace shinkou::render
