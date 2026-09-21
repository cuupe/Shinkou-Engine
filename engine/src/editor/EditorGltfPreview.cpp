#include "shinkou/editor/EditorGltfPreview.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::editor {
namespace {

constexpr std::size_t kMaxSourceBytes = 128u * 1024u * 1024u;
constexpr std::size_t kMaxJsonDepth = 32;
constexpr std::size_t kMaxJsonArrayEntries = 500000;
constexpr std::size_t kMaxJsonObjectEntries = 4096;
constexpr std::size_t kMaxStringBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaxVertices = 500000;
constexpr std::size_t kMaxTriangles = 1000000;
constexpr std::size_t kMaxWireSegments = 8192;
constexpr std::size_t kMaxBufferBytes = 128u * 1024u * 1024u;
constexpr std::size_t kMaxModelMetadataEntries = 4096;
constexpr std::size_t kMaxAnimationKeys = 65536;
constexpr float kMaxAnimationDuration = 3600.0f;
constexpr std::size_t kMaxImageArtifactBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxImageArtifactTotalBytes = 64u * 1024u * 1024u;

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Object, Array };
    Kind kind{Kind::Null};
    double number{0.0};
    bool boolean{false};
    std::string string;
    std::map<std::string, JsonValue, std::less<>> object;
    std::vector<JsonValue> array;
};

class JsonParser final {
    std::string_view input_;
    std::size_t position_{0};

    bool fail(std::string& error, std::string message) const {
        error = std::move(message);
        return false;
    }

    void skip_space() noexcept {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool consume(char value) noexcept {
        if (position_ >= input_.size() || input_[position_] != value) return false;
        ++position_;
        return true;
    }

    static bool hex_digit(char value, unsigned& output) noexcept {
        if (value >= '0' && value <= '9') { output = static_cast<unsigned>(value - '0'); return true; }
        if (value >= 'a' && value <= 'f') { output = static_cast<unsigned>(value - 'a' + 10); return true; }
        if (value >= 'A' && value <= 'F') { output = static_cast<unsigned>(value - 'A' + 10); return true; }
        return false;
    }

    static void append_utf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        output.clear();
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20u) return fail(error, "control character in JSON string");
            if (character != '\\') {
                if (output.size() >= kMaxStringBytes) return fail(error, "glTF JSON string exceeds the preview limit");
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) return fail(error, "unfinished JSON escape");
            const auto escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    if (position_ >= input_.size()) return fail(error, "unfinished unicode escape");
                    unsigned digit = 0;
                    if (!hex_digit(input_[position_++], digit)) return fail(error, "invalid unicode escape");
                    codepoint = (codepoint << 4u) | digit;
                }
                append_utf8(output, codepoint);
                break;
            }
            default: return fail(error, "invalid JSON escape");
            }
            if (output.size() > kMaxStringBytes) return fail(error, "glTF JSON string exceeds the preview limit");
        }
        return fail(error, "unterminated JSON string");
    }

    bool parse_literal(JsonValue& output, std::string_view literal, JsonValue::Kind kind,
                       std::string& error) {
        if (input_.substr(position_, literal.size()) != literal) return fail(error, "invalid JSON literal");
        position_ += literal.size();
        output.kind = kind;
        output.boolean = kind == JsonValue::Kind::Boolean && literal == "true";
        return true;
    }

    bool parse_number(JsonValue& output, std::string& error) {
        const auto start = position_;
        while (position_ < input_.size() && std::string_view("0123456789+-.eE").find(input_[position_]) != std::string_view::npos)
            ++position_;
        if (start == position_) return fail(error, "invalid JSON value");
        const std::string number(input_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !std::isfinite(value))
            return fail(error, "invalid JSON number");
        output.kind = JsonValue::Kind::Number;
        output.number = value;
        return true;
    }

    bool parse_value(JsonValue& output, std::string& error, std::size_t depth) {
        if (depth > kMaxJsonDepth) return fail(error, "glTF JSON nesting is too deep");
        skip_space();
        if (position_ >= input_.size()) return fail(error, "unexpected end of JSON");
        switch (input_[position_]) {
        case '{': return parse_object(output, error, depth + 1);
        case '[': return parse_array(output, error, depth + 1);
        case '"': output.kind = JsonValue::Kind::String; return parse_string(output.string, error);
        case 'n': return parse_literal(output, "null", JsonValue::Kind::Null, error);
        case 't': return parse_literal(output, "true", JsonValue::Kind::Boolean, error);
        case 'f': return parse_literal(output, "false", JsonValue::Kind::Boolean, error);
        default: return parse_number(output, error);
        }
    }

    bool parse_object(JsonValue& output, std::string& error, std::size_t depth) {
        consume('{');
        output.kind = JsonValue::Kind::Object;
        output.object.clear();
        skip_space();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            if (output.object.size() >= kMaxJsonObjectEntries)
                return fail(error, "glTF JSON object has too many members");
            skip_space();
            std::string key;
            if (!parse_string(key, error)) return false;
            skip_space();
            if (!consume(':')) return fail(error, "expected ':' in JSON object");
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            if (!output.object.emplace(std::move(key), std::move(child)).second)
                return fail(error, "duplicate glTF JSON key");
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON object");
        }
        return fail(error, "unterminated JSON object");
    }

    bool parse_array(JsonValue& output, std::string& error, std::size_t depth) {
        consume('[');
        output.kind = JsonValue::Kind::Array;
        output.array.clear();
        skip_space();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            if (output.array.size() >= kMaxJsonArrayEntries)
                return fail(error, "glTF JSON array has too many entries");
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            output.array.push_back(std::move(child));
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON array");
        }
        return fail(error, "unterminated JSON array");
    }

public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output, std::string& error) {
        if (input_.size() > kMaxSourceBytes) return fail(error, "glTF JSON exceeds the 128 MiB preview limit");
        skip_space();
        if (!parse_value(output, error, 0)) return false;
        skip_space();
        return position_ == input_.size() || fail(error, "trailing JSON data");
    }
};

struct BufferViewDesc {
    std::size_t buffer{0};
    std::size_t byteOffset{0};
    std::size_t byteLength{0};
    std::size_t byteStride{0};
};

struct AccessorDesc {
    std::size_t bufferView{0};
    std::size_t byteOffset{0};
    std::size_t count{0};
    std::uint32_t componentType{0};
    std::string type;
};

struct Geometry {
    std::vector<math::Vec3> vertices;
    std::vector<math::Vec2> textureCoordinates;
    std::vector<math::Vec3> normals;
    std::vector<std::uint32_t> indices;
    std::vector<animation::BoneIndex> vertexBones;
    bool hasTextureCoordinates{false};
    bool hasNormals{false};
};

const JsonValue* member(const JsonValue& value, std::string_view name) {
    if (value.kind != JsonValue::Kind::Object) return nullptr;
    const auto found = value.object.find(name);
    return found == value.object.end() ? nullptr : &found->second;
}

bool number_to_size(const JsonValue* value, std::size_t maximum, std::size_t& output) {
    if (!value || value->kind != JsonValue::Kind::Number || value->number < 0.0 ||
        !std::isfinite(value->number) || std::floor(value->number) != value->number ||
        value->number > static_cast<double>(maximum)) return false;
    output = static_cast<std::size_t>(value->number);
    return true;
}

