#pragma once

#include "shinkou/ui/Render.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shinkou::editor {

// Semantic asset kinds are independent of a platform file icon. The editor
// owns this mapping so SVG assets can be themed and replaced without changing
// the resource browser layout.
enum class AssetIconKind : std::uint8_t {
    Folder,
    Image,
    Mesh,
    Scene,
    Material,
    Audio,
    Video,
    Script,
    Font,
    Archive,
    File,
};

// Loads editor icon SVGs once and replays their normalized vector paths into
// the retained render list. No filesystem access happens from paint(). The
// built-in SVGs are deliberately a small supported subset (path commands
// M/L/H/V/Z), which keeps the runtime portable and deterministic.
class AssetIconLibrary final {
public:
    AssetIconLibrary() = default;

    bool initialize(std::filesystem::path preferredRoot = {});
    void clear() noexcept;

    bool ready() const noexcept { return loadedCount_ > 0; }
    std::size_t loaded_count() const noexcept { return loadedCount_; }
    const std::filesystem::path& root() const noexcept { return root_; }

    // Returns false only when the requested SVG is unavailable. Callers can
    // use a text marker as a last-resort fallback for a damaged installation.
    bool paint(ui::UiRenderList& output, ui::Rect bounds, AssetIconKind kind,
               ui::ThemeColor accent, ui::ThemeColor ink,
               ui::ThemeColor paper) const;

private:
    enum class ColorRole : std::uint8_t { Accent, Ink, Paper };
    struct Path {
        std::shared_ptr<const std::vector<ui::Vec2>> points;
        ColorRole role{ColorRole::Accent};
        bool closed{true};
    };
    struct Icon {
        std::vector<Path> paths;
    };

    std::unordered_map<AssetIconKind, Icon> icons_;
    std::filesystem::path root_{};
    std::size_t loadedCount_{0};

    bool load_directory(const std::filesystem::path& root);
    static bool parse_svg(std::string_view source, Icon& icon);
    static bool parse_path(std::string_view data, std::vector<ui::Vec2>& points, bool& closed);
    static std::string file_name(AssetIconKind kind);
};

} // namespace shinkou::editor
