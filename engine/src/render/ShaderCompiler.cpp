#include "shinkou/render/ShaderCompiler.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxc/dxcapi.h>
#include <windows.h>
#endif

#include <cstring>
#include <cctype>
#include <algorithm>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <utility>
#include <vector>
#include <sstream>

namespace shinkou::render {
namespace {
constexpr std::uint32_t invalid_binding = std::numeric_limits<std::uint32_t>::max();

bool is_identifier_character(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

std::uint64_t hash_bytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void hash_string(std::uint64_t& hash, std::string_view value) {
    hash = hash_bytes(hash, value.data(), value.size());
    const char separator = '\0';
    hash = hash_bytes(hash, &separator, 1);
}

std::string last_identifier(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && !is_identifier_character(text[end - 1])) --end;
    const auto stop = end;
    while (end > 0 && is_identifier_character(text[end - 1])) --end;
    if (end == stop) return {};
    return std::string(text.substr(end, stop - end));
}

std::string binding_type_from_register(char kind) {
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(kind)))) {
    case 't': return "texture";
    case 's': return "sampler";
    case 'b': return "uniform_buffer";
    case 'u': return "storage_buffer";
    default: return "unknown";
    }
}

std::string declaration_identifier(std::string_view declaration) {
    const auto colon = declaration.find(':');
    if (colon != std::string_view::npos) declaration = declaration.substr(0, colon);
    const auto array = declaration.find_last_of('[');
    if (array != std::string_view::npos) declaration = declaration.substr(0, array);
    return last_identifier(declaration);
}

void append_binding(CompiledShader& result, ShaderBinding binding) {
    const auto duplicate = std::find_if(result.bindings.begin(), result.bindings.end(), [&](const auto& existing) {
        return existing.slot == binding.slot && existing.space == binding.space;
    });
    if (duplicate == result.bindings.end()) {
        result.bindings.push_back(std::move(binding));
        return;
    }
    if (duplicate->type != binding.type || duplicate->count != binding.count || duplicate->unbounded != binding.unbounded) {
        result.layoutValid = false;
        result.diagnostics = "shader descriptor layout has conflicting declarations at space=" +
            std::to_string(binding.space) + " slot=" + std::to_string(binding.slot);
    }
}

void reflect_hlsl_registers(CompiledShader& result, std::string_view source) {
    std::size_t cursor = 0;
    while ((cursor = source.find("register(", cursor)) != std::string_view::npos) {
        const auto begin = cursor + 9;
        if (begin >= source.size()) break;
        const char kind = source[begin];
        if (!std::isalpha(static_cast<unsigned char>(kind))) { cursor = begin; continue; }
        std::size_t number = begin + 1;
        std::uint32_t slot = 0;
        while (number < source.size() && std::isdigit(static_cast<unsigned char>(source[number]))) {
            slot = slot * 10u + static_cast<std::uint32_t>(source[number] - '0');
            ++number;
        }
        const auto close = source.find(')', number);
        std::uint32_t space = 0;
        const auto spacePos = source.find("space", number);
        if (spacePos != std::string_view::npos && close != std::string_view::npos && spacePos < close) {
            std::size_t digit = spacePos + 5;
            while (digit < source.size() && std::isdigit(static_cast<unsigned char>(source[digit]))) {
                space = space * 10u + static_cast<std::uint32_t>(source[digit] - '0');
                ++digit;
            }
        }
        std::size_t statementStart = 0;
        for (const auto delimiter : {';', '{', '}'}) {
            const auto position = source.rfind(delimiter, cursor);
            if (position != std::string_view::npos) statementStart = std::max(statementStart, position + 1);
        }
        const auto declaration = source.substr(statementStart, cursor - statementStart);
        auto type = binding_type_from_register(kind);
        if (declaration.find("SamplerState") != std::string_view::npos ||
            declaration.find("SamplerComparisonState") != std::string_view::npos) type = "sampler";
        if (type == "texture" && declaration.find("StructuredBuffer") != std::string_view::npos) type = "structured_buffer";
        if (type == "storage_buffer" && (declaration.find("Texture") != std::string_view::npos ||
            declaration.find("texture") != std::string_view::npos)) type = "storage_texture";
        ShaderBinding binding{declaration_identifier(declaration), slot, space, std::move(type)};
        const auto open = declaration.find_last_of('[');
        const auto closeBracket = declaration.find(']', open == std::string_view::npos ? 0 : open + 1);
        if (open != std::string_view::npos && closeBracket != std::string_view::npos) {
            const auto length = declaration.substr(open + 1, closeBracket - open - 1);
            if (length.empty()) {
                binding.count = 0;
                binding.unbounded = true;
            } else {
                try { binding.count = static_cast<std::uint32_t>(std::stoul(std::string(length))); }
                catch (...) { binding.count = 0; binding.unbounded = true; }
                if (binding.count == 0) result.layoutValid = false;
            }
        }
        append_binding(result, std::move(binding));
        cursor = number;
    }
}