bool number_to_u32(const JsonValue* value, std::uint32_t& output) {
    std::size_t parsed = 0;
    if (!number_to_size(value, std::numeric_limits<std::uint32_t>::max(), parsed)) return false;
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

bool string_value(const JsonValue* value, std::string& output) {
    if (!value || value->kind != JsonValue::Kind::String) return false;
    output = value->string;
    return true;
}

bool number_to_float(const JsonValue* value, float minimum, float maximum, float& output) {
    if (!value || value->kind != JsonValue::Kind::Number || !std::isfinite(value->number) ||
        value->number < static_cast<double>(minimum) || value->number > static_cast<double>(maximum)) return false;
    output = static_cast<float>(value->number);
    return std::isfinite(output);
}

bool boolean_value(const JsonValue* value, bool& output) {
    if (!value || value->kind != JsonValue::Kind::Boolean) return false;
    output = value->boolean;
    return true;
}

bool optional_string(const JsonValue& object, std::string_view name, std::string& output,
                     std::string& error) {
    const auto* value = member(object, name);
    if (!value) return true;
    if (!string_value(value, output)) {
        error = "glTF " + std::string(name) + " must be a string";
        return false;
    }
    return true;
}

bool optional_index(const JsonValue& object, std::string_view name, std::size_t maximum,
                    std::int32_t& output, std::string& error) {
    const auto* value = member(object, name);
    if (!value) return true;
    std::size_t parsed = 0;
    if (!number_to_size(value, maximum, parsed) || parsed > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        error = "glTF " + std::string(name) + " index is invalid";
        return false;
    }
    output = static_cast<std::int32_t>(parsed);
    return true;
}

bool project_relative_image_uri(std::string_view uri) {
    if (uri.empty() || uri.rfind("data:", 0) == 0) return true;
    if (uri.find("://") != std::string_view::npos || uri.find('\0') != std::string_view::npos) return false;
    const std::filesystem::path path{std::string(uri)};
    if (path.is_absolute()) return false;
    const auto normalized = path.lexically_normal().generic_string();
    return normalized != ".." && normalized.rfind("../", 0) != 0;
}

bool parse_image_metadata(const JsonValue& value, std::vector<EditorModelImagePreview>& output,
                          std::string& error) {
    if (value.kind != JsonValue::Kind::Object) {
        error = "glTF image is not an object";
        return false;
    }
    EditorModelImagePreview image;
    if (!optional_string(value, "name", image.name, error) ||
        !optional_string(value, "uri", image.uri, error) ||
        !optional_string(value, "mimeType", image.mimeType, error) ||
        !optional_index(value, "bufferView", std::numeric_limits<std::size_t>::max(), image.bufferView, error)) return false;
    if (!project_relative_image_uri(image.uri)) {
        error = "glTF image URI is not project-relative";
        return false;
    }
    if (image.uri.empty() && image.bufferView < 0) {
        error = "glTF image has neither a URI nor a bufferView";
        return false;
    }
    output.push_back(std::move(image));
    return true;
}

bool parse_texture_metadata(const JsonValue& value, std::size_t imageCount, std::size_t samplerCount,
                            std::vector<EditorModelTexturePreview>& output, std::string& error) {
    if (value.kind != JsonValue::Kind::Object) {
        error = "glTF texture is not an object";
        return false;
    }
    EditorModelTexturePreview texture;
    if (!optional_string(value, "name", texture.name, error) ||
        !optional_index(value, "source", imageCount == 0 ? 0 : imageCount - 1, texture.source, error) ||
        !optional_index(value, "sampler", samplerCount == 0 ? 0 : samplerCount - 1, texture.sampler, error)) return false;
    if (texture.source >= 0 && static_cast<std::size_t>(texture.source) >= imageCount) {
        error = "glTF texture source is outside the image table";
        return false;
    }
    if (texture.sampler >= 0 && static_cast<std::size_t>(texture.sampler) >= samplerCount) {
        error = "glTF texture sampler is outside the sampler table";
        return false;
    }
    output.push_back(std::move(texture));
    return true;
}

bool parse_material_metadata(const JsonValue& value, std::size_t textureCount,
                            std::vector<EditorModelMaterialPreview>& output, std::string& error) {
    if (value.kind != JsonValue::Kind::Object) {
        error = "glTF material is not an object";
        return false;
    }
    EditorModelMaterialPreview material;
    if (!optional_string(value, "name", material.name, error)) return false;
    if (const auto* alphaMode = member(value, "alphaMode")) {
        if (!string_value(alphaMode, material.alphaMode) ||
            (material.alphaMode != "OPAQUE" && material.alphaMode != "MASK" && material.alphaMode != "BLEND")) {
            error = "glTF material alphaMode is invalid";
            return false;
        }
    }
    if (const auto* doubleSided = member(value, "doubleSided")) {
        if (!boolean_value(doubleSided, material.doubleSided)) {
            error = "glTF material doubleSided is invalid";
            return false;
        }
    }
    if (const auto* pbr = member(value, "pbrMetallicRoughness")) {
        if (pbr->kind != JsonValue::Kind::Object) {
            error = "glTF material PBR block is invalid";
            return false;
        }
        if (const auto* baseColor = member(*pbr, "baseColorFactor")) {
            if (baseColor->kind != JsonValue::Kind::Array || baseColor->array.size() != 4u) {
                error = "glTF baseColorFactor must contain four values";
                return false;
            }
            for (std::size_t index = 0; index < 4u; ++index) {
                if (!number_to_float(&baseColor->array[index], 0.0f, 1.0f, material.baseColorFactor[index])) {
                    error = "glTF baseColorFactor contains an invalid value";
                    return false;
                }
            }
        }
        if (const auto* metallic = member(*pbr, "metallicFactor")) {
            if (!number_to_float(metallic, 0.0f, 1.0f, material.metallicFactor)) {
                error = "glTF metallicFactor is invalid";
                return false;
            }
        }
        if (const auto* roughness = member(*pbr, "roughnessFactor")) {
            if (!number_to_float(roughness, 0.0f, 1.0f, material.roughnessFactor)) {
                error = "glTF roughnessFactor is invalid";
                return false;
            }
        }
        if (const auto* baseColorTexture = member(*pbr, "baseColorTexture")) {
            if (baseColorTexture->kind != JsonValue::Kind::Object ||
                !optional_index(*baseColorTexture, "index", textureCount == 0 ? 0 : textureCount - 1,
                                material.baseColorTexture, error)) return false;
            if (material.baseColorTexture < 0 || static_cast<std::size_t>(material.baseColorTexture) >= textureCount) {
                error = "glTF baseColorTexture index is outside the texture table";
                return false;
            }
        }
        if (const auto* metallicRoughnessTexture = member(*pbr, "metallicRoughnessTexture")) {
            if (metallicRoughnessTexture->kind != JsonValue::Kind::Object) {
                error = "glTF metallicRoughnessTexture is not an object";
                return false;
            }
            if (!optional_index(*metallicRoughnessTexture, "index",
                                textureCount == 0 ? 0 : textureCount - 1,
                                material.metallicRoughnessTexture, error)) {
                error = "glTF metallicRoughnessTexture index is invalid";
                return false;
            }
            if (material.metallicRoughnessTexture < 0 ||
                static_cast<std::size_t>(material.metallicRoughnessTexture) >= textureCount) {
                error = "glTF metallicRoughnessTexture index is outside the texture table";
                return false;
            }
        }
    }
    if (const auto* normalTexture = member(value, "normalTexture")) {
        if (normalTexture->kind != JsonValue::Kind::Object ||
            !optional_index(*normalTexture, "index", textureCount == 0 ? 0 : textureCount - 1,
                            material.normalTexture, error)) return false;
        if (material.normalTexture < 0 || static_cast<std::size_t>(material.normalTexture) >= textureCount) {
            error = "glTF normalTexture index is outside the texture table";
            return false;
        }
    }
    output.push_back(std::move(material));
    return true;
}

bool safe_add(std::size_t left, std::size_t right, std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    output = left + right;
    return true;
}

bool safe_mul(std::size_t left, std::size_t right, std::size_t& output) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    output = left * right;
    return true;
}

bool in_range(std::size_t offset, std::size_t length, std::size_t size) noexcept {
    return offset <= size && length <= size - offset;
}

std::uint32_t read_u32_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

float read_f32_le(const std::uint8_t* bytes) noexcept {
    const auto bits = read_u32_le(bytes);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool read_file_bytes(const std::filesystem::path& path, std::size_t maximum,
                     std::vector<std::uint8_t>& output, std::string& error) {
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError) { error = "model file size is unavailable"; return false; }
    if (size > maximum) { error = "model file exceeds the 128 MiB preview limit"; return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "model file cannot be opened"; return false; }
    output.resize(static_cast<std::size_t>(size));
    if (!output.empty()) file.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!file && !output.empty()) { error = "model file cannot be read"; return false; }
    return true;
}

bool decode_base64(std::string_view input, std::vector<std::uint8_t>& output,
                   std::string& error, std::size_t maximum = kMaxBufferBytes) {
    output.clear();
    unsigned accumulator = 0;
    unsigned bits = 0;
    auto digit = [](char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    };
    for (const char value : input) {
        if (value == '=') break;
        const int parsed = digit(value);
        if (parsed < 0) { error = "unsupported glTF data URI encoding"; return false; }
        accumulator = (accumulator << 6u) | static_cast<unsigned>(parsed);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (output.size() >= maximum) { error = "glTF binary payload exceeds the preview limit"; return false; }
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffu));
        }
    }
    if (bits >= 6u) { error = "invalid glTF base64 payload"; return false; }
    return true;
}

