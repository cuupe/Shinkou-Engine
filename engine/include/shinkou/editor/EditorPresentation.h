#pragma once

#include <cstdint>

namespace shinkou::editor {
// Editor concepts shared by controllers and the ImGui view. These types do
// not imply a second widget tree or a renderer-specific presentation path.
enum class EditorAssetAction : std::uint8_t { Open, Navigate, Refresh, Rename, NewFolder, Delete };
enum class EditorAssetView : std::uint8_t { SmallList, LargeIcons, Tree, List = SmallList };
enum class ViewportNavigation : std::uint8_t { Orbit, Pan, Zoom };
} // namespace shinkou::editor