void reflect_glsl_layouts(CompiledShader& result, std::string_view source) {
    std::size_t cursor = 0;
    while ((cursor = source.find("layout(", cursor)) != std::string_view::npos) {
        const auto close = source.find(')', cursor + 7);
        if (close == std::string_view::npos) break;
        const auto layout = source.substr(cursor + 7, close - cursor - 7);
        const auto parse_layout_value = [&](std::string_view key) {
            const auto keyPosition = layout.find(key);
            if (keyPosition == std::string_view::npos) return invalid_binding;
            std::size_t digit = keyPosition + key.size();
            while (digit < layout.size() && (layout[digit] == '=' || std::isspace(static_cast<unsigned char>(layout[digit])))) ++digit;
            std::uint32_t value = 0;
            bool found = false;
            while (digit < layout.size() && std::isdigit(static_cast<unsigned char>(layout[digit]))) {
                found = true;
                value = value * 10u + static_cast<std::uint32_t>(layout[digit] - '0');
                ++digit;
            }
            return found ? value : invalid_binding;
        };
        const auto binding = parse_layout_value("binding");
        if (binding != invalid_binding) {
            const auto set = parse_layout_value("set");
            const auto semicolon = source.find(';', close + 1);
            const auto declaration = source.substr(close + 1, semicolon == std::string_view::npos ? source.size() - close - 1 : semicolon - close - 1);
            const auto isStorage = declaration.find("buffer") != std::string_view::npos;
            const auto isStorageTexture = declaration.find("image") != std::string_view::npos && declaration.find("readonly") == std::string_view::npos;
            const auto isSampler = declaration.find(" sampler ") != std::string_view::npos || declaration.find(" sampler[") != std::string_view::npos;
            const auto isTexture = declaration.find("sampler") != std::string_view::npos || declaration.find("texture") != std::string_view::npos || declaration.find("image") != std::string_view::npos;
            const auto isStructured = declaration.find("buffer") != std::string_view::npos && declaration.find("readonly") != std::string_view::npos;
            const auto type = isStructured ? "structured_buffer" : (isStorage ? "storage_buffer" : (isStorageTexture ? "storage_texture" : (isSampler ? "sampler" : (isTexture ? "texture" : "uniform_buffer"))));
            ShaderBinding reflected{declaration_identifier(declaration), binding, set == invalid_binding ? 0u : set, type};
            const auto open = declaration.find_last_of('[');
            const auto closeBracket = declaration.find(']', open == std::string_view::npos ? 0 : open + 1);
            if (open != std::string_view::npos && closeBracket != std::string_view::npos) {
                const auto length = declaration.substr(open + 1, closeBracket - open - 1);
                if (length.empty()) { reflected.count = 0; reflected.unbounded = true; }
                else {
                    try { reflected.count = static_cast<std::uint32_t>(std::stoul(std::string(length))); }
                    catch (...) { reflected.count = 0; reflected.unbounded = true; }
                    if (reflected.count == 0) result.layoutValid = false;
                }
            }
            append_binding(result, std::move(reflected));
        }
        cursor = close + 1;
    }
}

void reflect_source(CompiledShader& result, std::string_view source) {
    reflect_hlsl_registers(result, source);
    reflect_glsl_layouts(result, source);
}

