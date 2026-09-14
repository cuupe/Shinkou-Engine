#include "shinkou/editor/AssetPreview.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace shinkou::editor {
namespace {

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

bool is_one_of(std::string_view value, std::initializer_list<std::string_view> values) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

AssetPreviewKind AssetPreviewCatalog::classify(const FileEntry& entry) {
    return classify(entry.relativePath, entry.directory);
}

AssetPreviewKind AssetPreviewCatalog::classify(const std::filesystem::path& path,
                                               bool directory) {
    if (directory) return AssetPreviewKind::Folder;
    const auto extension = lower_extension(path);
    if (is_one_of(extension, {".txt", ".md", ".log", ".ini", ".cfg", ".json", ".yaml", ".yml",
                              ".xml", ".cmake", ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp",
                              ".hxx", ".cs", ".java", ".kt", ".lua", ".py", ".js", ".ts", ".rs",
                              ".go", ".sh", ".bat", ".cmd", ".ps1"})) {
        return AssetPreviewKind::Text;
    }
    if (is_one_of(extension, {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".webp", ".ico"})) {
        return AssetPreviewKind::Image;
    }
    if (is_one_of(extension, {".wav", ".ogg", ".mp3", ".flac", ".aiff", ".aif", ".m4a"})) {
        return AssetPreviewKind::Audio;
    }
    if (is_one_of(extension, {".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v"})) {
        return AssetPreviewKind::Video;
    }
    if (is_one_of(extension, {".obj", ".gltf", ".glb", ".fbx", ".dae", ".ply", ".stl", ".3ds", ".mesh"})) {
        return AssetPreviewKind::Model;
    }
    if (is_one_of(extension, {".scene", ".world", ".prefab"})) return AssetPreviewKind::Scene;
    if (is_one_of(extension, {".mat", ".material"})) return AssetPreviewKind::Material;
    if (is_one_of(extension, {".vert", ".frag", ".geom", ".comp", ".glsl", ".hlsl", ".wgsl", ".shader"})) {
        return AssetPreviewKind::Shader;
    }
    if (is_one_of(extension, {".ttf", ".otf", ".ttc", ".woff", ".woff2"})) return AssetPreviewKind::Font;
    if (is_one_of(extension, {".zip", ".7z", ".tar", ".gz", ".bz2", ".rar"})) return AssetPreviewKind::Archive;
    return extension.empty() ? AssetPreviewKind::Unknown : AssetPreviewKind::Binary;
}

std::string_view AssetPreviewCatalog::kind_name(AssetPreviewKind kind) noexcept {
    switch (kind) {
    case AssetPreviewKind::Unknown: return "Unknown";
    case AssetPreviewKind::Folder: return "Folder";
    case AssetPreviewKind::Text: return "Text";
    case AssetPreviewKind::Image: return "Texture";
    case AssetPreviewKind::Audio: return "Audio";
    case AssetPreviewKind::Video: return "Video";
    case AssetPreviewKind::Model: return "Mesh";
    case AssetPreviewKind::Scene: return "Scene";
    case AssetPreviewKind::Material: return "Material";
    case AssetPreviewKind::Shader: return "Shader";
    case AssetPreviewKind::Font: return "Font";
    case AssetPreviewKind::Archive: return "Archive";
    case AssetPreviewKind::Binary: return "File";
    }
    return "Unknown";
}

std::string_view AssetPreviewCatalog::mime_type(AssetPreviewKind kind,
                                                std::string_view extension) noexcept {
    switch (kind) {
    case AssetPreviewKind::Text: return "text/plain";
    case AssetPreviewKind::Image:
        if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
        if (extension == ".png") return "image/png";
        if (extension == ".webp") return "image/webp";
        if (extension == ".gif") return "image/gif";
        return "image/*";
    case AssetPreviewKind::Audio:
        if (extension == ".wav") return "audio/wav";
        if (extension == ".mp3") return "audio/mpeg";
        if (extension == ".ogg") return "audio/ogg";
        return "audio/*";
    case AssetPreviewKind::Video:
        if (extension == ".mp4" || extension == ".m4v") return "video/mp4";
        if (extension == ".webm") return "video/webm";
        return "video/*";
    case AssetPreviewKind::Model:
        if (extension == ".gltf") return "model/gltf+json";
        if (extension == ".glb") return "model/gltf-binary";
        return "model/*";
    case AssetPreviewKind::Font: return "font/*";
    case AssetPreviewKind::Archive:
        if (extension == ".zip") return "application/zip";
        if (extension == ".7z") return "application/x-7z-compressed";
        if (extension == ".tar") return "application/x-tar";
        if (extension == ".gz") return "application/gzip";
        return "application/octet-stream";
    case AssetPreviewKind::Folder: return "inode/directory";
    default: return "application/octet-stream";
    }
}

std::string_view AssetPreviewCatalog::preview_title(AssetPreviewKind kind) noexcept {
    switch (kind) {
    case AssetPreviewKind::Folder: return "Folder Contents";
    case AssetPreviewKind::Text: return "Text Resource";
    case AssetPreviewKind::Image: return "Texture Preview";
    case AssetPreviewKind::Audio: return "Audio Preview";
    case AssetPreviewKind::Video: return "Video Preview";
    case AssetPreviewKind::Model: return "Mesh Preview";
    case AssetPreviewKind::Scene: return "Scene Document";
    case AssetPreviewKind::Material: return "Material Properties";
    case AssetPreviewKind::Shader: return "Shader Resource";
    case AssetPreviewKind::Font: return "Font Preview";
    case AssetPreviewKind::Archive: return "Archive Contents";
    case AssetPreviewKind::Unknown:
    case AssetPreviewKind::Binary: return "File Details";
    }
    return "File Details";
}

AssetPreviewDescriptor AssetPreviewCatalog::describe(const FileEntry& entry) const {
    AssetPreviewDescriptor result;
    result.relativePath = entry.relativePath;
    result.displayName = entry.name.empty() ? entry.relativePath.filename().string() : entry.name;
    result.extension = lower_extension(entry.relativePath);
    result.kind = classify(entry);
    result.mimeType = std::string(mime_type(result.kind, result.extension));
    result.size = entry.size;
    result.directory = entry.directory;
    result.previewTitle = std::string(preview_title(result.kind));
    result.capabilities = AssetPreviewCapability::Metadata;
    if (result.kind == AssetPreviewKind::Folder) {
        result.capabilities = result.capabilities | AssetPreviewCapability::Interactive;
        result.statusMessage = "Folder navigation is available";
    } else {
        result.statusMessage = "Metadata only; preview provider not registered";
    }
    return result;
}

} // namespace shinkou::editor
