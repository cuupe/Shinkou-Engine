#pragma once

#include "shinkou/GameObject.h"
#include "shinkou/assets/AssetSystem.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace shinkou::editor {

// A scene-safe bridge between the editor asset browser and a GameObject. The
// project-relative path remains readable for migration/debugging while the
// manifest-backed AssetId provides the stable runtime identity. The component
// owns no file handles, decoder state, or backend resources.
class AssetReferenceComponent final : public Component {
    std::string path_;
    assets::AssetId assetId_{0};

public:
    explicit AssetReferenceComponent(std::string path = {}, assets::AssetId assetId = 0) {
        if (set_path(std::move(path)) && !path_.empty()) assetId_ = assetId;
    }

    std::string_view type_name() const noexcept override { return "AssetReference"; }
    const std::string& path() const noexcept { return path_; }
    assets::AssetId asset_id() const noexcept { return assetId_; }

    bool set_path(std::string path) {
        if (path.empty()) {
            path_.clear();
            assetId_ = 0;
            return true;
        }
        const auto normalized = std::filesystem::path(path).lexically_normal();
        if (normalized.empty() || normalized.is_absolute() || normalized == ".." ||
            normalized.generic_string().rfind("../", 0) == 0) {
            return false;
        }
        if (path_ != normalized.generic_string()) assetId_ = 0;
        path_ = normalized.generic_string();
        return true;
    }

    bool set_asset_id(assets::AssetId assetId) noexcept {
        assetId_ = assetId;
        return true;
    }

protected:
    void define_properties(PropertyBuilder& builder) override {
        builder.add_custom("path", PropertyType::String,
            [this] { return PropertyValue{path_}; },
            [this](const PropertyValue& value) {
                const auto* text = std::get_if<std::string>(&value);
                return text != nullptr && set_path(*text);
            }, PropertyFlags::Serialized);
        builder.add_custom("assetId", PropertyType::UnsignedInteger,
            [this] { return PropertyValue{static_cast<std::uint64_t>(assetId_)}; },
            [this](const PropertyValue& value) {
                const auto* id = std::get_if<std::uint64_t>(&value);
                return id != nullptr && set_asset_id(static_cast<assets::AssetId>(*id));
            }, PropertyFlags::Serialized, 0.0, 0.0, 0.0, "Asset ID");
    }
};

} // namespace shinkou::editor