void reflect_spirv(CompiledShader& result) {
    if (result.bytecode.size() < 5 * sizeof(std::uint32_t) || result.bytecode.size() % sizeof(std::uint32_t) != 0) return;
    std::vector<std::uint32_t> words(result.bytecode.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), result.bytecode.data(), result.bytecode.size());
    if (words[0] != 0x07230203u) return;

    struct Decoration { std::uint32_t binding{invalid_binding}; std::uint32_t set{0}; };
    std::unordered_map<std::uint32_t, std::string> names;
    std::unordered_map<std::uint32_t, Decoration> decorations;
    std::unordered_map<std::uint32_t, std::uint32_t> pointeeTypes;
    std::unordered_map<std::uint32_t, std::uint32_t> typeKinds;
    std::unordered_map<std::uint32_t, std::uint32_t> arrayLengths;
    std::unordered_map<std::uint32_t, std::uint32_t> constants;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> variables;

    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto header = words[cursor];
        const auto count = header >> 16;
        const auto opcode = header & 0xffffu;
        if (count == 0 || cursor + count > words.size()) break;
        if (opcode == 5 && count >= 3) {
            const auto id = words[cursor + 1];
            std::string name;
            for (std::size_t index = cursor + 2; index < cursor + count; ++index) {
                const auto word = words[index];
                for (std::uint32_t byte = 0; byte < 4; ++byte) {
                    const char character = static_cast<char>((word >> (byte * 8)) & 0xffu);
                    if (character == '\0') break;
                    name.push_back(character);
                }
            }
            names[id] = std::move(name);
        } else if (opcode == 71 && count >= 4) {
            auto& decoration = decorations[words[cursor + 1]];
            if (words[cursor + 2] == 33) decoration.binding = words[cursor + 3];
            if (words[cursor + 2] == 34) decoration.set = words[cursor + 3];
        } else if (opcode == 59 && count >= 4) {
            variables.emplace_back(words[cursor + 1], words[cursor + 2], words[cursor + 3]);
        } else if (opcode == 25 || opcode == 26 || opcode == 27 || opcode == 30) {
            if (count >= 2) typeKinds[words[cursor + 1]] = opcode;
        } else if (opcode == 43 && count >= 4) {
            constants[words[cursor + 2]] = words[cursor + 3];
        } else if (opcode == 28 || opcode == 29) {
            if (count >= 3) {
                typeKinds[words[cursor + 1]] = opcode;
                pointeeTypes[words[cursor + 1]] = words[cursor + 2];
                if (opcode == 28 && count >= 4) arrayLengths[words[cursor + 1]] = words[cursor + 3];
            }
        } else if (opcode == 32) {
            if (count >= 4) pointeeTypes[words[cursor + 1]] = words[cursor + 3];
        }
        cursor += count;
    }

    struct ReflectedType { std::string type; std::uint32_t count{1}; bool unbounded{false}; };
    const auto classify = [&](std::uint32_t typeId) {
        ReflectedType reflected;
        for (std::size_t depth = 0; depth < 16; ++depth) {
            const auto kind = typeKinds.find(typeId);
            if (kind != typeKinds.end()) {
                if ((kind->second == 25 || kind->second == 27) && reflected.type.empty()) reflected.type = "texture";
                if (kind->second == 26 && reflected.type.empty()) reflected.type = "sampler";
                if (kind->second == 30 && reflected.type.empty()) reflected.type = "uniform_buffer";
                if (kind->second == 28 || kind->second == 29) {
                    if (kind->second == 29) { reflected.count = 0; reflected.unbounded = true; }
                    else if (const auto length = arrayLengths.find(typeId); length != arrayLengths.end()) {
                        const auto value = constants.find(length->second);
                        if (value != constants.end()) reflected.count = value->second;
                    }
                }
            }
            const auto next = pointeeTypes.find(typeId);
            if (next == pointeeTypes.end()) break;
            typeId = next->second;
        }
        if (reflected.type.empty()) reflected.type = "unknown";
        return reflected;
    };

    for (const auto& [typeId, id, storageClass] : variables) {
        const auto decoration = decorations.find(id);
        if (decoration == decorations.end() || decoration->second.binding == invalid_binding) continue;
        auto reflected = classify(typeId);
        if (storageClass == 2 && reflected.type == "unknown") reflected.type = "uniform_buffer";
        if (storageClass == 12 && reflected.type == "unknown") reflected.type = "storage_buffer";
        if (reflected.count == 0 && !reflected.unbounded) result.layoutValid = false;
        append_binding(result, {names.count(id) ? names[id] : std::string{}, decoration->second.binding, decoration->second.set,
            std::move(reflected.type), reflected.count, reflected.unbounded});
    }
}

