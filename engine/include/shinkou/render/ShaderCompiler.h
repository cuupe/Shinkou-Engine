#pragma once

#include "shinkou/render/RenderTypes.h"
#include <cstdint>
#include <memory>
#include <string>
#include <future>
#include <vector>

namespace shinkou::render {
enum class ShaderFormat { Source, Dxbc, Dxil, SpirV };

struct ShaderBinding {
    std::string name;
    std::uint32_t slot{0};
    std::uint32_t space{0};
    std::string type;
    std::uint32_t count{1};
    bool unbounded{false};
};

struct CompiledShader {
    ShaderStage stage{ShaderStage::Vertex};
    ShaderFormat format{ShaderFormat::Source};
    std::vector<std::uint8_t> bytecode;
    std::vector<ShaderBinding> bindings;
    std::string diagnostics;
    std::uint64_t variantHash{0};
    bool cacheHit{false};
    bool layoutValid{true};
    bool valid{false};
};

std::uint64_t shader_variant_hash(const ShaderDesc& shader, BackendApi backend);

bool merge_shader_bindings(const std::vector<ShaderBinding>& source,
                           std::vector<ShaderBinding>& merged,
                           std::string& diagnostics);

class IShaderCompiler {
public:
    virtual ~IShaderCompiler() = default;
    virtual CompiledShader compile(const ShaderDesc& shader, BackendApi backend) const = 0;
    virtual std::future<CompiledShader> compile_async(const ShaderDesc& shader, BackendApi backend) const {
        return std::async(std::launch::async, [this, shader, backend] { return compile(shader, backend); });
    }
};

std::unique_ptr<IShaderCompiler> create_shader_compiler();
}
