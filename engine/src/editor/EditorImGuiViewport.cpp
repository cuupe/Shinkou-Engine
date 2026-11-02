#include "EditorGlyphs.h"
#include "EditorImGuiView.h"
#include "EditorImGuiWidgets.h"
#include "shinkou/components/PrimitiveMeshComponent.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/profile/Profiler.h"
#include "shinkou/ui/Performance.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>

// ImGuizmo requires ImGui types to be declared before its header.
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui_internal.h>
namespace shinkou::editor {
void EditorImGuiView::viewport(EditorLayer &e, render::Renderer &renderer, World &world,
                               bool game) {
    // World images are offscreen textures now. Paint the panel padding too;
    // transparent gaps otherwise retain closed-popup pixels on load surfaces.
    if (!begin_panel(e, game ? "game" : "viewport",
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;
    if (!renderer.capabilities().supportsEditorViewportScissor) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("viewport-unavailable", {}, ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", label(e, "当前后端尚未接通编辑器场景视口。请使用 D3D11 编辑器。",
                                       "This backend does not support the editor scene viewport "
                                       "yet. Use the D3D11 editor."));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (!game) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("viewport-tools", {0, 0},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetNextItemWidth(80 * scale_);
        const char *viewLabel = e.renderViewMode_ == "Perspective" ? label(e, "透视", "Perspective")
                                                                   : e.renderViewMode_.c_str();
        if (ImGui::BeginCombo("##view", viewLabel)) {
            for (const auto *mode :
                 {"Perspective", "Front", "Back", "Left", "Right", "Top", "Bottom"})
                if (ImGui::Selectable(mode, e.renderViewMode_ == mode)) {
                    e.renderViewMode_ = mode;
                    e.apply_render_view_mode(world, mode);
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const char *ids[] = {"tool.select", "tool.move", "tool.rotate", "tool.scale"};
        const char *fallbacks[] = {"Sel", "Move", "Rot", "Scale"};
        const char *icons[] = {glyph::Object, glyph::Move, glyph::Rotate, glyph::Scale};
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            const bool selected = static_cast<int>(e.viewportController_.tool()) == i - 1;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(iconsReady_ ? icons[i] : fallbacks[i]))
                e.execute_action(ids[i], world);
            if (qaEnabled_)
                region(e,
                       i ? "viewport.operation:" + std::to_string(i - 1) : "viewport.select-tool");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s (%s)", e.actionRegistry_.find(ids[i])->label.c_str(),
                                  e.binding_label(ids[i]).c_str());
            if (selected)
                ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::GetContentRegionAvail().x > 180 * scale_) {
            if (ImGui::Button(e.preferences_.gizmo.localSpace ? label(e, "局部", "Local")
                                                              : label(e, "世界", "World")))
                e.execute_action("tool.space", world);
            region(e, "viewport.local-space");
            ImGui::SameLine();
            if (ImGui::Button(e.preferences_.gizmo.snapping ? label(e, "吸附 开", "Snap On")
                                                            : label(e, "吸附", "Snap")))
                e.execute_action("tool.snap", world);
            region(e, "viewport.snapping");
            ImGui::SameLine();
        }
        bool grid = e.preferences_.viewport.gridVisible;
        if (ImGui::Checkbox(ImGui::GetContentRegionAvail().x > 80 * scale_
                                ? label(e, "网格", "Grid")
                                : "##grid",
                            &grid))
            e.execute_action("view.grid", world);
        region(e, "viewport.grid");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    const auto origin = ImGui::GetCursorScreenPos(), available = ImGui::GetContentRegionAvail();
    const ImVec2 pos{std::round(origin.x), std::round(origin.y)},
        size{std::floor(available.x), std::floor(available.y)};
    if (size.x < 1 || size.y < 1) {
        ImGui::End();
        return;
    }
    const auto hostOrigin = ImGui::GetMainViewport()->Pos;
    const auto renderWidth = static_cast<std::uint32_t>(
        std::max(1.f, std::round(size.x * e.preferences_.viewport.renderScale)));
    const auto renderHeight = static_cast<std::uint32_t>(
        std::max(1.f, std::round(size.y * e.preferences_.viewport.renderScale)));
    const float aspect = static_cast<float>(renderWidth) / renderHeight;
    if (!game || !viewportVisible_)
        viewport_ = {pos.x - hostOrigin.x, pos.y - hostOrigin.y, size.x, size.y};
    viewportVisible_ = true;
    if (!game)
        sceneViewportVisible_ = true;
    ImGui::SetCursorScreenPos(pos);
    // Layout only. An active/hovered InvisibleButton prevents upstream
    // ImGuizmo::CanActivate() from ever claiming a left-button gesture.
    ImGui::Dummy(size);
    region(e, game ? "game.surface" : "viewport.surface");
    const bool hovered = ImGui::IsWindowHovered() &&
                         ImGui::IsMouseHoveringRect(pos, {pos.x + size.x, pos.y + size.y});
    if (ImGui::BeginDragDropTarget()) {
        if (const auto *payload = ImGui::AcceptDragDropPayload("SHINKOU_ASSET")) {
            const float s = std::max(0.25f, e.displayDpiScale_ * e.layout_.uiScale);
            e.drop_asset_to_viewport(static_cast<const char *>(payload->Data),
                                     {(ImGui::GetIO().MousePos.x - hostOrigin.x) / s,
                                      (ImGui::GetIO().MousePos.y - hostOrigin.y) / s});
        }
        ImGui::EndDragDropTarget();
    }
    const auto &instances = e.modelSceneInstances_;
    auto &worldView = worldViews_[game ? 1 : 0];
    bool pickOnRelease = false;
    bool transformChanged = false;
    auto *drawList = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(drawList, 2);
    layers.SetCurrentChannel(drawList, 1);
    ImGui::GetWindowDrawList()->PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, true);
    if (!game) {
        auto &io = ImGui::GetIO();
        const auto pointerWindow =
            reinterpret_cast<std::uintptr_t>(ImGui::GetWindowViewport()->PlatformHandleRaw);
        const bool globalMouse = (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
        const auto screenPoint = [&](math::Vec2 point) {
            return EditorPointerCapture::screen_point(pointerWindow, point, globalMouse);
        };
        const auto syncAnchor = [&] {
            const auto anchor = EditorPointerCapture::input_point(
                pointerWindow, pointerCapture_.anchor(), globalMouse);
            io.MousePos = io.MousePosPrev = {anchor.x, anchor.y};
            io.AddMousePosEvent(anchor.x, anchor.y);
        };
        static const bool postedMouse =
            std::getenv("SHINKOU_UI_QA_TARGETED_MOUSE") && std::getenv("SHINKOU_UI_QA_SNAPSHOT");
        const int lookButton = static_cast<int>(e.preferences_.camera.lookButton),
                  panButton = lookButton == 1 ? 2 : 1;
        const bool cancel =
            io.AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const auto cameraId = ImGui::GetID("scene-camera-gesture");
        if (cameraMouseButton_ >= 0 && (cancel || !ImGui::IsMouseDown(cameraMouseButton_))) {
            if (pointerCapture_.active() && !io.AppFocusLost)
                syncAnchor();
            pointerCapture_.end(!io.AppFocusLost);
            if (ImGui::GetActiveID() == cameraId)
                ImGui::ClearActiveID();
            cameraMouseButton_ = -1;
        }
        if (!ImGuizmo::IsUsing()) {
            if (hovered && !cancel && !ImGui::GetDragDropPayload()) {
                const bool beginLook = ImGui::IsMouseClicked(lookButton),
                           beginPan = ImGui::IsMouseClicked(panButton);
                if (cameraMouseButton_ < 0 && (beginLook || beginPan) && !io.MouseDown[0]) {
                    e.finish_inspector_edit();
                    ImGui::ClearActiveID();
                    cameraMouseButton_ = beginLook ? lookButton : panButton;
                    ImGui::SetActiveID(cameraId, ImGui::GetCurrentWindow());
                    ImGui::FocusWindow(ImGui::GetCurrentWindow());
                    pointerCapture_.end(false);
                    if (e.preferences_.camera.captureMouse && !postedMouse) {
                        const auto low = screenPoint({pos.x, pos.y}),
                                   high = screenPoint({pos.x + size.x, pos.y + size.y});
                        pointerCapture_.begin(pointerWindow,
                                              {std::lround(low.x), std::lround(low.y),
                                               std::lround(high.x), std::lround(high.y)},
                                              screenPoint({io.MousePos.x, io.MousePos.y}));
                    }
                }
                if (cameraMouseButton_ < 0 && !ImGui::IsAnyItemActive()) {
                    if (ImGui::IsMouseClicked(0))
                        viewportLeftPress_ = true;
                    if (io.MouseWheel != 0)
                        e.navigate_viewport(ViewportNavigation::Zoom, {0, io.MouseWheel});
                }
            }
            if (cameraMouseButton_ >= 0) {
                ui::ui_performance().workload(ui::UiWorkload::Input);
                ImGui::KeepAliveID(cameraId);
                if (!ImGui::IsMouseClicked(cameraMouseButton_)) {
                    const auto delta =
                        pointerCapture_.delta(screenPoint({io.MousePos.x, io.MousePos.y}),
                                              {io.MouseDelta.x, io.MouseDelta.y});
                    if (pointerCapture_.active()) {
                        syncAnchor();
                        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                    }
                    if (pointerCapture_.lost_focus()) {
                        if (ImGui::GetActiveID() == cameraId)
                            ImGui::ClearActiveID();
                        cameraMouseButton_ = -1;
                        pointerCapture_.end(false);
                    } else
                        e.navigate_viewport(cameraMouseButton_ == lookButton
                                                ? ViewportNavigation::Orbit
                                                : ViewportNavigation::Pan,
                                            delta);
                }
                if (cameraMouseButton_ == lookButton &&
                    !e.viewportController_.camera().orthographic) {
                    auto frame = inputFrame_;
                    frame.textInput = false;
                    frame.itemActive = false;
                    const auto held =
                        e.inputRouter_.route(e.actionRegistry_, e.preferences_.bindings, frame,
                                             EditorInputContext::FlyCamera);
                    e.fly_viewport(held, io.DeltaTime);
                    if (hovered && io.MouseWheel != 0) {
                        auto preferences = e.preferences_;
                        preferences.camera.flySpeed =
                            std::clamp(preferences.camera.flySpeed * std::exp(io.MouseWheel * .2f),
                                       .05f, 10000.f);
                        e.set_preferences(std::move(preferences));
                    }
                }
            }
        }
        if (cameraMouseButton_ < 0 && !ImGuizmo::IsUsing() &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
            route_actions(e, world, EditorInputContext::Scene);
        const auto view = e.viewportController_.view(aspect);
        // Stable projected hit regions let native QA click actual world pixels.
        // They are observations only; picking still travels through Win32 input.
        if (qaEnabled_)
            world.each_game_object([&](const GameObject &object) {
                if (!object.active_in_hierarchy() ||
                    (!object.has_ecs_entity() && !object.get_component<AssetReferenceComponent>() &&
                     !object.get_component<components::PrimitiveMeshComponent>()))
                    return;
                const auto *transform = object.get_component<components::TransformComponent>();
                if (!transform)
                    return;
                const auto center = math::TransformPoint(transform->world_matrix(), {0, 0, 0});
                const auto clip =
                    math::TransformVector4(view.viewProjection, {center.x, center.y, center.z, 1});
                if (clip.w <= 0 || clip.z < 0 || clip.z > clip.w || std::abs(clip.x) >= clip.w ||
                    std::abs(clip.y) >= clip.w)
                    return;
                const float x = pos.x + (clip.x / clip.w + 1) * size.x * 0.5f,
                            y = pos.y + (1 - clip.y / clip.w) * size.y * 0.5f;
                region_rect(e, "world-object:" + std::string(object.name()), {x - 3, y - 3, 6, 6});
            });
        ImGuizmo::SetOrthographic(view.projection.orthographic);
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
        ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
        if (cancel && gizmoEditing_) {
            e.finish_inspector_edit(true);
            gizmoEditing_ = false;
            transformChanged = true;
        }
        ImGuizmo::Enable(!cancel && cameraMouseButton_ < 0);
        const int operation = static_cast<int>(e.viewportController_.tool());
        if (auto *object = operation >= 0 ? world.find_object(e.layout_.selectedObject) : nullptr) {
            if (auto *transform = object->get_component<components::TransformComponent>()) {
                auto matrix = transform->world_matrix();
                if (qaEnabled_) {
                    const auto origin = math::TransformVector4(
                        view.viewProjection, {matrix.m[12], matrix.m[13], matrix.m[14], 1});
                    if (origin.w > 0 && origin.z >= 0 && origin.z <= origin.w) {
                        const float x = pos.x + (origin.x / origin.w + 1) * size.x * .5f,
                                    y = pos.y + (1 - origin.y / origin.w) * size.y * .5f;
                        region_rect(e, "viewport.gizmo-center", {x - 2, y - 2, 4, 4});
                    }
                }
                const ImGuizmo::OPERATION operations[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE,
                                                          ImGuizmo::SCALE};
                const auto &config = e.preferences_.gizmo;
                const float translation[] = {config.translationStep.x, config.translationStep.y,
                                             config.translationStep.z};
                const float *snap = (config.snapping || io.KeyCtrl)
                                        ? (operation == 0   ? translation
                                           : operation == 1 ? &config.rotationStep
                                                            : &config.scaleStep)
                                        : nullptr;
                const bool changed = ImGuizmo::Manipulate(
                    view.viewMatrix.m, view.projectionMatrix.m, operations[operation],
                    e.preferences_.gizmo.localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, matrix.m,
                    nullptr, snap);
                const bool usingGizmo = ImGuizmo::IsUsing();
                if (usingGizmo)
                    ui::ui_performance().workload(ui::UiWorkload::Input);
                if (qaEnabled_ && ImGuizmo::IsOver()) {
                    const auto mouse = ImGui::GetIO().MousePos;
                    region_rect(e, "viewport.gizmo-hover", {mouse.x - 1, mouse.y - 1, 2, 2});
                }
                gizmoConsumedPress_ = gizmoConsumedPress_ || usingGizmo;
                const auto field = std::to_string(transform->id()) + ":__local";
                if (usingGizmo && !gizmoEditing_)
                    gizmoEditing_ = e.begin_inspector_edit(field);
                if (changed && gizmoEditing_) {
                    if (object->parent())
                        if (auto *parent =
                                object->parent()->get_component<components::TransformComponent>())
                            matrix = math::InverseOrIdentity(parent->world_matrix()) * matrix;
                    math::Transform next;
                    if (math::Decompose(matrix, next))
                        transformChanged = e.edit_inspector_value(field, next) || transformChanged;
                }
                if (!usingGizmo && gizmoEditing_) {
                    e.finish_inspector_edit();
                    gizmoEditing_ = false;
                }
            }
        }
        if (!ImGuizmo::IsUsing()) {
            const auto &io = ImGui::GetIO();
            if (hovered && viewportLeftPress_ && !gizmoConsumedPress_ && !ImGuizmo::IsOver() &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <
                    io.MouseDragThreshold * io.MouseDragThreshold) {
                pickOnRelease = true;
            }
        }
        if (cancel || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            viewportLeftPress_ = false;
            gizmoConsumedPress_ = false;
        }
    }
    ImGui::GetWindowDrawList()->PopClipRect();
    // Apply this frame's interaction before recording draws. The image and
    // picking now consume the current transform/camera instead of lagging by
    // an additional frame. Refresh descendants too after a parent edit.
    if (transformChanged)
        for (auto &instance : e.modelSceneInstances_) {
            if (auto *object = world.find_object(instance.objectId))
                if (auto *transform = object->get_component<components::TransformComponent>())
                    instance.model = transform->world_matrix();
        }
    layers.SetCurrentChannel(drawList, 0);
    const auto currentView =
        game ? render::extract_render_view(world, aspect) : e.viewportController_.view(aspect);
    const bool rendered = currentView.has_camera() &&
                          worldView.render(renderer, world, renderWidth, renderHeight, instances,
                                           !game && e.preferences_.viewport.gridVisible,
                                           &currentView, &e.preferences_.viewport);
    if (!game)
        e.modelSceneRenderState_ = worldView.model_state();
    if (rendered) {
        const auto texture = renderer.imgui_texture(worldView.texture());
        if (texture)
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture), pos,
                                                 {pos.x + size.x, pos.y + size.y});
        else if (!game)
            e.modelSceneRenderState_.rendererReady = false;
    } else if (!game)
        e.modelSceneRenderState_.rendererReady = false;
    if (game && !currentView.has_camera()) {
        const auto *text = label(e, "没有活动的场景相机。可在游戏对象菜单中创建相机。",
                                 "No active scene camera. Create one from the GameObject menu.");
        drawList->AddText({pos.x + 12 * scale_, pos.y + 24 * scale_},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
    }
    layers.Merge(drawList);
    if (pickOnRelease) {
        const auto mouse = ImGui::GetIO().MousePos;
        e.select_object(worldView.pick(world, renderer, instances,
                                       {(mouse.x - pos.x) * renderWidth / size.x,
                                        (mouse.y - pos.y) * renderHeight / size.y}),
                        world);
    }

    if (!game && size.x > 440 * scale_) {
        if (cameraHintRevision_ != e.preferencesRevision_ ||
            cameraHintLanguage_ != e.layout_.language) {
            cameraHintRevision_ = e.preferencesRevision_;
            cameraHintLanguage_ = e.layout_.language;
            cameraHint_ = e.preferences_.camera.lookButton == 1
                              ? label(e, "右键 转向/飞行   中键 平移", "RMB Look/Fly   MMB Pan")
                              : label(e, "中键 转向/飞行   右键 平移", "MMB Look/Fly   RMB Pan");
            cameraHint_ += "   " + e.binding_label("camera.forward") + "/" +
                           e.binding_label("camera.left") + "/" + e.binding_label("camera.back") +
                           "/" + e.binding_label("camera.right");
        }
        char movingHint[384]{};
        const char *hint = cameraHint_.c_str();
        if (cameraMouseButton_ >= 0) {
            std::snprintf(movingHint, sizeof(movingHint), "%s   %.2f m/s", hint,
                          e.preferences_.camera.flySpeed);
            hint = movingHint;
        }
        const auto extent = ImGui::CalcTextSize(hint);
        const ImVec2 text{pos.x + 10 * scale_, pos.y + size.y - extent.y - 8 * scale_};
        ImGui::GetWindowDrawList()->AddRectFilled(
            {text.x - 4 * scale_, text.y - 3 * scale_},
            {text.x + extent.x + 4 * scale_, text.y + extent.y + 3 * scale_},
            IM_COL32(20, 23, 28, 180), 3 * scale_);
        ImGui::GetWindowDrawList()->AddText(text, IM_COL32(220, 225, 235, 255), hint);
    }
    ImGui::End();
}
} // namespace shinkou::editor
#endif