#if defined(SHINKOU_PLATFORM_WINDOWS)
void reflect_dxbc(CompiledShader& result) {
    ID3D11ShaderReflection* reflection = nullptr;
    if (FAILED(D3DReflect(result.bytecode.data(), result.bytecode.size(), IID_ID3D11ShaderReflection,
        reinterpret_cast<void**>(&reflection))) || !reflection) return;
    D3D11_SHADER_DESC shaderDesc{};
    if (SUCCEEDED(reflection->GetDesc(&shaderDesc))) {
        for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &binding))) {
                ShaderBinding reflected;
                reflected.name = binding.Name ? binding.Name : "";
                reflected.slot = binding.BindPoint;
                reflected.space = 0;
                reflected.count = binding.BindCount == 0 ? 1u : binding.BindCount;
                reflected.unbounded = binding.BindCount == UINT_MAX;
                switch (binding.Type) {
                case D3D_SIT_CBUFFER: reflected.type = "uniform_buffer"; break;
                case D3D_SIT_TEXTURE: reflected.type = "texture"; break;
                case D3D_SIT_SAMPLER: reflected.type = "sampler"; break;
                default: reflected.type = "storage_buffer"; break;
                }
                append_binding(result, std::move(reflected));
            }
        }
    }
    reflection->Release();
}

void reflect_dxil(CompiledShader& result) {
    const GUID clsidDxcUtils{0x6245d6af, 0x66e0, 0x48fd, {0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c}};
    const GUID iidDxcUtils{0x4605c4cb, 0x2019, 0x492a, {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};
    const GUID iidShaderReflection{0x5a58797d, 0xa72c, 0x478d, {0x8b, 0xa2, 0xef, 0xc6, 0xb0, 0xef, 0xe8, 0x8e}};
    const auto module = LoadLibraryW(L"dxcompiler.dll");
    if (!module) return;
    const auto rawCreateInstance = GetProcAddress(module, "DxcCreateInstance");
    DxcCreateInstanceProc createInstance = nullptr;
    static_assert(sizeof(createInstance) == sizeof(rawCreateInstance));
    std::memcpy(&createInstance, &rawCreateInstance, sizeof(createInstance));
    if (!createInstance) {
        FreeLibrary(module);
        return;
    }
    IDxcUtils* utils = nullptr;
    if (FAILED(createInstance(clsidDxcUtils, iidDxcUtils, reinterpret_cast<void**>(&utils))) || !utils) {
        FreeLibrary(module);
        return;
    }
    const DxcBuffer shaderBuffer{result.bytecode.data(), result.bytecode.size(), DXC_CP_ACP};
    DxcBuffer reflectionBuffer = shaderBuffer;
    void* reflectionPart = nullptr;
    UINT32 reflectionPartSize = 0;
    if (SUCCEEDED(utils->GetDxilContainerPart(&shaderBuffer, DXC_PART_REFLECTION_DATA, &reflectionPart, &reflectionPartSize)) &&
        reflectionPart != nullptr && reflectionPartSize != 0) {
        reflectionBuffer = {reflectionPart, reflectionPartSize, DXC_CP_ACP};
    }
    ID3D12ShaderReflection* reflection = nullptr;
    if (SUCCEEDED(utils->CreateReflection(&reflectionBuffer, iidShaderReflection,
        reinterpret_cast<void**>(&reflection))) && reflection) {
        D3D12_SHADER_DESC shaderDescription{};
        if (SUCCEEDED(reflection->GetDesc(&shaderDescription))) {
            for (UINT index = 0; index < shaderDescription.BoundResources; ++index) {
                D3D12_SHADER_INPUT_BIND_DESC binding{};
                if (FAILED(reflection->GetResourceBindingDesc(index, &binding))) continue;
                ShaderBinding reflected;
                reflected.name = binding.Name ? binding.Name : "";
                reflected.slot = binding.BindPoint;
                reflected.space = binding.Space;
                reflected.count = binding.BindCount == 0 ? 1u : binding.BindCount;
                reflected.unbounded = binding.BindCount == UINT_MAX;
                switch (binding.Type) {
                case D3D_SIT_CBUFFER: reflected.type = "uniform_buffer"; break;
                case D3D_SIT_SAMPLER: reflected.type = "sampler"; break;
                case D3D_SIT_TEXTURE: reflected.type = "texture"; break;
                case D3D_SIT_STRUCTURED: reflected.type = "structured_buffer"; break;
                case D3D_SIT_BYTEADDRESS: reflected.type = "storage_buffer"; break;
                case D3D_SIT_UAV_RWTYPED:
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS: reflected.type = "storage_buffer"; break;
                case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED: reflected.type = "storage_buffer"; break;
                default: reflected.type = "texture"; break;
                }
                append_binding(result, std::move(reflected));
            }
        }
        reflection->Release();
    }
    utils->Release();
    FreeLibrary(module);
}
#endif

class DefaultShaderCompiler final : public IShaderCompiler {
    mutable std::unordered_map<std::uint64_t, CompiledShader> cache_;
    mutable std::mutex cacheMutex_;
public:
    CompiledShader compile(const ShaderDesc& shader, BackendApi backend) const override {
        const auto key = shader_variant_hash(shader, backend);
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            const auto cached = cache_.find(key);
            if (cached != cache_.end()) {
                auto result = cached->second;
                result.cacheHit = true;
                return result;
            }
        }
        CompiledShader diskCached;
        if (read_disk_cache(key, diskCached)) {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            cache_[key] = diskCached;
            return diskCached;
        }
        auto result = compile_uncached(shader, backend);
        result.variantHash = key;
        result.cacheHit = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            cache_[key] = result;
        }
        write_disk_cache(key, result);
        return result;
    }