bool load_buffer(const FileSystemService& files, const std::filesystem::path& modelPath,
                 std::string_view uri, const std::vector<std::uint8_t>* embedded,
                 std::vector<std::uint8_t>& output, std::string& error) {
    if (uri.empty()) {
        if (!embedded) { error = "glTF buffer has no URI or embedded BIN chunk"; return false; }
        output = *embedded;
        return output.size() <= kMaxBufferBytes || (error = "glTF buffer exceeds the preview limit", false);
    }
    if (uri.rfind("data:", 0) == 0) {
        const auto comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos) {
            error = "glTF buffer data URI is not base64 encoded";
            return false;
        }
        return decode_base64(uri.substr(comma + 1), output, error);
    }
    if (uri.find("://") != std::string_view::npos || uri.find('\0') != std::string_view::npos) {
        error = "glTF external buffer URI is not a project-relative path";
        return false;
    }
    const std::filesystem::path uriPath{std::string(uri)};
    if (uriPath.is_absolute()) { error = "glTF external buffer URI is absolute"; return false; }
    const auto relative = (modelPath.parent_path() / uriPath).lexically_normal();
    const auto absolute = files.resolve_existing(relative);
    if (absolute.empty()) { error = "glTF external buffer is outside the project or missing"; return false; }
    return read_file_bytes(absolute, kMaxBufferBytes, output, error);
}

bool load_image_artifact(const FileSystemService& files,
                         const std::filesystem::path& modelPath,
                         const EditorModelImagePreview& image,
                         const std::vector<BufferViewDesc>& views,
                         const std::vector<std::vector<std::uint8_t>>& buffers,
                         std::vector<std::uint8_t>& output,
                         std::string& error) {
    output.clear();
    if (!image.uri.empty() && image.uri.rfind("data:", 0) == 0) {
        const auto comma = image.uri.find(',');
        if (comma == std::string::npos || image.uri.substr(0, comma).find(";base64") == std::string::npos) {
            error = "glTF image data URI is not base64 encoded";
            return false;
        }
        return decode_base64(image.uri.substr(comma + 1), output, error, kMaxImageArtifactBytes);
    }
    if (image.bufferView >= 0) {
        const auto viewIndex = static_cast<std::size_t>(image.bufferView);
        if (viewIndex >= views.size() || views[viewIndex].buffer >= buffers.size()) {
            error = "glTF image bufferView is outside the buffer table";
            return false;
        }
        const auto& view = views[viewIndex];
        const auto& buffer = buffers[view.buffer];
        if (view.byteLength > kMaxImageArtifactBytes || !in_range(view.byteOffset, view.byteLength, buffer.size())) {
            error = "glTF image bufferView exceeds the image artifact limit";
            return false;
        }
        output.assign(buffer.begin() + static_cast<std::ptrdiff_t>(view.byteOffset),
                      buffer.begin() + static_cast<std::ptrdiff_t>(view.byteOffset + view.byteLength));
        return !output.empty() || (error = "glTF image bufferView is empty", false);
    }
    if (image.uri.empty()) {
        error = "glTF image has no payload URI or bufferView";
        return false;
    }
    if (!project_relative_image_uri(image.uri)) {
        error = "glTF image URI is not project-relative";
        return false;
    }
    const std::filesystem::path uriPath{image.uri};
    const auto relative = (modelPath.parent_path() / uriPath).lexically_normal();
    const auto absolute = files.resolve_existing(relative);
    if (absolute.empty()) {
        error = "glTF external image is outside the project or missing";
        return false;
    }
    return read_file_bytes(absolute, kMaxImageArtifactBytes, output, error);
}

bool parse_glb(const std::vector<std::uint8_t>& bytes, std::string& json,
               std::vector<std::uint8_t>& binary, std::string& error) {
    if (bytes.size() < 12 || read_u32_le(bytes.data()) != 0x46546c67u ||
        read_u32_le(bytes.data() + 4) != 2u) {
        error = "GLB header is invalid or unsupported";
        return false;
    }
    const auto declaredLength = static_cast<std::size_t>(read_u32_le(bytes.data() + 8));
    if (declaredLength < 12 || declaredLength > bytes.size() || declaredLength > kMaxSourceBytes) {
        error = "GLB length is outside the preview limit";
        return false;
    }
    std::size_t cursor = 12;
    while (cursor < declaredLength) {
        if (!in_range(cursor, 8, declaredLength)) { error = "GLB chunk header is truncated"; return false; }
        const auto chunkLength = static_cast<std::size_t>(read_u32_le(bytes.data() + cursor));
        const auto chunkType = read_u32_le(bytes.data() + cursor + 4);
        cursor += 8;
        if (!in_range(cursor, chunkLength, declaredLength)) { error = "GLB chunk exceeds the container"; return false; }
        if (chunkType == 0x4e4f534au) {
            if (!json.empty()) { error = "GLB contains multiple JSON chunks"; return false; }
            json.assign(reinterpret_cast<const char*>(bytes.data() + cursor), chunkLength);
            while (!json.empty() && (json.back() == '\0' || std::isspace(static_cast<unsigned char>(json.back())))) json.pop_back();
        } else if (chunkType == 0x004e4942u) {
            if (!binary.empty()) { error = "GLB contains multiple BIN chunks"; return false; }
            binary.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                          bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkLength));
        }
        cursor += chunkLength;
    }
    if (json.empty()) { error = "GLB has no JSON chunk"; return false; }
    return true;
}

bool parse_accessor(const JsonValue& value, AccessorDesc& output, std::string& error) {
    if (value.kind != JsonValue::Kind::Object ||
        !number_to_size(member(value, "bufferView"), kMaxJsonArrayEntries, output.bufferView) ||
        !number_to_size(member(value, "count"), kMaxVertices * 3u, output.count) ||
        !number_to_u32(member(value, "componentType"), output.componentType) ||
        !string_value(member(value, "type"), output.type)) {
        error = "glTF accessor is missing a supported descriptor";
        return false;
    }
    if (const auto* offset = member(value, "byteOffset")) {
        if (!number_to_size(offset, kMaxBufferBytes, output.byteOffset)) { error = "glTF accessor byteOffset is invalid"; return false; }
    }
    return true;
}

bool parse_buffer_view(const JsonValue& value, BufferViewDesc& output, std::string& error) {
    if (value.kind != JsonValue::Kind::Object ||
        !number_to_size(member(value, "buffer"), kMaxJsonArrayEntries, output.buffer) ||
        !number_to_size(member(value, "byteLength"), kMaxBufferBytes, output.byteLength)) {
        error = "glTF bufferView is missing a supported descriptor";
        return false;
    }
    if (const auto* offset = member(value, "byteOffset")) {
        if (!number_to_size(offset, kMaxBufferBytes, output.byteOffset)) { error = "glTF bufferView byteOffset is invalid"; return false; }
    }
    if (const auto* stride = member(value, "byteStride")) {
        if (!number_to_size(stride, 4096, output.byteStride)) { error = "glTF bufferView byteStride is invalid"; return false; }
    }
    return true;
}

bool read_position_accessor(const AccessorDesc& accessor, const BufferViewDesc& view,
                            const std::vector<std::vector<std::uint8_t>>& buffers,
                            const std::atomic_bool* cancel, std::vector<math::Vec3>& output,
                            std::string& error) {
    if (accessor.type != "VEC3" || accessor.componentType != 5126u || accessor.count == 0 ||
        accessor.count > kMaxVertices || view.buffer >= buffers.size()) {
        error = "glTF POSITION accessor is not a float VEC3";
        return false;
    }
    const auto& buffer = buffers[view.buffer];
    const auto stride = view.byteStride == 0 ? 12u : view.byteStride;
    if (stride < 12u) { error = "glTF POSITION byteStride is too small"; return false; }
    std::size_t base = 0;
    if (!safe_add(view.byteOffset, accessor.byteOffset, base)) { error = "glTF POSITION offset overflows"; return false; }
    std::size_t span = 0;
    if (!safe_mul(accessor.count - 1u, stride, span) || !safe_add(span, 12u, span) ||
        !in_range(base, span, buffer.size()) || !in_range(view.byteOffset, view.byteLength, buffer.size()) ||
        base < view.byteOffset || !in_range(base - view.byteOffset, span, view.byteLength)) {
        error = "glTF POSITION accessor exceeds its bufferView";
        return false;
    }
    output.reserve(output.size() + accessor.count);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { error = "model preview cancelled"; return false; }
        std::size_t offset = 0;
        if (!safe_mul(index, stride, offset) || !safe_add(base, offset, offset)) {
            error = "glTF POSITION offset overflows"; return false;
        }
        const math::Vec3 position{read_f32_le(buffer.data() + offset),
                                  read_f32_le(buffer.data() + offset + 4),
                                  read_f32_le(buffer.data() + offset + 8)};
        if (!math::IsFinite(position.x) || !math::IsFinite(position.y) || !math::IsFinite(position.z)) {
            error = "glTF POSITION contains a non-finite value"; return false;
        }
        output.push_back(position);
    }
    return true;
}

