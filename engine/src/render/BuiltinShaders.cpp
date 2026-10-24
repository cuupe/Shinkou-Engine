#include "shinkou/render/BuiltinShaders.h"
#include "BuiltinShaderSources.h"
#include <fstream>
namespace shinkou::render {
std::string_view pbr_shader_library() noexcept { return detail::pbrSource; }
namespace {
ShaderDesc pbr_shader(BackendApi api,ShaderStage stage,const char* variant) {
    ShaderDesc shader;
    shader.stage=stage;
    shader.name=std::string("builtin_")+variant;
    shader.profile=stage==ShaderStage::Vertex?"vs_5_0":"ps_5_0";
    if (api==BackendApi::Vulkan) {
        std::ifstream stream(std::string(SHINKOU_BUILTIN_SHADER_OUTPUT_DIR)+"/"+variant+".spv",std::ios::binary);
        shader.bytecode.assign(std::istreambuf_iterator<char>(stream),{});
        shader.sourceKind=ShaderSourceKind::SpirV;
    } else shader.source=std::string("#define SHINKOU_")+variant+" 1\n"+std::string(detail::pbrSource)+std::string(detail::forwardSource);
    return shader;
}
}
ShaderDesc builtin_pbr_shader(BackendApi api,ShaderStage stage) {
    return pbr_shader(api,stage,stage==ShaderStage::Vertex?"PBR_VERTEX":"PBR_FRAGMENT");
}
ShaderDesc builtin_shadow_fragment_shader(BackendApi api) {
    return pbr_shader(api,ShaderStage::Fragment,"SHADOW_FRAGMENT");
}
}