private:
    static std::filesystem::path cache_directory() {
        if (const auto* directory = std::getenv("SHINKOU_SHADER_CACHE_DIR")) return directory;
        return std::filesystem::temp_directory_path() / "shinkou_shader_cache";
    }
    static std::filesystem::path cache_path(std::uint64_t key) {
        return cache_directory() / (std::to_string(key) + ".wsc");
    }
    static bool read_disk_cache(std::uint64_t key, CompiledShader& result) {
        std::ifstream stream(cache_path(key), std::ios::binary);
        if (!stream) return false;
        std::uint32_t version = 0;
        std::uint32_t stage = 0;
        std::uint32_t format = 0;
        std::uint32_t bindingCount = 0;
        std::uint64_t bytecodeSize = 0;
        std::uint64_t storedKey = 0;
        if (!stream.read(reinterpret_cast<char*>(&version), sizeof(version)) || version != 5 ||
            !stream.read(reinterpret_cast<char*>(&storedKey), sizeof(storedKey)) || storedKey != key ||
            !stream.read(reinterpret_cast<char*>(&stage), sizeof(stage)) ||
            !stream.read(reinterpret_cast<char*>(&format), sizeof(format)) ||
            !stream.read(reinterpret_cast<char*>(&bytecodeSize), sizeof(bytecodeSize)) ||
            !stream.read(reinterpret_cast<char*>(&bindingCount), sizeof(bindingCount)) ||
            bytecodeSize > (1ull << 30) || bindingCount > (1u << 20)) return false;
        result.stage = static_cast<ShaderStage>(stage);
        result.format = static_cast<ShaderFormat>(format);
        result.bytecode.resize(static_cast<std::size_t>(bytecodeSize));
        if (bytecodeSize && !stream.read(reinterpret_cast<char*>(result.bytecode.data()), static_cast<std::streamsize>(bytecodeSize))) return false;
        for (std::uint32_t i = 0; i < bindingCount; ++i) {
            std::uint32_t nameSize = 0;
            ShaderBinding binding;
            if (!stream.read(reinterpret_cast<char*>(&nameSize), sizeof(nameSize)) || nameSize > (1u << 20)) return false;
            binding.name.resize(nameSize);
            if (nameSize && !stream.read(binding.name.data(), static_cast<std::streamsize>(nameSize))) return false;
            if (!stream.read(reinterpret_cast<char*>(&binding.slot), sizeof(binding.slot)) ||
                !stream.read(reinterpret_cast<char*>(&binding.space), sizeof(binding.space)) ||
                !stream.read(reinterpret_cast<char*>(&binding.count), sizeof(binding.count)) ||
                !stream.read(reinterpret_cast<char*>(&binding.unbounded), sizeof(binding.unbounded)) ||
                !stream.read(reinterpret_cast<char*>(&nameSize), sizeof(nameSize)) || nameSize > (1u << 20)) return false;
            binding.type.resize(nameSize);
            if (nameSize && !stream.read(binding.type.data(), static_cast<std::streamsize>(nameSize))) return false;
            if (binding.type.empty() || (binding.count == 0 && !binding.unbounded)) return false;
            result.bindings.push_back(std::move(binding));
        }
        std::vector<ShaderBinding> merged;
        std::string diagnostics;
        if (!merge_shader_bindings(result.bindings, merged, diagnostics)) return false;
        if (stream.peek() != std::ifstream::traits_type::eof()) return false;
        result.valid = true;
        result.layoutValid = true;
        result.variantHash = key;
        result.cacheHit = true;
        return true;
    }
    static void write_disk_cache(std::uint64_t key, const CompiledShader& result) {
        if (!result.valid) return;
        std::error_code error;
        std::filesystem::create_directories(cache_directory(), error);
        if (error) return;
        const auto path = cache_path(key);
        const auto temporary = path.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        const std::uint32_t version = 5;
        const auto stage = static_cast<std::uint32_t>(result.stage);
        const auto format = static_cast<std::uint32_t>(result.format);
        const auto bytecodeSize = static_cast<std::uint64_t>(result.bytecode.size());
        const auto bindingCount = static_cast<std::uint32_t>(result.bindings.size());
        stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
        stream.write(reinterpret_cast<const char*>(&key), sizeof(key));
        stream.write(reinterpret_cast<const char*>(&stage), sizeof(stage));
        stream.write(reinterpret_cast<const char*>(&format), sizeof(format));
        stream.write(reinterpret_cast<const char*>(&bytecodeSize), sizeof(bytecodeSize));
        stream.write(reinterpret_cast<const char*>(&bindingCount), sizeof(bindingCount));
        if (bytecodeSize) stream.write(reinterpret_cast<const char*>(result.bytecode.data()), static_cast<std::streamsize>(bytecodeSize));
        for (const auto& binding : result.bindings) {
            const auto nameSize = static_cast<std::uint32_t>(binding.name.size());
            const auto typeSize = static_cast<std::uint32_t>(binding.type.size());
            stream.write(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
            stream.write(binding.name.data(), static_cast<std::streamsize>(nameSize));
            stream.write(reinterpret_cast<const char*>(&binding.slot), sizeof(binding.slot));
            stream.write(reinterpret_cast<const char*>(&binding.space), sizeof(binding.space));
            stream.write(reinterpret_cast<const char*>(&binding.count), sizeof(binding.count));
            stream.write(reinterpret_cast<const char*>(&binding.unbounded), sizeof(binding.unbounded));
            stream.write(reinterpret_cast<const char*>(&typeSize), sizeof(typeSize));
            stream.write(binding.type.data(), static_cast<std::streamsize>(typeSize));
        }
        stream.close();
        if (stream) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) std::filesystem::remove(temporary, error);
    }
    CompiledShader compile_uncached(const ShaderDesc& shader, BackendApi backend) const {
        CompiledShader result;
        result.stage = shader.stage;
        if (!shader.bytecode.empty()) {
            result.bytecode = shader.bytecode;
            result.valid = true;
            result.format = shader.sourceKind == ShaderSourceKind::Dxbc ? ShaderFormat::Dxbc :
                (shader.sourceKind == ShaderSourceKind::Dxil ? ShaderFormat::Dxil : ShaderFormat::SpirV);
            if (result.format == ShaderFormat::SpirV) reflect_spirv(result);
#if defined(SHINKOU_PLATFORM_WINDOWS)
            if (result.format == ShaderFormat::Dxbc) reflect_dxbc(result);
            if (result.format == ShaderFormat::Dxil) reflect_dxil(result);
#endif
            result.valid = result.layoutValid;
            return result;
        }
        if (shader.source.empty()) {
            result.diagnostics = "shader source is empty; resource remains logical-only";
            return result;
        }

#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (backend == BackendApi::DirectX11 || backend == BackendApi::DirectX12) {
            const char* profile = shader.profile.c_str();
            const std::string fallback = shader.stage == ShaderStage::Vertex ? "vs_5_0" :
                (shader.stage == ShaderStage::Fragment ? "ps_5_0" : "cs_5_0");
            if (shader.profile.empty()) profile = fallback.c_str();
            std::vector<std::string> defineNames;
            std::vector<std::string> defineValues;
            std::vector<D3D_SHADER_MACRO> macros;
            defineNames.reserve(shader.defines.size());
            defineValues.reserve(shader.defines.size());
            for (const auto& define : shader.defines) {
                const auto separator = define.find('=');
                defineNames.push_back(define.substr(0, separator));
                defineValues.push_back(separator == std::string::npos ? "1" : define.substr(separator + 1));
            }
            for (std::size_t index = 0; index < defineNames.size(); ++index) {
                macros.push_back({defineNames[index].c_str(), defineValues[index].c_str()});
            }
            macros.push_back({nullptr, nullptr});
            ID3DBlob* bytecode = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT hr = D3DCompile(shader.source.data(), shader.source.size(), shader.name.c_str(),
                shader.defines.empty() ? nullptr : macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, shader.entryPoint.c_str(), profile,
                D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
            if (errors) {
                result.diagnostics.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                errors->Release();
            }
            if (FAILED(hr) || !bytecode) return result;
            const auto* bytes = static_cast<const std::uint8_t*>(bytecode->GetBufferPointer());
            result.bytecode.assign(bytes, bytes + bytecode->GetBufferSize());
            bytecode->Release();
            result.format = ShaderFormat::Dxbc;
            reflect_dxbc(result);
            result.valid = result.layoutValid;
            return result;
        }
#endif

        if (backend == BackendApi::Vulkan) {
            if (shader.source.size() >= sizeof(std::uint32_t)) {
                std::uint32_t magic = 0;
                std::memcpy(&magic, shader.source.data(), sizeof(magic));
                if (magic == 0x07230203u) {
                    result.bytecode.assign(shader.source.begin(), shader.source.end());
                    result.format = ShaderFormat::SpirV;
                    reflect_spirv(result);
                    result.valid = result.layoutValid;
                    return result;
                }
            }
            result.diagnostics = "Vulkan shaders require SPIR-V bytecode or an external GLSL/HLSL compiler integration";
            return result;
        }

        result.format = ShaderFormat::Source;
        result.bytecode.assign(shader.source.begin(), shader.source.end());
        reflect_source(result, shader.source);
        result.valid = result.layoutValid;
        return result;
    }
};
}

