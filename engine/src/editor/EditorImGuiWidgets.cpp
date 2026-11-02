#include "EditorImGuiWidgets.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <algorithm>
#include <array>
#include <cstring>
#include <imgui.h>
#include <vector>

namespace shinkou::editor::widgets {
namespace {
EditEvent interaction(bool changed) {
    return {changed,
            ImGui::IsItemActivated(),
            ImGui::IsItemDeactivated(),
            false,
            ImGui::IsKeyPressed(ImGuiKey_Escape),
            false};
}
} // namespace
EditEvent text_input(const char *id, std::string &value, const char *hint, int flags,
                     std::size_t limit) {
    std::array<char, 4097> local{};
    std::vector<char> large;
    if (limit >= local.size())
        large.resize(limit + 1);
    char *buffer = large.empty() ? local.data() : large.data();
    std::copy_n(value.data(), std::min(value.size(), limit), buffer);
    const bool submitted = ImGui::InputTextWithHint(id, hint, buffer, limit + 1, flags);
    auto event = interaction(ImGui::IsItemEdited());
    event.submitted = submitted;
    if (event.changed)
        value.assign(buffer);
    return event;
}
bool checkbox(const char *label, bool &value) {
    return ImGui::Checkbox(label, &value);
}
void clipped_text(const char *text, float width) {
    const auto start = ImGui::GetCursorScreenPos();
    const auto end = ImVec2(start.x + std::max(1.0f, width), start.y + ImGui::GetTextLineHeight());
    ImGui::PushClipRect(start, end, true);
    ImGui::TextUnformatted(text);
    ImGui::PopClipRect();
}
bool slider(const char *label, float &value, float minimum, float maximum, const char *format) {
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.56f));
    return ImGui::SliderFloat(label, &value, minimum, maximum, format);
}
void vector_input(float *values, int count, const VectorInput &options) {
    if (!values || count < 1 || count > 4)
        return;
    const float available = ImGui::GetContentRegionAvail().x;
    const bool stacked = available < count * 48 * options.scale;
    const float gap = 3 * options.scale;
    const float width = stacked ? available : (available - gap * (count - 1)) / count;
    const char *axes[]{"X", "Y", "Z", "W"};
    const ImVec4 colors[]{{0.74f, 0.27f, 0.28f, 1},
                          {0.24f, 0.60f, 0.38f, 1},
                          {0.25f, 0.48f, 0.78f, 1},
                          {0.55f, 0.48f, 0.68f, 1}};
    ImGui::BeginGroup();
    for (int axis = 0; axis < count; ++axis) {
        if (axis && !stacked)
            ImGui::SameLine(0, gap);
        ImGui::PushID(axis);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, colors[axis]);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        const bool reset = ImGui::Button(axes[axis], {17 * options.scale, ImGui::GetFrameHeight()});
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %s", options.resetLabel, axes[axis]);
        ImGui::PopStyleColor(2);
        if (reset)
            values[axis] = options.resetValues[axis];
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(std::max(18 * options.scale, width - 17 * options.scale));
        const bool changed =
            ImGui::DragFloat("##axis", &values[axis], options.speed, options.minimum,
                             options.maximum, "%.3f", ImGuiSliderFlags_NoRoundToFormat);
        auto event = interaction(changed);
        event.reset = reset;
        if (options.onAxis)
            options.onAxis(axis, event);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndGroup();
}
} // namespace shinkou::editor::widgets
#endif
