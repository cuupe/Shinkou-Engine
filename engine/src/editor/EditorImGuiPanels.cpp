#include "EditorGlyphs.h"
#include "EditorImGuiView.h"
#include "EditorImGuiWidgets.h"
#include "shinkou/components/PrimitiveMeshComponent.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/profile/Profiler.h"
#include "shinkou/ui/Performance.h"
#if defined(SHINKOU_WITH_IMGUI)
#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderScene.h"
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <sstream>

namespace shinkou::editor {
void EditorImGuiView::object_row(EditorLayer &e, const EditorHierarchyRow &row) {
    const auto &node = *row.node;
    ImGui::PushID(static_cast<int>(node.id));
    ImGui::PushID(static_cast<int>(node.id >> 32));
    const float indent = static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing;
    if (indent)
        ImGui::Indent(indent);
    auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                 ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (node.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (node.selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::SetNextItemOpen(hierarchyRows_.expanded(node.id));
    if (!node.active)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    const bool open = ImGui::TreeNodeEx(
        "object", flags, "%s  %s",
        iconsReady_ ? (node.children.empty() ? glyph::Object : glyph::Folder) : "",
        node.name.c_str());
    ++performance_.hierarchySubmitted;
    if (qaEnabled_) {
        const auto id = "hierarchy:" + std::to_string(node.id);
        region(e, id);
        alias_region("hierarchy-name:" + node.name, id);
    }
    if (ImGui::IsItemToggledOpen())
        hierarchyRows_.set_expanded(node.id, open);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        e.select_object(node.id, *e.activeWorld_);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", node.name.c_str());
    if (ImGui::BeginPopupContextItem("object-actions")) {
        e.select_object(node.id, *e.activeWorld_);
        if (ImGui::MenuItem(label(e, "聚焦对象", "Frame selection"), "F"))
            e.dispatch_command(EditorCommand::FrameSelection, {}, *e.activeWorld_);
        if (ImGui::MenuItem(label(e, "创建子对象", "Create child")))
            e.dispatch_command(EditorCommand::CreateChild, {}, *e.activeWorld_);
        ImGui::Separator();
        if (ImGui::MenuItem(label(e, "删除对象", "Delete object"), "Delete"))
            e.dispatch_command(EditorCommand::DeleteObject, {}, *e.activeWorld_);
        ImGui::EndPopup();
    }
    if (!node.active)
        ImGui::PopStyleVar();
    if (indent)
        ImGui::Unindent(indent);
    ImGui::PopID();
    ImGui::PopID();
}
void EditorImGuiView::hierarchy(EditorLayer &e, World &world) {
    SHINKOU_PROFILE_SCOPE("Editor.UI.Hierarchy");
    if (!begin_panel(e, "hierarchy"))
        return;
    ImGui::SetNextItemWidth(std::max(40 * scale_, ImGui::GetContentRegionAvail().x -
                                                      ImGui::GetFrameHeight() -
                                                      ImGui::GetStyle().ItemSpacing.x));
    auto filter = e.uiModel_.object_filter();
    if (widgets::text_input("##filter", filter, label(e, "搜索场景对象", "Search objects")).changed)
        e.uiModel_.set_object_filter(std::move(filter));
    region(e, "hierarchy.filter");
    ImGui::SameLine();
    if (button(e, "+ 创建对象", "+ Create object", "command:create-empty"))
        e.dispatch_command(EditorCommand::CreateEmpty, {}, world);
    ImGui::Separator();
    hierarchyRows_.refresh(e.uiModel_.object_roots(), e.uiModel_.hierarchy_revision(),
                           !e.uiModel_.object_filter().empty());
    const auto &style = ImGui::GetStyle();
    const float fontSize = ImGui::GetFontSize();
    if (hierarchyWidthWorld_ != world.instance_id() ||
        hierarchyWidthRevision_ != world.hierarchy_revision() ||
        hierarchyWidthFontSize_ != fontSize || hierarchyWidthIndent_ != style.IndentSpacing) {
        hierarchyContentWidth_ = 1;
        const float icons = iconsReady_ ? std::max(ImGui::CalcTextSize(glyph::Object).x,
                                                   ImGui::CalcTextSize(glyph::Folder).x)
                                        : 0;
        const float padding =
            fontSize + 2 * style.FramePadding.x + icons + ImGui::CalcTextSize("  ").x;
        const auto measure = [&](const auto &self, const GameObject &object,
                                 std::size_t depth) -> void {
            const auto name = object.name();
            hierarchyContentWidth_ =
                std::max(hierarchyContentWidth_,
                         static_cast<float>(depth) * style.IndentSpacing + padding +
                             ImGui::CalcTextSize(name.data(), name.data() + name.size()).x);
            for (const auto &child : object.children())
                if (child && !child->destroy_requested())
                    self(self, *child, depth + 1);
        };
        world.each_object([&](const GameObject &object) { measure(measure, object, 0); });
        // Keep a stable horizontal extent while filtering/collapsing. Besides
        // preventing scrollbar layout shifts this avoids remeasuring N labels
        // on every search keystroke.
        hierarchyWidthWorld_ = world.instance_id();
        hierarchyWidthRevision_ = world.hierarchy_revision();
        hierarchyWidthFontSize_ = fontSize;
        hierarchyWidthIndent_ = style.IndentSpacing;
    }
    // A virtual list must supply its full horizontal extent as well. Measuring
    // only visible labels would create a scrollbar late, shrink the viewport
    // and hide the last row immediately after an End-key jump.
    ImGui::SetNextWindowContentSize({hierarchyContentWidth_, 0});
    // Keep search/create controls reachable when the virtual list scrolls.
    if (!ImGui::BeginChild("hierarchy-rows", {}, 0, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    // Theme borders can put the first row on a half pixel. ItemSize snaps
    // subsequent rows, so align the first row before measuring its advance.
    auto start = ImGui::GetCursorScreenPos();
    start.y = std::floor(start.y);
    ImGui::SetCursorScreenPos(start);
    // A flattened tree has no ImGui TreePush stack for its navigation search.
    // Route list-edge scrolling explicitly instead of estimating a clipped
    // final row's navigation rectangle. Text fields retain their own keys.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive()) {
        const bool home = ImGui::IsKeyPressed(ImGuiKey_Home, false);
        const bool end = ImGui::IsKeyPressed(ImGuiKey_End, false);
        if (home || end) {
            ImGui::NavMoveRequestCancel();
            ImGui::SetScrollY(end ? ImGui::GetScrollMaxY() : 0);
        }
    }
    performance_.hierarchyRows = hierarchyRows_.rows().size();
    performance_.hierarchyRebuilds = hierarchyRows_.rebuilds();
    ImGuiListClipper clipper;
    // Measure actual cursor advance: fractional DPI pixel snapping can differ
    // from the nominal font/spacing sum and otherwise accumulate over N rows.
    clipper.Begin(static_cast<int>(hierarchyRows_.rows().size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            object_row(e, hierarchyRows_.rows()[i]);
    // Production shortcuts follow ImGui focus, independently of QA records.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        route_actions(e, world, EditorInputContext::Hierarchy);
    if (ImGui::BeginPopupContextWindow("objects-context", ImGuiPopupFlags_MouseButtonRight |
                                                              ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(label(e, "创建空对象", "Create empty")))
            e.dispatch_command(EditorCommand::CreateEmpty, {}, world);
        if (ImGui::BeginMenu(label(e, "创建 3D 对象", "Create 3D object"))) {
            for (const auto *shape : {"Cube", "Sphere", "Plane"})
                if (ImGui::MenuItem(shape == std::string_view("Cube") ? label(e, "立方体", "Cube")
                                    : shape == std::string_view("Sphere")
                                        ? label(e, "球体", "Sphere")
                                        : label(e, "平面", "Plane")))
                    e.dispatch_command(EditorCommand::Create3DObject, shape, world);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}
void EditorImGuiView::assets(EditorLayer &e) {
    if (!begin_panel(e, "assets"))
        return;
    if (button(e, "上一级", "Up", "asset-parent"))
        e.set_asset_directory(e.assetBrowser_.directory.parent_path());
    ImGui::SameLine();
    if (button(e, "刷新", "Refresh", "asset-refresh"))
        e.handle_asset_action(EditorAssetAction::Refresh, {}, {});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
    widgets::text_input("##filter", e.assetBrowser_.filter,
                        label(e, "搜索资源（包含子目录）", "Search assets (including subfolders)"));
    region(e, "assets.filter");
    ImGui::TextDisabled("%s", e.assetBrowser_.directory.empty()
                                  ? "/"
                                  : e.assetBrowser_.directory.generic_string().c_str());
    ImGui::SameLine();
    int mode = static_cast<int>(e.assetBrowser_.view);
    ImGui::SetNextItemWidth(100 * scale_);
    if (ImGui::Combo("##asset-view", &mode,
                     label(e, "列表\0大图标\0目录树\0", "List\0Icons\0Tree\0")))
        e.set_asset_view(static_cast<EditorAssetView>(mode));
    region(e, "assets.view");
    e.assetBrowser_.refresh(e.projectFiles_, e.projectFilesRevision_);
    const auto &rows = e.assetBrowser_.rows();
    visibleAssets_ = rows.size();
    std::string toggle;
    const auto item = [&](const EditorAssetBrowserRow &row, bool icons) {
        const auto &entry = e.projectFiles_[row.sourceIndex];
        const auto path = entry.relativePath.generic_string();
        ImGui::PushID(path.c_str());
        if (!icons && row.depth)
            ImGui::Indent(row.depth * 16 * scale_);
        if (!icons && entry.directory && e.assetBrowser_.view == EditorAssetView::Tree &&
            e.assetBrowser_.filter.empty()) {
            if (ImGui::ArrowButton("expand",
                                   e.assetBrowser_.expanded(path) ? ImGuiDir_Down : ImGuiDir_Right))
                toggle = path;
            if (qaEnabled_)
                region(e, "asset-expand:" + path);
            ImGui::SameLine();
        }
        const std::string name =
            std::string(iconsReady_ ? (entry.directory ? glyph::Folder : glyph::Code) : "") +
            (iconsReady_ ? "  " : "") + entry.name;
        const auto flags = ImGuiSelectableFlags_AllowDoubleClick |
                           (icons ? 0 : ImGuiSelectableFlags_SpanAllColumns);
        if (ImGui::Selectable(name.c_str(), e.assetBrowser_.selection == path, flags,
                              icons ? ImVec2(0, 64 * scale_) : ImVec2())) {
            e.set_selected_asset(path);
            if (!entry.directory) {
                e.set_panel_visible("inspector", true);
                focusPanel_ = "inspector";
            }
            if (ImGui::IsMouseDoubleClicked(0))
                e.handle_asset_action(entry.directory ? EditorAssetAction::Navigate
                                                      : EditorAssetAction::Open,
                                      path, {});
        }
        if (qaEnabled_)
            region(e, "asset:" + path);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", path.c_str());
        if (!entry.directory && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SHINKOU_ASSET", path.c_str(), path.size() + 1);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("asset-context")) {
            if (ImGui::MenuItem(label(e, "打开", "Open")))
                e.handle_asset_action(entry.directory ? EditorAssetAction::Navigate
                                                      : EditorAssetAction::Open,
                                      path, {});
            if (ImGui::MenuItem(label(e, "重命名…", "Rename…"))) {
                actionPath_ = path;
                actionKind_ = "rename";
                std::strncpy(rename_.data(), entry.name.c_str(), rename_.size() - 1);
                rename_.back() = 0;
            }
            if (ImGui::MenuItem(label(e, "移到回收区…", "Move to recovery…"))) {
                actionPath_ = path;
                actionKind_ = "delete";
            }
            if (entry.directory && ImGui::MenuItem(label(e, "新建文件夹…", "New folder…"))) {
                actionPath_ = path;
                actionKind_ = "folder";
                rename_.fill(0);
            }
            ImGui::EndPopup();
        }
        if (!icons && row.depth)
            ImGui::Unindent(row.depth * 16 * scale_);
        ImGui::PopID();
    };
    const bool icons = e.assetBrowser_.view == EditorAssetView::LargeIcons;
    const int columns =
        icons
            ? std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / (150 * scale_)), 1, 12)
            : 2;
    if (ImGui::BeginTable(
            "asset-list", columns,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                (icons ? 0 : ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))) {
        if (!icons) {
            ImGui::TableSetupColumn(label(e, "名称", "Name"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(label(e, "大小", "Size"), ImGuiTableColumnFlags_WidthFixed,
                                    85 * scale_);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
        }
        ImGuiListClipper clipper;
        clipper.Begin(
            static_cast<int>((rows.size() + (icons ? columns : 1) - 1) / (icons ? columns : 1)));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::TableNextRow(0, icons ? 70 * scale_ : ImGui::GetFrameHeight());
                if (icons) {
                    for (int c = 0; c < columns; ++c) {
                        const auto index = static_cast<std::size_t>(i * columns + c);
                        if (index >= rows.size())
                            break;
                        ImGui::TableSetColumnIndex(c);
                        item(rows[index], true);
                    }
                } else {
                    ImGui::TableNextColumn();
                    item(rows[i], false);
                    ImGui::TableNextColumn();
                    const auto &entry = e.projectFiles_[rows[i].sourceIndex];
                    if (entry.directory)
                        ImGui::TextDisabled("—");
                    else
                        ImGui::TextDisabled("%.1f KB", entry.size / 1024.0);
                }
            }
        assetScroll_ = ImGui::GetScrollY() / std::max(scale_, 0.5f);
        ImGui::EndTable();
    }
    if (!toggle.empty())
        e.assetBrowser_.toggle_directory(toggle);
    if (ImGui::BeginPopupContextWindow("asset-background", ImGuiPopupFlags_MouseButtonRight |
                                                               ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(label(e, "新建文件夹…", "New folder…"))) {
            actionPath_ = e.assetBrowser_.directory.generic_string();
            actionKind_ = "folder";
            rename_.fill(0);
        }
        ImGui::EndPopup();
    }
    if (!actionKind_.empty())
        ImGui::OpenPopup("asset-operation");
    if (ImGui::BeginPopupModal("asset-operation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", actionPath_.c_str());
        const bool deleting = actionKind_ == "delete";
        if (deleting)
            ImGui::TextWrapped("%s",
                               label(e, "资源将移入项目回收区，可通过撤销恢复。",
                                     "The asset will move to project recovery and can be undone."));
        else {
            ImGui::SetNextItemWidth(280 * scale_);
            ImGui::InputText(label(e, "名称", "Name"), rename_.data(), rename_.size());
        }
        if (button(e, "确认", "Confirm", "asset-delete-confirm")) {
            e.handle_asset_action(deleting                  ? EditorAssetAction::Delete
                                  : actionKind_ == "rename" ? EditorAssetAction::Rename
                                                            : EditorAssetAction::NewFolder,
                                  actionPath_, rename_.data());
            actionKind_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(label(e, "取消", "Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            actionKind_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace shinkou::editor
#endif
