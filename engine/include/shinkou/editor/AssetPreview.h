#pragma once

#include "shinkou/editor/FileSystem.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace shinkou::editor {

enum class AssetPreviewKind : std::uint8_t {
    Unknown,
    Folder,
    Text,
    Image,
    Audio,
    Video,
    Model,
    Scene,
    Material,
    Shader,
    Font,
    Archive,
    Binary,
};

enum class AssetPreviewCapability : std::uint8_t {
    None = 0,
    Metadata = 1u << 0u,
    Thumbnail = 1u << 1u,
    Interactive = 1u << 2u,
};

constexpr AssetPreviewCapability operator|(AssetPreviewCapability left,
                                           AssetPreviewCapability right) noexcept {
    return static_cast<AssetPreviewCapability>(static_cast<std::uint8_t>(left) |
                                               static_cast<std::uint8_t>(right));
}

constexpr bool has_capability(AssetPreviewCapability value,
                              AssetPreviewCapability capability) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(capability)) != 0;
}

struct AssetPreviewDescriptor {
    std::filesystem::path relativePath;
    std::string displayName;
    std::string extension;
    std::string mimeType;
    AssetPreviewKind kind{AssetPreviewKind::Unknown};
    AssetPreviewCapability capabilities{AssetPreviewCapability::None};
    std::uintmax_t size{0};
    bool directory{false};
    std::string previewTitle;
    std::string statusMessage;

    bool supports(AssetPreviewCapability capability) const noexcept {
        return has_capability(capabilities, capability);
    }
};

// Pure, deterministic resource classification. It owns no file handles and
// performs no IO, so descriptors can be created on a worker and consumed by
// the retained UI without touching the paint path.
class AssetPreviewCatalog final {
public:
    AssetPreviewDescriptor describe(const FileEntry& entry) const;

    static AssetPreviewKind classify(const FileEntry& entry);
    static AssetPreviewKind classify(const std::filesystem::path& path,
                                     bool directory = false);
    static std::string_view kind_name(AssetPreviewKind kind) noexcept;
    static std::string_view mime_type(AssetPreviewKind kind,
                                      std::string_view extension) noexcept;
    static std::string_view preview_title(AssetPreviewKind kind) noexcept;
};

} // namespace shinkou::editor
