#pragma once
#include "shinkou/render/ShaderCompiler.h"
#include <string_view>
namespace shinkou::render {
std::string_view pbr_shader_library() noexcept;
ShaderDesc builtin_fullscreen_vertex_shader(BackendApi api);
ShaderDesc builtin_pbr_shader(BackendApi api, ShaderStage stage);
ShaderDesc builtin_shadow_fragment_shader(BackendApi api);
} // namespace shinkou::render
