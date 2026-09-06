#include "shinkou/editor/AssetIconLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace shinkou::editor {
namespace {

std::string attribute(std::string_view tag, std::string_view name) {
    std::size_t cursor = 0;
    while ((cursor = tag.find(name, cursor)) != std::string_view::npos) {
        const bool boundary = cursor == 0 || std::isspace(static_cast<unsigned char>(tag[cursor - 1])) != 0;
        if (!boundary) { cursor += name.size(); continue; }
        cursor += name.size();
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) ++cursor;
        if (cursor >= tag.size() || tag[cursor] != '=') continue;
        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) ++cursor;
        if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\'')) return {};
        const char quote = tag[cursor++];
        const auto end = tag.find(quote, cursor);
        return end == std::string_view::npos ? std::string{} : std::string(tag.substr(cursor, end - cursor));
    }
    return {};
}

bool number(std::string_view text, std::size_t& cursor, float& value) {
    while (cursor < text.size() && (std::isspace(static_cast<unsigned char>(text[cursor])) != 0 || text[cursor] == ',')) ++cursor;
    if (cursor >= text.size()) return false;
    const auto begin = cursor;
    if (text[cursor] == '+' || text[cursor] == '-') ++cursor;
    bool digits = false;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
        digits = true;
        ++cursor;
    }
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
            digits = true;
            ++cursor;
        }
    }
    if (!digits) return false;
    if (cursor < text.size() && (text[cursor] == 'e' || text[cursor] == 'E')) {
        const auto exponent = cursor++;
        if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) ++cursor;
        const auto exponentDigits = cursor;
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) ++cursor;
        if (exponentDigits == cursor) cursor = exponent;
    }
    try {
        value = std::stof(std::string(text.substr(begin, cursor - begin)));
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::filesystem::path> search_roots(const std::filesystem::path& preferred) {
    std::vector<std::filesystem::path> roots;
    if (!preferred.empty()) roots.push_back(preferred);
    if (const char* overrideRoot = std::getenv("SHINKOU_EDITOR_ICON_ROOT")) {
        if (*overrideRoot != '\0') roots.emplace_back(overrideRoot);
    }
#if defined(SHINKOU_EDITOR_ICON_SOURCE_DIR)
    roots.emplace_back(SHINKOU_EDITOR_ICON_SOURCE_DIR);
#endif
#if defined(SHINKOU_EDITOR_ICON_INSTALL_DIR)
    roots.emplace_back(SHINKOU_EDITOR_ICON_INSTALL_DIR);
#endif
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (!error) {
        roots.push_back(current / "resources" / "editor" / "icons");
        roots.push_back(current / "engine" / "resources" / "editor" / "icons");
    }
    return roots;
}

} // namespace

std::string AssetIconLibrary::file_name(AssetIconKind kind) {
    switch (kind) {
    case AssetIconKind::Folder: return "folder.svg";
    case AssetIconKind::Image: return "image.svg";
    case AssetIconKind::Mesh: return "mesh.svg";
    case AssetIconKind::Scene: return "scene.svg";
    case AssetIconKind::Material: return "material.svg";
    case AssetIconKind::Audio: return "audio.svg";
    case AssetIconKind::Video: return "video.svg";
    case AssetIconKind::Script: return "script.svg";
    case AssetIconKind::Font: return "font.svg";
    case AssetIconKind::Archive: return "archive.svg";
    case AssetIconKind::File: return "file.svg";
    }
    return "file.svg";
}

bool AssetIconLibrary::initialize(std::filesystem::path preferredRoot) {
    clear();
    for (const auto& root : search_roots(preferredRoot)) {
        if (load_directory(root)) return true;
    }
    return false;
}

void AssetIconLibrary::clear() noexcept {
    icons_.clear();
    root_.clear();
    loadedCount_ = 0;
}

bool AssetIconLibrary::load_directory(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return false;
    std::unordered_map<AssetIconKind, Icon> loaded;
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(AssetIconKind::File); ++raw) {
        const auto kind = static_cast<AssetIconKind>(raw);
        std::ifstream file(root / file_name(kind), std::ios::binary);
        if (!file) continue;
        std::ostringstream content;
        content << file.rdbuf();
        Icon icon;
        if (parse_svg(content.str(), icon) && !icon.paths.empty()) loaded.emplace(kind, std::move(icon));
    }
    if (loaded.empty()) return false;
    icons_ = std::move(loaded);
    root_ = root;
    loadedCount_ = icons_.size();
    return true;
}