bool read_texcoord_accessor(const AccessorDesc& accessor, const BufferViewDesc& view,
                            const std::vector<std::vector<std::uint8_t>>& buffers,
                            const std::atomic_bool* cancel, std::vector<math::Vec2>& output,
                            std::string& error) {
    if (accessor.type != "VEC2" || accessor.componentType != 5126u || accessor.count == 0 ||
        accessor.count > kMaxVertices || view.buffer >= buffers.size()) {
        error = "glTF TEXCOORD_0 accessor is not a float VEC2";
        return false;
    }
    const auto& buffer = buffers[view.buffer];
    const auto stride = view.byteStride == 0 ? 8u : view.byteStride;
    if (stride < 8u) { error = "glTF TEXCOORD_0 byteStride is too small"; return false; }
    std::size_t base = 0;
    if (!safe_add(view.byteOffset, accessor.byteOffset, base)) {
        error = "glTF TEXCOORD_0 offset overflows";
        return false;
    }
    std::size_t span = 0;
    if (!safe_mul(accessor.count - 1u, stride, span) || !safe_add(span, 8u, span) ||
        !in_range(base, span, buffer.size()) || !in_range(view.byteOffset, view.byteLength, buffer.size()) ||
        base < view.byteOffset || !in_range(base - view.byteOffset, span, view.byteLength)) {
        error = "glTF TEXCOORD_0 accessor exceeds its bufferView";
        return false;
    }
    output.reserve(output.size() + accessor.count);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { error = "model preview cancelled"; return false; }
        std::size_t offset = 0;
        if (!safe_mul(index, stride, offset) || !safe_add(base, offset, offset)) {
            error = "glTF TEXCOORD_0 offset overflows";
            return false;
        }
        const math::Vec2 coordinate{read_f32_le(buffer.data() + offset),
                                    read_f32_le(buffer.data() + offset + 4)};
        if (!math::IsFinite(coordinate.x) || !math::IsFinite(coordinate.y)) {
            error = "glTF TEXCOORD_0 contains a non-finite value";
            return false;
        }
        output.push_back(coordinate);
    }
    return true;
}

bool read_normal_accessor(const AccessorDesc& accessor, const BufferViewDesc& view,
                          const std::vector<std::vector<std::uint8_t>>& buffers,
                          const std::atomic_bool* cancel, std::vector<math::Vec3>& output,
                          std::string& error) {
    if (accessor.type != "VEC3" || accessor.componentType != 5126u || accessor.count == 0 ||
        accessor.count > kMaxVertices || view.buffer >= buffers.size()) {
        error = "glTF NORMAL accessor is not a float VEC3";
        return false;
    }
    const auto& buffer = buffers[view.buffer];
    const auto stride = view.byteStride == 0 ? 12u : view.byteStride;
    if (stride < 12u) { error = "glTF NORMAL byteStride is too small"; return false; }
    std::size_t base = 0;
    if (!safe_add(view.byteOffset, accessor.byteOffset, base)) {
        error = "glTF NORMAL offset overflows";
        return false;
    }
    std::size_t span = 0;
    if (!safe_mul(accessor.count - 1u, stride, span) || !safe_add(span, 12u, span) ||
        !in_range(base, span, buffer.size()) || !in_range(view.byteOffset, view.byteLength, buffer.size()) ||
        base < view.byteOffset || !in_range(base - view.byteOffset, span, view.byteLength)) {
        error = "glTF NORMAL accessor exceeds its bufferView";
        return false;
    }
    output.reserve(output.size() + accessor.count);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { error = "model preview cancelled"; return false; }
        std::size_t offset = 0;
        if (!safe_mul(index, stride, offset) || !safe_add(base, offset, offset)) {
            error = "glTF NORMAL offset overflows";
            return false;
        }
        const math::Vec3 normal{read_f32_le(buffer.data() + offset),
                                read_f32_le(buffer.data() + offset + 4),
                                read_f32_le(buffer.data() + offset + 8)};
        if (!math::IsFinite(normal.x) || !math::IsFinite(normal.y) || !math::IsFinite(normal.z) ||
            math::LengthSquared(normal) <= math::Epsilon) {
            error = "glTF NORMAL contains an invalid vector";
            return false;
        }
        output.push_back(math::Normalize(normal));
    }
    return true;
}

bool read_index_accessor(const AccessorDesc& accessor, const BufferViewDesc& view,
                         const std::vector<std::vector<std::uint8_t>>& buffers,
                         const std::atomic_bool* cancel, std::size_t vertexCount,
                         std::vector<std::uint32_t>& output, std::string& error) {
    if (accessor.type != "SCALAR" || accessor.count == 0 || accessor.count > kMaxVertices * 3u ||
        view.buffer >= buffers.size() || (accessor.componentType != 5121u &&
        accessor.componentType != 5123u && accessor.componentType != 5125u)) {
        error = "glTF index accessor is not a supported scalar";
        return false;
    }
    const std::size_t componentSize = accessor.componentType == 5121u ? 1u : accessor.componentType == 5123u ? 2u : 4u;
    const auto stride = view.byteStride == 0 ? componentSize : view.byteStride;
    if (stride < componentSize) { error = "glTF index byteStride is too small"; return false; }
    const auto& buffer = buffers[view.buffer];
    std::size_t base = 0, span = 0;
    if (!safe_add(view.byteOffset, accessor.byteOffset, base) ||
        !safe_mul(accessor.count - 1u, stride, span) || !safe_add(span, componentSize, span) ||
        !in_range(base, span, buffer.size()) || !in_range(view.byteOffset, view.byteLength, buffer.size()) ||
        base < view.byteOffset || !in_range(base - view.byteOffset, span, view.byteLength)) {
        error = "glTF index accessor exceeds its bufferView";
        return false;
    }
    output.reserve(output.size() + accessor.count);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { error = "model preview cancelled"; return false; }
        std::size_t offset = 0;
        if (!safe_mul(index, stride, offset) || !safe_add(base, offset, offset)) {
            error = "glTF index offset overflows"; return false;
        }
        std::uint32_t value = 0;
        if (componentSize == 1u) value = buffer[offset];
        else if (componentSize == 2u) value = static_cast<std::uint32_t>(buffer[offset]) |
            (static_cast<std::uint32_t>(buffer[offset + 1]) << 8u);
        else value = read_u32_le(buffer.data() + offset);
        if (value >= vertexCount) { error = "glTF index points outside POSITION"; return false; }
        output.push_back(value);
    }
    return true;
}

bool read_float_accessor(const AccessorDesc& accessor, const BufferViewDesc& view,
                         const std::vector<std::vector<std::uint8_t>>& buffers,
                         const std::atomic_bool* cancel, std::size_t components,
                         std::vector<float>& output, std::string& error) {
    if (accessor.componentType != 5126u || accessor.count == 0 ||
        accessor.count > kMaxAnimationKeys || view.buffer >= buffers.size() ||
        (accessor.type == "SCALAR" && components != 1u) ||
        (accessor.type == "VEC3" && components != 3u) ||
        (accessor.type == "VEC4" && components != 4u) ||
        (accessor.type != "SCALAR" && accessor.type != "VEC3" && accessor.type != "VEC4")) {
        error = "glTF animation accessor is not a supported float vector";
        return false;
    }
    const std::size_t elementBytes = components * sizeof(float);
    const auto stride = view.byteStride == 0 ? elementBytes : view.byteStride;
    if (stride < elementBytes) { error = "glTF animation byteStride is too small"; return false; }
    const auto& buffer = buffers[view.buffer];
    std::size_t base = 0, span = 0;
    if (!safe_add(view.byteOffset, accessor.byteOffset, base) ||
        !safe_mul(accessor.count - 1u, stride, span) || !safe_add(span, elementBytes, span) ||
        !in_range(base, span, buffer.size()) || !in_range(view.byteOffset, view.byteLength, buffer.size()) ||
        base < view.byteOffset || !in_range(base - view.byteOffset, span, view.byteLength)) {
        error = "glTF animation accessor exceeds its bufferView";
        return false;
    }
    std::size_t outputCount = 0;
    if (!safe_mul(accessor.count, components, outputCount) ||
        outputCount > kMaxAnimationKeys * 4u) {
        error = "glTF animation accessor exceeds the preview key limit";
        return false;
    }
    output.reserve(output.size() + outputCount);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            error = "model preview cancelled";
            return false;
        }
        std::size_t offset = 0;
        if (!safe_mul(index, stride, offset) || !safe_add(base, offset, offset)) {
            error = "glTF animation accessor offset overflows";
            return false;
        }
        for (std::size_t component = 0; component < components; ++component) {
            const auto value = read_f32_le(buffer.data() + offset + component * sizeof(float));
            if (!std::isfinite(value)) {
                error = "glTF animation accessor contains a non-finite value";
                return false;
            }
            output.push_back(value);
        }
    }
    return true;
}

