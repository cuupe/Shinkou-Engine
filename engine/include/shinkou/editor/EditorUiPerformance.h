#pragma once
#include <cstddef>
#include <cstdint>
namespace shinkou::editor {
struct EditorUiPerformance {
    std::size_t hierarchyRows{0};
    std::size_t hierarchySubmitted{0};
    std::uint64_t hierarchyRebuilds{0};
    std::size_t qaRegions{0};
    bool qaEnabled{false};
};
} // namespace shinkou::editor
