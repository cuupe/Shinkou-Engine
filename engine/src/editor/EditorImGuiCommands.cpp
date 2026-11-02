#include "EditorImGuiView.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <algorithm>
#include <cctype>
#include <imgui.h>

namespace shinkou::editor {
void EditorImGuiView::command_palette(EditorLayer &e, World &world) {
    if (e.commandSearchRequested_) {
        e.commandSearchRequested_ = false;
        commandSearch_.fill(0);
        commandSelection_ = 0;
        ImGui::OpenPopup("editor-command-search");
    }
    const auto *main = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({std::min(620 * scale_, main->WorkSize.x * .9f), 0},
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        {main->WorkPos.x + main->WorkSize.x * .5f, main->WorkPos.y + main->WorkSize.y * .18f},
        ImGuiCond_Appearing, {.5f, 0});
    if (!ImGui::BeginPopup("editor-command-search",
                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        return;
    ImGui::TextUnformatted(label(e, "搜索命令", "Command Search"));
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##command-search", label(e, "输入命令名称…", "Type a command…"),
                                 commandSearch_.data(), commandSearch_.size()))
        commandSelection_ = 0;
    region(e, "commands.search");
    commandMatches_.clear();
    const EditorLocalization localization(e.layout_.language);
    const auto matches = [&](std::string_view text) {
        const std::string_view query = commandSearch_.data();
        return std::search(text.begin(), text.end(), query.begin(), query.end(),
                           [](unsigned char a, unsigned char b) {
                               return std::tolower(a) == std::tolower(b);
                           }) != text.end();
    };
    for (const auto &action : e.actionRegistry_.actions()) {
        if (action.held)
            continue;
        if (matches(action.id) || matches(action.label) || matches(localization.text(action.label)))
            commandMatches_.push_back(&action);
    }
    if (!commandMatches_.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            ++commandSelection_;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            --commandSelection_;
        commandSelection_ =
            std::clamp(commandSelection_, 0, static_cast<int>(commandMatches_.size()) - 1);
    }
    std::string selected;
    if (ImGui::BeginChild("command-results", {0, 290 * scale_})) {
        for (std::size_t i = 0; i < commandMatches_.size(); ++i) {
            const auto &action = *commandMatches_[i];
            ImGui::PushID(action.id.c_str());
            const bool enabled = e.can_execute_action(action.id, world);
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Selectable(std::string(localization.text(action.label)).c_str(),
                                  static_cast<int>(i) == commandSelection_))
                selected = action.id;
            if (qaEnabled_)
                region(e, "command-result:" + action.id);
            if (static_cast<int>(i) == commandSelection_ &&
                (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                ImGui::SetScrollHereY();
            ImGui::SameLine();
            ImGui::TextDisabled("%s  %s", action.category.c_str(),
                                e.binding_label(action.id).c_str());
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (commandMatches_.empty())
            ImGui::TextDisabled("%s", label(e, "没有匹配的命令", "No matching commands"));
    }
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !commandMatches_.empty()) {
        const auto &action = *commandMatches_[commandSelection_];
        if (e.can_execute_action(action.id, world))
            selected = action.id;
    }
    if (!selected.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!selected.empty())
        e.execute_action(selected, world);
}
} // namespace shinkou::editor
#endif