bool parse_float_array(const JsonValue* value, std::size_t count, float minimum, float maximum,
                       float* output, std::string& error, std::string_view label) {
    if (!value || value->kind != JsonValue::Kind::Array || value->array.size() != count) {
        error = "glTF " + std::string(label) + " must contain " + std::to_string(count) + " values";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!number_to_float(&value->array[index], minimum, maximum, output[index])) {
            error = "glTF " + std::string(label) + " contains an invalid value";
            return false;
        }
    }
    return true;
}

std::uint64_t revision(std::string_view path, std::uint64_t stamp,
                       std::size_t vertices, std::size_t triangles, std::string_view format) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) { hash ^= bytes[index]; hash *= 1099511628211ull; }
    };
    mix(path.data(), path.size()); mix(format.data(), format.size());
    mix(&stamp, sizeof(stamp)); mix(&vertices, sizeof(vertices)); mix(&triangles, sizeof(triangles));
    return hash == 0 ? 1 : hash;
}

void calculate_bounds_and_wire(const Geometry& geometry, EditorModelPreviewSnapshot& snapshot,
                               const std::atomic_bool* cancel) {
    math::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    math::Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const auto& point : geometry.vertices) {
        minimum.x = std::min(minimum.x, point.x); minimum.y = std::min(minimum.y, point.y); minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x); maximum.y = std::max(maximum.y, point.y); maximum.z = std::max(maximum.z, point.z);
    }
    snapshot.minX = minimum.x; snapshot.minY = minimum.y; snapshot.minZ = minimum.z;
    snapshot.maxX = maximum.x; snapshot.maxY = maximum.y; snapshot.maxZ = maximum.z;
    const math::Vec3 extent = maximum - minimum;
    int horizontal = 0, vertical = 1;
    if (extent.z > extent.x && extent.z >= extent.y) horizontal = 2;
    if ((horizontal == 0 && extent.z > extent.y) || (horizontal == 2 && extent.x > extent.y)) vertical = 1;
    else if (horizontal == 0) vertical = 2;
    else vertical = 0;
    const auto coordinate = [](const math::Vec3& point, int axis) { return axis == 0 ? point.x : axis == 1 ? point.y : point.z; };
    const float minHorizontal = coordinate(minimum, horizontal);
    const float minVertical = coordinate(minimum, vertical);
    const float horizontalExtent = std::max(1.0e-6f, coordinate(maximum, horizontal) - minHorizontal);
    const float verticalExtent = std::max(1.0e-6f, coordinate(maximum, vertical) - minVertical);
    auto wire = std::make_shared<std::vector<ui::Vec2>>();
    wire->reserve(std::min<std::size_t>(geometry.indices.size() * 2u, kMaxWireSegments * 2u));
    const auto project = [&](std::uint32_t index) {
        const auto& point = geometry.vertices[index];
        return ui::Vec2{(coordinate(point, horizontal) - minHorizontal) / horizontalExtent,
                        1.0f - (coordinate(point, vertical) - minVertical) / verticalExtent};
    };
    for (std::size_t index = 0; index + 2 < geometry.indices.size() && wire->size() < kMaxWireSegments * 2u; index += 3) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        const auto a = project(geometry.indices[index]);
        const auto b = project(geometry.indices[index + 1]);
        const auto c = project(geometry.indices[index + 2]);
        wire->push_back(a); wire->push_back(b); wire->push_back(b); wire->push_back(c); wire->push_back(c); wire->push_back(a);
    }
    snapshot.wireSegments = std::move(wire);
}

EditorModelPreviewResult failed(std::string path, std::uint64_t generation,
                                std::uint64_t sourceStamp, std::string error) {
    EditorModelPreviewResult result;
    result.generation = generation; result.sourceStamp = sourceStamp;
    result.path = std::move(path); result.error = std::move(error);
    return result;
}

} // namespace

