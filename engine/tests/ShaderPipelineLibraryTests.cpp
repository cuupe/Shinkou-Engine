#include "shinkou/render/ShaderPipelineLibrary.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace shinkou::render;

namespace {
void write_text(const std::filesystem::path& path, const char* text) {
    std::ofstream file(path, std::ios::trunc);
    assert(file);
    file << text;
}

ShaderPipelineCompileRequest request_for(const std::filesystem::path& root) {
    ShaderPipelineCompileRequest request;
    request.name = "industrial_shader";
    request.shader.stage = ShaderStage::Fragment;
    request.shader.sourceKind = ShaderSourceKind::Source;
    request.sourcePath = root;
    request.includeDirectories = {root.parent_path()};
    return request;
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "shinkou_shader_pipeline_library_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    const auto root = directory / "root.hlsl";
    const auto include = directory / "common.hlsl";
    write_text(include, "#define INDUSTRIAL_TEST 1\n");
    write_text(root, "#include \"common.hlsl\"\nTexture2D albedo : register(t0);\nfloat4 main() : SV_Target { return 1; }\n");

    const auto request = request_for(root);
    const auto manifest = scan_shader_dependencies(request);
    assert(manifest.valid && manifest.files.size() == 2 && manifest.contentHash != 0);
    const auto keyA = make_shader_pipeline_variant_key(request, BackendApi::Null, manifest);
    auto changedRequest = request;
    changedRequest.shader.defines = {"QUALITY=HIGH"};
    const auto keyB = make_shader_pipeline_variant_key(changedRequest, BackendApi::Null, manifest);
    assert(keyA.value != keyB.value);

    ShaderPipelineLibrary library;
    const auto first = library.compile(request, BackendApi::Null);
    assert(first.success && first.compiled && first.artifact.compiled.valid);
    assert(first.artifact.compiled.bindings.size() == 1);
    ShaderPipelineSnapshot snapshot;
    assert(library.snapshot(request.name, BackendApi::Null, snapshot));
    assert(snapshot.hasCurrentVersion && snapshot.current.generation == 1);

    write_text(include, "#define INDUSTRIAL_TEST 2\n");
    const auto changes = library.detect_changes();
    assert(changes.size() == 1 && !changes.front().files.empty());
    const auto reloaded = library.reload_if_changed(request.name, BackendApi::Null);
    assert(reloaded.success && reloaded.compiled && reloaded.artifact.generation == 2);

    write_text(root, "#include \"missing.hlsl\"\nTexture2D albedo : register(t0);\n");
    const auto failed = library.reload_if_changed(request.name, BackendApi::Null);
    assert(!failed.success && failed.usedPreviousVersion && failed.artifact.generation == 2);
    assert(library.snapshot(request.name, BackendApi::Null, snapshot));
    assert(snapshot.hasCurrentVersion && snapshot.current.generation == 2 && !snapshot.lastError.empty());

    write_text(root, "#include \"common.hlsl\"\nTexture2D albedo : register(t0);\nfloat4 main() : SV_Target { return 1; }\n");
    const auto recovered = library.reload_if_changed(request.name, BackendApi::Null);
    assert(recovered.success && recovered.artifact.generation == 3);

    auto dxbcRequest = request;
    dxbcRequest.name = "cross_backend";
    dxbcRequest.shader.source.clear();
    dxbcRequest.shader.sourceKind = ShaderSourceKind::Dxbc;
    dxbcRequest.shader.bytecode = {1, 2, 3, 4};
    auto dxilRequest = dxbcRequest;
    dxilRequest.shader.sourceKind = ShaderSourceKind::Dxil;
    dxilRequest.shader.bytecode = {5, 6, 7, 8};
    auto spirvRequest = dxbcRequest;
    spirvRequest.shader.sourceKind = ShaderSourceKind::SpirV;
    spirvRequest.shader.bytecode.resize(5 * sizeof(std::uint32_t));
    const std::uint32_t spirvMagic = 0x07230203u;
    std::memcpy(spirvRequest.shader.bytecode.data(), &spirvMagic, sizeof(spirvMagic));
    assert(library.compile(dxbcRequest, BackendApi::DirectX11).success);
    assert(library.compile(dxilRequest, BackendApi::DirectX12).success);
    assert(library.compile(spirvRequest, BackendApi::Vulkan).success);
    const auto metadata = library.cache_metadata("cross_backend");
    assert(metadata.size() == 3);
    assert(metadata[0].format == ShaderFormat::Dxbc && metadata[1].format == ShaderFormat::Dxil &&
        metadata[2].format == ShaderFormat::SpirV);

    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            const auto result = library.compile(request, BackendApi::Null);
            assert(result.success);
        });
    }
    for (auto& worker : workers) worker.join();
    assert(library.size() == 4);

    std::filesystem::remove_all(directory, error);
    return 0;
}