bool AssetIconLibrary::parse_svg(std::string_view source, Icon& icon) {
    const auto svgStart = source.find("<svg");
    const auto svgEnd = source.find('>', svgStart);
    if (svgStart == std::string_view::npos || svgEnd == std::string_view::npos) return false;
    const auto svgTag = source.substr(svgStart, svgEnd - svgStart + 1);
    float viewX = 0.0f;
    float viewY = 0.0f;
    float viewWidth = 24.0f;
    float viewHeight = 24.0f;
    std::istringstream viewBox(attribute(svgTag, "viewBox"));
    viewBox >> viewX >> viewY >> viewWidth >> viewHeight;
    if (!(viewWidth > 0.0f && viewHeight > 0.0f)) return false;

    std::size_t cursor = 0;
    while ((cursor = source.find("<path", cursor)) != std::string_view::npos) {
        const auto end = source.find('>', cursor);
        if (end == std::string_view::npos) break;
        const auto tag = source.substr(cursor, end - cursor + 1);
        const auto data = attribute(tag, "d");
        const auto roleName = attribute(tag, "data-role");
        ColorRole role = ColorRole::Accent;
        if (roleName == "ink") role = ColorRole::Ink;
        else if (roleName == "paper") role = ColorRole::Paper;
        // A single SVG <path> may contain multiple closed subpaths. Split
        // them at M/m so the retained Path command does not accidentally
        // connect unrelated contours when creating a D2D geometry.
        std::vector<std::string_view> subpaths;
        std::size_t subpathStart = 0;
        for (std::size_t index = 0; index < data.size(); ++index) {
            if ((data[index] == 'M' || data[index] == 'm') && index > subpathStart) {
                subpaths.push_back(std::string_view(data).substr(subpathStart, index - subpathStart));
                subpathStart = index;
            }
        }
        if (!data.empty()) subpaths.push_back(std::string_view(data).substr(subpathStart));
        for (const auto subpath : subpaths) {
            std::vector<ui::Vec2> points;
            bool closed = true;
            if (!parse_path(subpath, points, closed) || points.size() < 2) continue;
            for (auto& point : points) {
                point.x = (point.x - viewX) / viewWidth;
                point.y = (point.y - viewY) / viewHeight;
            }
            icon.paths.push_back({std::make_shared<const std::vector<ui::Vec2>>(std::move(points)), role, closed});
        }
        cursor = end + 1;
    }
    return !icon.paths.empty();
}

bool AssetIconLibrary::parse_path(std::string_view data, std::vector<ui::Vec2>& points, bool& closed) {
    std::size_t cursor = 0;
    char command = 0;
    bool firstMove = true;
    ui::Vec2 current{};
    closed = false;
    while (cursor < data.size()) {
        while (cursor < data.size() && (std::isspace(static_cast<unsigned char>(data[cursor])) != 0 || data[cursor] == ',')) ++cursor;
        if (cursor >= data.size()) break;
        if (std::isalpha(static_cast<unsigned char>(data[cursor])) != 0) {
            command = data[cursor++];
            if (command == 'z' || command == 'Z') {
                closed = true;
                command = 0;
            }
            if (command == 0) continue;
        }
        if (command == 0) return false;
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        float first = 0.0f;
        float second = 0.0f;
        if (upper == 'M' || upper == 'L') {
            if (!number(data, cursor, first) || !number(data, cursor, second)) return false;
            ui::Vec2 next{first, second};
            if (relative) { next.x += current.x; next.y += current.y; }
            current = next;
            if (upper == 'M' && firstMove) {
                points.push_back(current);
                firstMove = false;
                command = relative ? 'l' : 'L';
            } else {
                points.push_back(current);
            }
        } else if (upper == 'H' || upper == 'V') {
            if (!number(data, cursor, first)) return false;
            if (upper == 'H') current.x = relative ? current.x + first : first;
            else current.y = relative ? current.y + first : first;
            points.push_back(current);
        } else {
            // The built-in asset set intentionally uses only commands which
            // map cleanly to deterministic polygon geometry.
            return false;
        }
    }
    return points.size() >= 2;
}

bool AssetIconLibrary::paint(ui::UiRenderList& output, ui::Rect bounds, AssetIconKind kind,
                             ui::ThemeColor accent, ui::ThemeColor ink, ui::ThemeColor paper) const {
    const auto found = icons_.find(kind);
    if (found == icons_.end()) return false;
    for (const auto& path : found->second.paths) {
        const auto fill = path.role == ColorRole::Ink ? ink : path.role == ColorRole::Paper ? paper : accent;
        output.path(bounds, path.points, fill, {}, 0.0f, path.closed, true);
    }
    return true;
}

} // namespace shinkou::editor