static EditorModelPreviewResult load_editor_gltf_preview_source(
    const FileSystemService& files, std::string path,
    const std::vector<std::uint8_t>& source,
    std::uint64_t generation, std::uint64_t sourceStamp,
    const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return failed(path, generation, sourceStamp, "model preview cancelled");

    std::string error;
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool binary = extension == ".glb";
    std::string json;
    std::vector<std::uint8_t> embeddedBinary;
    if (binary) {
        if (!parse_glb(source, json, embeddedBinary, error)) return failed(path, generation, sourceStamp, error);
    } else {
        json.assign(reinterpret_cast<const char*>(source.data()), source.size());
        if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xefu &&
            static_cast<unsigned char>(json[1]) == 0xbbu && static_cast<unsigned char>(json[2]) == 0xbfu) json.erase(0, 3);
    }
    JsonValue root;
    if (!JsonParser(json).parse(root, error) || root.kind != JsonValue::Kind::Object)
        return failed(path, generation, sourceStamp, error.empty() ? "glTF JSON root is not an object" : error);

    const auto* buffersValue = member(root, "buffers");
    const auto* viewsValue = member(root, "bufferViews");
    const auto* accessorsValue = member(root, "accessors");
    const auto* meshesValue = member(root, "meshes");
    if (!buffersValue || buffersValue->kind != JsonValue::Kind::Array ||
        !viewsValue || viewsValue->kind != JsonValue::Kind::Array ||
        !accessorsValue || accessorsValue->kind != JsonValue::Kind::Array ||
        !meshesValue || meshesValue->kind != JsonValue::Kind::Array || meshesValue->array.empty())
        return failed(path, generation, sourceStamp, "glTF is missing buffers, accessors, bufferViews or meshes");
    if (buffersValue->array.size() > 256 || viewsValue->array.size() > kMaxJsonArrayEntries ||
        accessorsValue->array.size() > kMaxJsonArrayEntries || meshesValue->array.size() > 4096)
        return failed(path, generation, sourceStamp, "glTF table exceeds the preview limit");

    std::vector<std::vector<std::uint8_t>> buffers;
    buffers.reserve(buffersValue->array.size());
    const auto modelRelative = std::filesystem::path(path);
    std::size_t totalBufferBytes = 0;
    for (std::size_t index = 0; index < buffersValue->array.size(); ++index) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return failed(path, generation, sourceStamp, "model preview cancelled");
        const auto& value = buffersValue->array[index];
        std::size_t declaredLength = 0;
        if (value.kind != JsonValue::Kind::Object || !number_to_size(member(value, "byteLength"), kMaxBufferBytes, declaredLength))
            return failed(path, generation, sourceStamp, "glTF buffer has an invalid byteLength");
        std::string uri;
        const auto* uriValue = member(value, "uri");
        if (uriValue && !string_value(uriValue, uri)) return failed(path, generation, sourceStamp, "glTF buffer URI is invalid");
        std::vector<std::uint8_t> loaded;
        if (!load_buffer(files, modelRelative, uri, binary && index == 0 ? &embeddedBinary : nullptr, loaded, error))
            return failed(path, generation, sourceStamp, error);
        if (loaded.size() < declaredLength) return failed(path, generation, sourceStamp, "glTF buffer is shorter than byteLength");
        loaded.resize(declaredLength);
        if (!safe_add(totalBufferBytes, loaded.size(), totalBufferBytes) || totalBufferBytes > kMaxBufferBytes)
            return failed(path, generation, sourceStamp, "glTF buffers exceed the aggregate preview limit");
        buffers.push_back(std::move(loaded));
    }

    std::vector<BufferViewDesc> views;
    views.reserve(viewsValue->array.size());
    for (const auto& value : viewsValue->array) {
        BufferViewDesc view;
        if (!parse_buffer_view(value, view, error) || view.buffer >= buffers.size() ||
            !in_range(view.byteOffset, view.byteLength, buffers[view.buffer].size()))
            return failed(path, generation, sourceStamp, error.empty() ? "glTF bufferView exceeds its buffer" : error);
        views.push_back(view);
    }
    std::vector<AccessorDesc> accessors;
    accessors.reserve(accessorsValue->array.size());
    for (const auto& value : accessorsValue->array) {
        AccessorDesc accessor;
        if (!parse_accessor(value, accessor, error) || accessor.bufferView >= views.size())
            return failed(path, generation, sourceStamp, error.empty() ? "glTF accessor references an invalid bufferView" : error);
        accessors.push_back(std::move(accessor));
    }

    std::vector<EditorModelNodePreview> nodeMetadata;
    std::vector<math::Transform> nodeBindPose;
    std::vector<animation::BoneIndex> meshBones(meshesValue->array.size(), animation::InvalidBone);
    std::shared_ptr<animation::Skeleton> animationSkeleton;
    if (const auto* nodesValue = member(root, "nodes")) {
        if (nodesValue->kind != JsonValue::Kind::Array ||
            nodesValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF node table exceeds the preview limit");
        nodeMetadata.resize(nodesValue->array.size());
        nodeBindPose.resize(nodesValue->array.size());
        for (std::size_t index = 0; index < nodesValue->array.size(); ++index) {
            const auto& value = nodesValue->array[index];
            if (value.kind != JsonValue::Kind::Object)
                return failed(path, generation, sourceStamp, "glTF node is not an object");
            auto& node = nodeMetadata[index];
            if (!optional_string(value, "name", node.name, error) ||
                !optional_index(value, "mesh", meshesValue->array.empty() ? 0 : meshesValue->array.size() - 1u,
                                node.mesh, error))
                return failed(path, generation, sourceStamp, error);
            if (node.name.empty()) node.name = "Node #" + std::to_string(index + 1u);
            const bool hasMatrix = member(value, "matrix") != nullptr;
            const bool hasTranslation = member(value, "translation") != nullptr;
            const bool hasRotation = member(value, "rotation") != nullptr;
            const bool hasScale = member(value, "scale") != nullptr;
            if (hasMatrix && (hasTranslation || hasRotation || hasScale))
                return failed(path, generation, sourceStamp, "glTF node cannot combine matrix and TRS transforms");
            auto& bind = nodeBindPose[index];
            if (hasMatrix) {
                const auto* matrix = member(value, "matrix");
                if (!matrix || matrix->kind != JsonValue::Kind::Array || matrix->array.size() != 16u)
                    return failed(path, generation, sourceStamp, "glTF node matrix is invalid");
                math::Mat4 matrixValue = math::Mat4::Identity();
                for (std::size_t component = 0; component < 16u; ++component) {
                    if (!number_to_float(&matrix->array[component], -1.0e6f, 1.0e6f,
                                         matrixValue.m[component]))
                        return failed(path, generation, sourceStamp, "glTF node matrix contains an invalid value");
                }
                if (!math::Decompose(matrixValue, bind))
                    return failed(path, generation, sourceStamp, "glTF node matrix cannot be decomposed for preview");
            } else {
                float values[4]{};
                if (hasTranslation) {
                    if (!parse_float_array(member(value, "translation"), 3u, -1.0e6f, 1.0e6f,
                                           values, error, "node translation"))
                        return failed(path, generation, sourceStamp, error);
                    bind.position = {values[0], values[1], values[2]};
                }
                if (hasRotation) {
                    if (!parse_float_array(member(value, "rotation"), 4u, -1.0f, 1.0f,
                                           values, error, "node rotation"))
                        return failed(path, generation, sourceStamp, error);
                    bind.rotation = math::Normalize(math::Quat{values[0], values[1], values[2], values[3]});
                }
                if (hasScale) {
                    if (!parse_float_array(member(value, "scale"), 3u, -1.0e4f, 1.0e4f,
                                           values, error, "node scale"))
                        return failed(path, generation, sourceStamp, error);
                    bind.scale = {values[0], values[1], values[2]};
                }
            }
        }
        for (std::size_t index = 0; index < nodesValue->array.size(); ++index) {
            const auto* children = member(nodesValue->array[index], "children");
            if (!children) continue;
            if (children->kind != JsonValue::Kind::Array || children->array.size() > kMaxModelMetadataEntries)
                return failed(path, generation, sourceStamp, "glTF node children list is invalid");
            for (const auto& child : children->array) {
                std::size_t childIndex = 0;
                if (!number_to_size(&child, nodeMetadata.size() == 0 ? 0 : nodeMetadata.size() - 1u, childIndex) ||
                    childIndex == index)
                    return failed(path, generation, sourceStamp, "glTF node child index is invalid");
                auto& childNode = nodeMetadata[childIndex];
                if (childNode.parent >= 0 && childNode.parent != static_cast<std::int32_t>(index))
                    return failed(path, generation, sourceStamp, "glTF node has multiple parents");
                childNode.parent = static_cast<std::int32_t>(index);
            }
        }
        std::vector<std::uint8_t> visitState(nodeMetadata.size(), 0);
        std::vector<std::size_t> order;
        order.reserve(nodeMetadata.size());
        const auto visit = [&](auto&& self, std::size_t nodeIndex) -> bool {
            if (visitState[nodeIndex] == 1u) return false;
            if (visitState[nodeIndex] == 2u) return true;
            visitState[nodeIndex] = 1u;
            const auto parent = nodeMetadata[nodeIndex].parent;
            if (parent >= 0 && !self(self, static_cast<std::size_t>(parent))) return false;
            visitState[nodeIndex] = 2u;
            order.push_back(nodeIndex);
            return true;
        };
        for (std::size_t index = 0; index < nodeMetadata.size(); ++index) {
            if (!visit(visit, index))
                return failed(path, generation, sourceStamp, "glTF node hierarchy contains a cycle");
        }
        animationSkeleton = std::make_shared<animation::Skeleton>();
        std::vector<animation::BoneIndex> nodeBones(nodeMetadata.size(), animation::InvalidBone);
        for (const auto nodeIndex : order) {
            const auto boneIndex = static_cast<animation::BoneIndex>(animationSkeleton->parents.size());
            nodeBones[nodeIndex] = boneIndex;
            nodeMetadata[nodeIndex].bone = boneIndex;
            const auto parent = nodeMetadata[nodeIndex].parent;
            animationSkeleton->parents.push_back(parent < 0 ? animation::InvalidBone : nodeBones[static_cast<std::size_t>(parent)]);
            animationSkeleton->bindPose.push_back(nodeBindPose[nodeIndex]);
            animationSkeleton->inverseBindMatrices.push_back(math::Mat4::Identity());
            animationSkeleton->names.push_back(nodeMetadata[nodeIndex].name);
        }
        if (!animationSkeleton->valid())
            return failed(path, generation, sourceStamp, "glTF node hierarchy is not a valid preview skeleton");
        for (const auto& node : nodeMetadata) {
            if (node.mesh >= 0 && static_cast<std::size_t>(node.mesh) < meshBones.size() &&
                meshBones[static_cast<std::size_t>(node.mesh)] == animation::InvalidBone)
                meshBones[static_cast<std::size_t>(node.mesh)] = node.bone;
        }
    }

    std::vector<EditorModelImagePreview> imageMetadata;
    std::vector<EditorModelTextureArtifact> imageArtifacts;
    std::size_t totalImageArtifactBytes = 0;
    if (const auto* imagesValue = member(root, "images")) {
        if (imagesValue->kind != JsonValue::Kind::Array || imagesValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF image table exceeds the preview limit");
        imageMetadata.reserve(imagesValue->array.size());
        imageArtifacts.reserve(imagesValue->array.size());
        for (const auto& value : imagesValue->array) {
            if (!parse_image_metadata(value, imageMetadata, error))
                return failed(path, generation, sourceStamp, error);
            const auto& image = imageMetadata.back();
            if (image.bufferView >= 0 && static_cast<std::size_t>(image.bufferView) >= views.size())
                return failed(path, generation, sourceStamp, "glTF image bufferView is outside the bufferView table");
            std::vector<std::uint8_t> encoded;
            if (!load_image_artifact(files, modelRelative, image, views, buffers, encoded, error))
                return failed(path, generation, sourceStamp, error);
            if (!safe_add(totalImageArtifactBytes, encoded.size(), totalImageArtifactBytes) ||
                totalImageArtifactBytes > kMaxImageArtifactTotalBytes)
                return failed(path, generation, sourceStamp, "glTF image artifacts exceed the aggregate preview limit");
            EditorModelTextureArtifact artifact;
            artifact.imageIndex = static_cast<std::int32_t>(imageMetadata.size() - 1u);
            artifact.uri = image.uri;
            artifact.mimeType = image.mimeType;
            artifact.encodedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(encoded));
            imageArtifacts.push_back(std::move(artifact));
        }
    }

    std::vector<EditorModelTexturePreview> textureMetadata;
    std::size_t samplerCount = 0;
    if (const auto* samplersValue = member(root, "samplers")) {
        if (samplersValue->kind != JsonValue::Kind::Array || samplersValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF sampler table exceeds the preview limit");
        samplerCount = samplersValue->array.size();
    }
    if (const auto* texturesValue = member(root, "textures")) {
        if (texturesValue->kind != JsonValue::Kind::Array || texturesValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF texture table exceeds the preview limit");
        textureMetadata.reserve(texturesValue->array.size());
        for (const auto& value : texturesValue->array) {
            if (!parse_texture_metadata(value, imageMetadata.size(), samplerCount, textureMetadata, error))
                return failed(path, generation, sourceStamp, error);
        }
    }

    std::vector<EditorModelMaterialPreview> materialMetadata;
    if (const auto* materialsValue = member(root, "materials")) {
        if (materialsValue->kind != JsonValue::Kind::Array || materialsValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF material table exceeds the preview limit");
        materialMetadata.reserve(materialsValue->array.size());
        for (const auto& value : materialsValue->array) {
            if (!parse_material_metadata(value, textureMetadata.size(), materialMetadata, error))
                return failed(path, generation, sourceStamp, error);
        }
    }

    std::vector<EditorModelAnimationPreview> animationMetadata;
    if (const auto* animationsValue = member(root, "animations")) {
        if (animationsValue->kind != JsonValue::Kind::Array ||
            animationsValue->array.size() > kMaxModelMetadataEntries)
            return failed(path, generation, sourceStamp, "glTF animation table exceeds the preview limit");
        animationMetadata.reserve(animationsValue->array.size());
        for (std::size_t index = 0; index < animationsValue->array.size(); ++index) {
            const auto& value = animationsValue->array[index];
            const auto* samplers = member(value, "samplers");
            const auto* channels = member(value, "channels");
            if (value.kind != JsonValue::Kind::Object || !samplers ||
                samplers->kind != JsonValue::Kind::Array || !channels ||
                channels->kind != JsonValue::Kind::Array ||
                samplers->array.size() > kMaxModelMetadataEntries ||
                channels->array.size() > kMaxModelMetadataEntries)
                return failed(path, generation, sourceStamp, "glTF animation has invalid sampler/channel tables");
            EditorModelAnimationPreview animation;
            if (!optional_string(value, "name", animation.name, error))
                return failed(path, generation, sourceStamp, error);
            if (animation.name.empty()) animation.name = "Animation #" + std::to_string(index + 1u);
            animation.samplerCount = samplers->array.size();
            animation.channelCount = channels->array.size();
            bool fullySupported = !channels->array.empty() && animationSkeleton != nullptr;
            std::shared_ptr<animation::AnimationClip> clip;
            if (animationSkeleton) clip = std::make_shared<animation::AnimationClip>(animationSkeleton->bone_count());
            if (clip) clip->set_name(animation.name);
            for (const auto& channel : channels->array) {
                if (channel.kind != JsonValue::Kind::Object)
                    return failed(path, generation, sourceStamp, "glTF animation channel is not an object");
                std::size_t samplerIndex = 0;
                if (!number_to_size(member(channel, "sampler"),
                                    animation.samplerCount == 0 ? 0 : animation.samplerCount - 1u,
                                    samplerIndex))
                    return failed(path, generation, sourceStamp, "glTF animation channel sampler index is invalid");
                const auto& sampler = samplers->array[samplerIndex];
                if (sampler.kind != JsonValue::Kind::Object) {
                    fullySupported = false;
                    continue;
                }
                std::size_t inputIndex = 0;
                std::size_t outputIndex = 0;
                if (!number_to_size(member(sampler, "input"), accessors.size() ? accessors.size() - 1u : 0, inputIndex) ||
                    !number_to_size(member(sampler, "output"), accessors.size() ? accessors.size() - 1u : 0, outputIndex)) {
                    // Keep metadata-only animation records readable for older
                    // fixtures and partially authored assets. They are not
                    // exposed as CPU-playable clips without real accessors.
                    fullySupported = false;
                    continue;
                }
                std::string interpolation = "LINEAR";
                if (!optional_string(sampler, "interpolation", interpolation, error))
                    return failed(path, generation, sourceStamp, error);
                if (interpolation != "LINEAR") fullySupported = false;
                const auto* target = member(channel, "target");
                std::size_t nodeIndex = 0;
                std::string targetPath;
                if (!target || target->kind != JsonValue::Kind::Object ||
                    !number_to_size(member(*target, "node"), nodeMetadata.size() ? nodeMetadata.size() - 1u : 0, nodeIndex) ||
                    !string_value(member(*target, "path"), targetPath)) {
                    fullySupported = false;
                    continue;
                }
                if (targetPath != "translation" && targetPath != "rotation" && targetPath != "scale") {
                    fullySupported = false;
                    continue;
                }
                std::vector<float> times;
                if (!read_float_accessor(accessors[inputIndex], views[accessors[inputIndex].bufferView],
                                         buffers, cancel, 1u, times, error))
                    return failed(path, generation, sourceStamp, error);
                if (times.empty() || times.size() > kMaxAnimationKeys) {
                    fullySupported = false;
                    continue;
                }
                for (std::size_t timeIndex = 0; timeIndex < times.size(); ++timeIndex) {
                    if (times[timeIndex] < 0.0f || times[timeIndex] > kMaxAnimationDuration ||
                        (timeIndex != 0 && times[timeIndex] < times[timeIndex - 1u])) {
                        return failed(path, generation, sourceStamp, "glTF animation input times are invalid");
                    }
                }
                animation.duration = std::max(animation.duration, times.back());
                if (!clip || nodeMetadata[nodeIndex].bone == animation::InvalidBone || interpolation != "LINEAR")
                    continue;
                const std::size_t components = targetPath == "rotation" ? 4u : 3u;
                std::vector<float> values;
                if (!read_float_accessor(accessors[outputIndex], views[accessors[outputIndex].bufferView],
                                         buffers, cancel, components, values, error))
                    return failed(path, generation, sourceStamp, error);
                if (values.size() != times.size() * components) {
                    fullySupported = false;
                    continue;
                }
                auto& track = clip->track(nodeMetadata[nodeIndex].bone);
                for (std::size_t key = 0; key < times.size(); ++key) {
                    const auto offset = key * components;
                    if (targetPath == "translation") {
                        track.positions.push_back({times[key], {values[offset], values[offset + 1u], values[offset + 2u]}});
                    } else if (targetPath == "scale") {
                        track.scales.push_back({times[key], {values[offset], values[offset + 1u], values[offset + 2u]}});
                    } else {
                        track.rotations.push_back(animation::QuatKey{
                            times[key], math::Normalize(math::Quat{values[offset], values[offset + 1u],
                                                                  values[offset + 2u], values[offset + 3u]})});
                    }
                }
                ++animation.playableChannelCount;
            }
            animation.cpuPlayable = fullySupported && animation.playableChannelCount > 0 &&
                animation.duration >= 0.0f;
            if (animation.cpuPlayable && clip) {
                clip->set_duration(animation.duration);
                if (clip->valid_for(*animationSkeleton)) animation.cpuClip = std::move(clip);
                else animation.cpuPlayable = false;
            }
            animationMetadata.push_back(std::move(animation));
        }
    }

    Geometry geometry;
    std::size_t primitiveCount = 0;
    for (std::size_t meshIndex = 0; meshIndex < meshesValue->array.size(); ++meshIndex) {
        const auto& mesh = meshesValue->array[meshIndex];
        const auto* primitives = member(mesh, "primitives");
        if (!primitives || primitives->kind != JsonValue::Kind::Array || primitives->array.empty())
            return failed(path, generation, sourceStamp, "glTF mesh has no primitives");
        for (const auto& primitive : primitives->array) {
            ++primitiveCount;
            if (primitive.kind != JsonValue::Kind::Object) return failed(path, generation, sourceStamp, "glTF primitive is not an object");
            std::size_t mode = 4;
            if (const auto* modeValue = member(primitive, "mode")) {
                if (!number_to_size(modeValue, 7, mode)) return failed(path, generation, sourceStamp, "glTF primitive mode is invalid");
            }
            if (mode != 4) return failed(path, generation, sourceStamp, "glTF preview supports TRIANGLES primitives only");
            const auto* attributes = member(primitive, "attributes");
            const auto* positionIndex = attributes ? member(*attributes, "POSITION") : nullptr;
            std::size_t positionAccessorIndex = 0;
            if (!number_to_size(positionIndex, accessors.size() ? accessors.size() - 1 : 0, positionAccessorIndex))
                return failed(path, generation, sourceStamp, "glTF primitive has no POSITION accessor");
            if (positionAccessorIndex >= accessors.size())
                return failed(path, generation, sourceStamp, "glTF POSITION accessor index is outside the accessor table");
            const auto& positionAccessor = accessors[positionAccessorIndex];
            const auto& positionView = views[positionAccessor.bufferView];
            const std::size_t vertexOffset = geometry.vertices.size();
            if (vertexOffset > kMaxVertices - positionAccessor.count)
                return failed(path, generation, sourceStamp, "glTF vertex count exceeds the preview limit");
            if (!read_position_accessor(positionAccessor, positionView, buffers, cancel, geometry.vertices, error))
                return failed(path, generation, sourceStamp, error);
            geometry.vertexBones.insert(geometry.vertexBones.end(), positionAccessor.count,
                                        meshIndex < meshBones.size() ? meshBones[meshIndex] : animation::InvalidBone);
            std::vector<math::Vec2> localTextureCoordinates;
            const auto* textureCoordinateIndex = attributes ? member(*attributes, "TEXCOORD_0") : nullptr;
            if (textureCoordinateIndex) {
                std::size_t textureAccessorIndex = 0;
                if (!number_to_size(textureCoordinateIndex, accessors.size() ? accessors.size() - 1 : 0,
                                    textureAccessorIndex) || textureAccessorIndex >= accessors.size())
                    return failed(path, generation, sourceStamp, "glTF TEXCOORD_0 accessor index is invalid");
                const auto& textureAccessor = accessors[textureAccessorIndex];
                if (textureAccessor.count != positionAccessor.count ||
                    !read_texcoord_accessor(textureAccessor, views[textureAccessor.bufferView], buffers, cancel,
                                            localTextureCoordinates, error))
                    return failed(path, generation, sourceStamp,
                                  error.empty() ? "glTF TEXCOORD_0 count does not match POSITION" : error);
                geometry.hasTextureCoordinates = true;
            } else {
                localTextureCoordinates.resize(positionAccessor.count);
            }
            geometry.textureCoordinates.insert(geometry.textureCoordinates.end(),
                                               localTextureCoordinates.begin(), localTextureCoordinates.end());
            std::vector<math::Vec3> localNormals;
            const auto* normalIndex = attributes ? member(*attributes, "NORMAL") : nullptr;
            if (normalIndex) {
                std::size_t normalAccessorIndex = 0;
                if (!number_to_size(normalIndex, accessors.size() ? accessors.size() - 1 : 0,
                                    normalAccessorIndex) || normalAccessorIndex >= accessors.size())
                    return failed(path, generation, sourceStamp, "glTF NORMAL accessor index is invalid");
                const auto& normalAccessor = accessors[normalAccessorIndex];
                if (normalAccessor.count != positionAccessor.count ||
                    !read_normal_accessor(normalAccessor, views[normalAccessor.bufferView], buffers, cancel,
                                          localNormals, error))
                    return failed(path, generation, sourceStamp,
                                  error.empty() ? "glTF NORMAL count does not match POSITION" : error);
                geometry.hasNormals = true;
            } else {
                localNormals.resize(positionAccessor.count, {0.0f, 0.0f, 1.0f});
            }
            geometry.normals.insert(geometry.normals.end(), localNormals.begin(), localNormals.end());
            const auto* indexValue = member(primitive, "indices");
            std::size_t indexAccessorIndex = 0;
            if (indexValue) {
                if (!number_to_size(indexValue, accessors.size() ? accessors.size() - 1 : 0, indexAccessorIndex))
                    return failed(path, generation, sourceStamp, "glTF indices accessor is invalid");
                if (indexAccessorIndex >= accessors.size())
                    return failed(path, generation, sourceStamp, "glTF indices accessor index is outside the accessor table");
                std::vector<std::uint32_t> localIndices;
                const auto& indexAccessor = accessors[indexAccessorIndex];
                if (!read_index_accessor(indexAccessor, views[indexAccessor.bufferView], buffers, cancel,
                                         positionAccessor.count, localIndices, error))
                    return failed(path, generation, sourceStamp, error);
                if (localIndices.size() % 3u != 0u)
                    return failed(path, generation, sourceStamp, "glTF index count is not divisible by three");
                if (localIndices.size() / 3u > kMaxTriangles - geometry.indices.size() / 3u)
                    return failed(path, generation, sourceStamp, "glTF triangle count exceeds the preview limit");
                for (const auto index : localIndices) geometry.indices.push_back(static_cast<std::uint32_t>(vertexOffset + index));
            } else {
                if (positionAccessor.count % 3u != 0u || positionAccessor.count / 3u > kMaxTriangles - geometry.indices.size() / 3u)
                    return failed(path, generation, sourceStamp, "glTF non-indexed POSITION count is not a triangle list");
                for (std::size_t index = 0; index < positionAccessor.count; ++index)
                    geometry.indices.push_back(static_cast<std::uint32_t>(vertexOffset + index));
            }
        }
    }
    if (geometry.vertices.empty() || geometry.indices.empty()) return failed(path, generation, sourceStamp, "glTF contains no previewable geometry");

    auto snapshot = std::make_shared<EditorModelPreviewSnapshot>();
    snapshot->sourceFormat = binary ? "glb" : "gltf";
    snapshot->revision = revision(path, sourceStamp, geometry.vertices.size(), geometry.indices.size() / 3u, snapshot->sourceFormat);
    snapshot->vertexCount = geometry.vertices.size();
    snapshot->triangleCount = geometry.indices.size() / 3u;
    snapshot->objectCount = meshesValue->array.size();
    snapshot->meshCount = meshesValue->array.size();
    snapshot->primitiveCount = primitiveCount;
    snapshot->materialCount = materialMetadata.size();
    snapshot->textureCount = textureMetadata.size();
    snapshot->imageCount = imageMetadata.size();
    snapshot->animationCount = animationMetadata.size();
    calculate_bounds_and_wire(geometry, *snapshot, cancel);
    if (!snapshot->wireSegments) return failed(path, generation, sourceStamp, "model preview cancelled");
    snapshot->vertices = std::make_shared<const std::vector<math::Vec3>>(std::move(geometry.vertices));
    snapshot->indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(geometry.indices));
    if (geometry.hasTextureCoordinates) {
        snapshot->textureCoordinates = std::make_shared<const std::vector<math::Vec2>>(
            std::move(geometry.textureCoordinates));
    }
    if (geometry.hasNormals) {
        snapshot->normals = std::make_shared<const std::vector<math::Vec3>>(std::move(geometry.normals));
    }
    snapshot->materials = std::make_shared<const std::vector<EditorModelMaterialPreview>>(std::move(materialMetadata));
    snapshot->textures = std::make_shared<const std::vector<EditorModelTexturePreview>>(std::move(textureMetadata));
    snapshot->images = std::make_shared<const std::vector<EditorModelImagePreview>>(std::move(imageMetadata));
    snapshot->imageArtifacts = std::make_shared<const std::vector<EditorModelTextureArtifact>>(std::move(imageArtifacts));
    if (!nodeMetadata.empty()) {
        snapshot->nodes = std::make_shared<const std::vector<EditorModelNodePreview>>(std::move(nodeMetadata));
        snapshot->animationSkeleton = std::move(animationSkeleton);
    }
    snapshot->vertexBones = std::make_shared<const std::vector<animation::BoneIndex>>(
        std::move(geometry.vertexBones));
    snapshot->animations = std::make_shared<const std::vector<EditorModelAnimationPreview>>(std::move(animationMetadata));
    EditorModelPreviewResult result;
    result.generation = generation; result.sourceStamp = sourceStamp;
    result.path = path; result.snapshot = std::move(snapshot);
    return result;
}

EditorModelPreviewResult load_editor_gltf_preview(
    const FileSystemService& files, std::string_view relativePath,
    std::uint64_t generation, std::uint64_t sourceStamp,
    const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    const auto absolute = files.resolve_existing(path);
    if (absolute.empty()) return failed(path, generation, sourceStamp,
        "model path is outside the project or no longer exists");
    std::vector<std::uint8_t> source;
    std::string error;
    if (!read_file_bytes(absolute, kMaxSourceBytes, source, error))
        return failed(path, generation, sourceStamp, error);
    return load_editor_gltf_preview_source(files, path, source, generation, sourceStamp, cancel);
}

EditorModelPreviewResult load_editor_gltf_preview_bytes(
    const FileSystemService& files, std::string_view relativePath,
    const std::vector<std::uint8_t>& sourceBytes,
    std::uint64_t generation, std::uint64_t sourceStamp,
    const std::atomic_bool* cancel) {
    const std::string path(relativePath);
    if (sourceBytes.size() > kMaxSourceBytes)
        return failed(path, generation, sourceStamp, "model source bytes exceed the 128 MiB preview limit");
    return load_editor_gltf_preview_source(files, path, sourceBytes, generation, sourceStamp, cancel);
}

} // namespace shinkou::editor