std::uint64_t shader_variant_hash(const ShaderDesc& shader, BackendApi backend) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto backendValue = static_cast<std::uint32_t>(backend);
    const auto stageValue = static_cast<std::uint32_t>(shader.stage);
    const auto sourceKindValue = static_cast<std::uint32_t>(shader.sourceKind);
    hash = hash_bytes(hash, &backendValue, sizeof(backendValue));
    hash = hash_bytes(hash, &stageValue, sizeof(stageValue));
    hash = hash_bytes(hash, &sourceKindValue, sizeof(sourceKindValue));
    hash = hash_bytes(hash, &shader.revision, sizeof(shader.revision));
    hash_string(hash, shader.name);
    hash_string(hash, shader.entryPoint);
    hash_string(hash, shader.profile);
    hash_string(hash, shader.source);
    if (!shader.bytecode.empty()) hash = hash_bytes(hash, shader.bytecode.data(), shader.bytecode.size());
    auto defines = shader.defines;
    std::sort(defines.begin(), defines.end());
    for (const auto& define : defines) hash_string(hash, define);
    return hash;
}

std::unique_ptr<IShaderCompiler> create_shader_compiler() {
    return std::make_unique<DefaultShaderCompiler>();
}

bool merge_shader_bindings(const std::vector<ShaderBinding>& source,
                           std::vector<ShaderBinding>& merged,
                           std::string& diagnostics) {
    for (const auto& binding : source) {
        const auto existing = std::find_if(merged.begin(), merged.end(), [&](const auto& candidate) {
            return candidate.slot == binding.slot && candidate.space == binding.space;
        });
        if (existing == merged.end()) {
            merged.push_back(binding);
            continue;
        }
        if (existing->type != binding.type || existing->count != binding.count || existing->unbounded != binding.unbounded) {
            diagnostics = "shader stages disagree on descriptor space=" + std::to_string(binding.space) +
                " slot=" + std::to_string(binding.slot) + " (" + existing->type + " vs " + binding.type + ")";
            return false;
        }
    }
    return true;
}
}
