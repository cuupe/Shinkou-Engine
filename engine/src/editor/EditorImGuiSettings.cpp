#include "EditorImGuiView.h"
#include "EditorImGuiWidgets.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <algorithm>
#include <cctype>
#include <imgui.h>

namespace shinkou::editor {
namespace {
bool contains_text(std::string_view text, std::string_view query) {
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
                       [](unsigned char a, unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       }) != text.end();
}
} // namespace
void EditorImGuiView::settings(EditorLayer &e, render::Renderer &renderer) {
    if (!e.panel_visible("settings")) {
        settingsSession_ = false;
        bindingCaptureAction_.clear();
        return;
    }
    if (!begin_panel(e, "settings"))
        return;
    const auto reloadDraft = [&] {
        settingsDraft_ = e.preferences_;
        styleDraft_ = e.styleConfig_;
        languageDraft_ = e.layout_.language;
        toolbarDraft_ = e.layout_.showToolbar;
        statusBarDraft_ = e.layout_.showStatusBar;
        dockingDraft_ = e.layout_.allowDocking;
        settingsEdited_ = false;
        settingsMessage_.clear();
        settingsSession_ = true;
    };
    if (!settingsSession_)
        reloadDraft();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##settings-search",
        label(e, "搜索设置、命令或快捷键", "Search settings, commands or shortcuts"),
        settingsSearch_.data(), settingsSearch_.size());
    region(e, "settings.search");
    ImGui::Separator();
    const char *chinese[] = {"外观",   "相机控制", "物体操作", "视口与网格",
                             "快捷键", "工作区",   "渲染器"};
    const char *english[] = {"Appearance", "Camera",    "Transform Tools", "Viewport & Grid",
                             "Shortcuts",  "Workspace", "Renderer"};
    const bool sidebar = ImGui::GetContentRegionAvail().x > 540 * scale_;
    const float footer = ImGui::GetFrameHeightWithSpacing() * (sidebar ? 2.5f : 3.5f);
    const float bodyHeight = std::max(80 * scale_, ImGui::GetContentRegionAvail().y - footer);
    if (sidebar) {
        ImGui::BeginChild("settings-navigation", {145 * scale_, bodyHeight},
                          ImGuiChildFlags_Borders);
        for (int page = 0; page < 7; ++page) {
            if (ImGui::Selectable(label(e, chinese[page], english[page]), settingsPage_ == page)) {
                settingsPage_ = page;
                settingsSearch_.fill(0);
            }
            if (qaEnabled_)
                region(e, "settings.page:" + std::to_string(page));
        }
        ImGui::EndChild();
        ImGui::SameLine();
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##settings-category",
                              label(e, chinese[settingsPage_], english[settingsPage_]))) {
            for (int page = 0; page < 7; ++page) {
                if (ImGui::Selectable(label(e, chinese[page], english[page]),
                                      settingsPage_ == page)) {
                    settingsPage_ = page;
                    settingsSearch_.fill(0);
                }
                if (qaEnabled_)
                    region(e, "settings.page:" + std::to_string(page));
            }
            ImGui::EndCombo();
        }
        region(e, "settings.category");
    }
    const float contentHeight =
        sidebar ? bodyHeight : std::max(80 * scale_, ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("settings-content", {0, contentHeight}, 0,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const std::string_view query = settingsSearch_.data();
    const bool wide = ImGui::GetContentRegionAvail().x > 360 * scale_;
    int visibleRows = 0, currentPage = 0;
    bool table = false, allSection = false, sectionVisible = false;
    const auto ensureSection = [&] {
        if (sectionVisible)
            return;
        sectionVisible = true;
        ImGui::SeparatorText(label(e, chinese[currentPage], english[currentPage]));
        table = wide && currentPage != 4 &&
                ImGui::BeginTable("settings-form", 2, ImGuiTableFlags_SizingStretchProp);
        if (table) {
            ImGui::TableSetupColumn("name", 0, .42f);
            ImGui::TableSetupColumn("value", 0, .58f);
        }
    };
    const auto finishForm = [&] {
        if (table) {
            ImGui::EndTable();
            table = false;
        }
    };
    const auto note = [&](const char *cn, const char *en) {
        if (!sectionVisible)
            return;
        finishForm();
        ImGui::TextWrapped("%s", label(e, cn, en));
    };
    const auto row = [&](const char *cn, const char *en, const char *id, const auto &draw) {
        if (!query.empty() && !allSection && !contains_text(cn, query) &&
            !contains_text(en, query) && !contains_text(id, query))
            return;
        ensureSection();
        ++visibleRows;
        ImGui::PushID(id);
        if (table) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", label(e, cn, en));
        if (table)
            ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (draw()) {
            settingsEdited_ = true;
            settingsMessage_.clear();
        }
        region(e, id);
        ImGui::PopID();
        if (!table)
            ImGui::Spacing();
    };
    const auto section = [&](int page, const auto &draw) {
        if (query.empty() && settingsPage_ != page)
            return;
        currentPage = page;
        sectionVisible = false;
        table = false;
        allSection = !query.empty() &&
                     (contains_text(chinese[page], query) || contains_text(english[page], query));
        ImGui::PushID(page);
        if (query.empty() || allSection)
            ensureSection();
        draw();
        finishForm();
        ImGui::PopID();
        if (sectionVisible)
            ImGui::Spacing();
    };
    section(0, [&] {
        row("主题", "Theme", "settings.theme", [&] {
            bool changed = false;
            if (ImGui::BeginCombo("##value", styleDraft_.activeTheme.c_str())) {
                for (const auto *theme : {"dark", "light", "high-contrast"})
                    if (ImGui::Selectable(theme, styleDraft_.activeTheme == theme)) {
                        styleDraft_.activeTheme = theme;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            return changed;
        });
        row("语言", "Language", "settings.language", [&] {
            bool changed = false;
            if (ImGui::BeginCombo("##value", languageDraft_.c_str())) {
                for (const auto *language : {"zh-CN", "en-US"})
                    if (ImGui::Selectable(language, languageDraft_ == language)) {
                        languageDraft_ = language;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            return changed;
        });
        row("界面缩放", "UI scale", "settings.ui-scale", [&] {
            return ImGui::SliderFloat("##value", &styleDraft_.uiScale, .75f, 2.f, "%.2fx");
        });
        row("字体大小", "Font size", "settings.font-size", [&] {
            return ImGui::SliderFloat("##value", &styleDraft_.fontSize, 12, 32, "%.0f px");
        });
        row("字体文件", "Font file", "settings.font-path", [&] {
            return widgets::text_input("##value", styleDraft_.fontPath,
                                       label(e, "留空使用系统字体", "Empty uses the system font"))
                .changed;
        });
        row("紧凑控件", "Compact controls", "settings.compact",
            [&] { return ImGui::Checkbox("##value", &styleDraft_.compactControls); });
        finishForm();
        if (sectionVisible &&
            button(e, "恢复默认缩放与字号", "Reset scale and font", "settings.reset-scale")) {
            styleDraft_.uiScale = 1;
            styleDraft_.fontSize = EditorLayoutState{}.fontSize;
            settingsEdited_ = true;
        }
    });
    section(1, [&] {
        auto &c = settingsDraft_.camera;
        row("转向灵敏度", "Look sensitivity", "settings.look-sensitivity", [&] {
            return ImGui::DragFloat("##value", &c.lookSensitivity, .02f, .05f, 10, "%.2fx",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("平移灵敏度", "Pan sensitivity", "settings.pan-sensitivity", [&] {
            return ImGui::DragFloat("##value", &c.panSensitivity, .02f, .05f, 10, "%.2fx",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("滚轮灵敏度", "Zoom sensitivity", "settings.zoom-sensitivity", [&] {
            return ImGui::DragFloat("##value", &c.zoomSensitivity, .02f, .05f, 10, "%.2fx",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("反转垂直转向", "Invert vertical look", "settings.invert-y",
            [&] { return ImGui::Checkbox("##value", &c.invertY); });
        row("飞行速度", "Flight speed", "settings.fly-speed", [&] {
            return ImGui::DragFloat("##value", &c.flySpeed, .1f, .05f, 10000, "%.2f m/s",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("加速倍率", "Fast multiplier", "settings.fast-speed", [&] {
            return ImGui::DragFloat("##value", &c.fastMultiplier, .1f, 1, 20, "%.1fx",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("精细移动倍率", "Precise multiplier", "settings.precise-speed", [&] {
            return ImGui::DragFloat("##value", &c.preciseMultiplier, .01f, .01f, 1, "%.2fx",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("水平面飞行", "Planar flight", "settings.planar-flight",
            [&] { return ImGui::Checkbox("##value", &c.planarFlight); });
        row("捕获并循环鼠标", "Capture and wrap mouse", "settings.capture-mouse",
            [&] { return ImGui::Checkbox("##value", &c.captureMouse); });
        row("转向鼠标键", "Look mouse button", "settings.look-button", [&] {
            int value = c.lookButton == 1 ? 0 : 1;
            if (ImGui::Combo("##value", &value,
                             label(e, "右键（中键平移）\0中键（右键平移）\0",
                                   "Right (middle pans)\0Middle (right pans)\0"))) {
                c.lookButton = value ? 2 : 1;
                return true;
            }
            return false;
        });
        row("透视视场角", "Perspective field of view", "settings.fov", [&] {
            return ImGui::DragFloat("##value", &c.fieldOfView, .5f, 5, 150, "%.1f deg",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("近裁剪面", "Near clip plane", "settings.near", [&] {
            return ImGui::DragFloat("##value", &c.nearPlane, .005f, .001f, 10, "%.3f m",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("远裁剪面", "Far clip plane", "settings.far", [&] {
            return ImGui::DragFloat("##value", &c.farPlane, 10, 10.001f, 100000, "%.1f m",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        note("按住转向键使用移动快捷键；滚轮调整飞行速度。编辑器相机与场景相机相互独立。",
             "Hold look to use movement bindings; the wheel adjusts flight speed. Editor and scene "
             "cameras are independent.");
    });
    section(2, [&] {
        auto &g = settingsDraft_.gizmo;
        row("本地坐标", "Local space", "settings.local-space",
            [&] { return ImGui::Checkbox("##value", &g.localSpace); });
        row("启用吸附", "Enable snapping", "settings.snapping",
            [&] { return ImGui::Checkbox("##value", &g.snapping); });
        row("移动吸附 XYZ", "Translation snap XYZ", "settings.translation-snap", [&] {
            float values[] = {g.translationStep.x, g.translationStep.y, g.translationStep.z};
            bool changed = false;
            widgets::VectorInput options;
            options.scale = scale_;
            options.speed = .05f;
            options.minimum = .001f;
            options.maximum = 10000;
            options.resetValues = {1, 1, 1, 0};
            options.onAxis = [&](int, const widgets::EditEvent &event) {
                changed = changed || event.changed;
            };
            widgets::vector_input(values, 3, options);
            if (changed)
                g.translationStep = {values[0], values[1], values[2]};
            return changed;
        });
        row("旋转吸附角度", "Rotation snap angle", "settings.rotation-snap", [&] {
            return ImGui::DragFloat("##value", &g.rotationStep, .5f, .1f, 180, "%.1f deg",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        row("缩放吸附步长", "Scale snap step", "settings.scale-snap", [&] {
            return ImGui::DragFloat("##value", &g.scaleStep, .01f, .001f, 10, "%.3f",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        note("拖动操作器时按住 Ctrl 也可临时启用吸附。Esc 取消本次拖动。",
             "Hold Ctrl while dragging for temporary snapping. Escape cancels the gesture.");
    });
    section(3, [&] {
        auto &v = settingsDraft_.viewport;
        row("视口渲染比例", "Viewport render scale", "settings.render-scale",
            [&] { return ImGui::SliderFloat("##value", &v.renderScale, .5f, 2, "%.2fx"); });
        row("显示网格", "Show grid", "settings.grid-visible",
            [&] { return ImGui::Checkbox("##value", &v.gridVisible); });
        row("自适应网格", "Adaptive grid", "settings.grid-adaptive",
            [&] { return ImGui::Checkbox("##value", &v.adaptiveGrid); });
        row("固定网格间距", "Fixed grid spacing", "settings.grid-step", [&] {
            ImGui::BeginDisabled(v.adaptiveGrid);
            const bool changed = ImGui::DragFloat("##value", &v.gridStep, .1f, .01f, 10000,
                                                  "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            return changed;
        });
        row("网格淡出距离（0 自动）", "Grid fade distance (0 auto)", "settings.grid-fade", [&] {
            return ImGui::DragFloat("##value", &v.gridFadeDistance, 1, 0, 100000, "%.0f m",
                                    ImGuiSliderFlags_AlwaysClamp);
        });
        note("1.0 为原生像素密度。降低比例可减轻 GPU 负担，选择和操作器仍按窗口像素计算。",
             "1.0 uses native pixel density. Lower scales reduce GPU cost; picking and gizmos "
             "still use window pixels.");
    });
    std::string captureRequest;
    section(4, [&] {
        bool shortcutTable = false;
        const EditorLocalization localization(e.layout_.language);
        for (auto &binding : settingsDraft_.bindings) {
            const auto *action = e.actionRegistry_.find(binding.action);
            if (!action)
                continue;
            const auto title = std::string(localization.text(action->label));
            const auto chord = editor_binding_name(binding);
            if (!query.empty() && !allSection && !contains_text(title, query) &&
                !contains_text(action->id, query) && !contains_text(chord, query))
                continue;
            ensureSection();
            if (!shortcutTable) {
                note("点击绑定录入组合键；右键可清除或恢复。飞行控制使用单独按键。",
                     "Click a binding to record a chord; right-click to clear or reset. Flight "
                     "controls use individual keys.");
                shortcutTable = ImGui::BeginTable(
                    "shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
                if (!shortcutTable)
                    break;
                ImGui::TableSetupColumn("action", 0, .6f);
                ImGui::TableSetupColumn("binding", 0, .4f);
            }
            ++visibleRows;
            ImGui::PushID(action->id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", title.c_str());
            ImGui::TextDisabled("%s", editor_context_name(action->context));
            ImGui::TableNextColumn();
            if (ImGui::Button(binding.key ? chord.c_str() : label(e, "未绑定", "Unbound"), {-1, 0}))
                captureRequest = action->id;
            if (qaEnabled_)
                region(e, "settings.binding:" + action->id);
            if (ImGui::BeginPopupContextItem("binding-options")) {
                if (ImGui::MenuItem(label(e, "清除绑定", "Clear binding"))) {
                    binding.key = 0;
                    binding.modifiers = 0;
                    settingsEdited_ = true;
                }
                if (ImGui::MenuItem(label(e, "恢复默认", "Restore default"))) {
                    binding.key = static_cast<std::uint16_t>(action->defaultKey);
                    binding.modifiers = action->defaultModifiers;
                    settingsEdited_ = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (shortcutTable)
            ImGui::EndTable();
    });
    section(5, [&] {
        row("显示工具栏", "Show toolbar", "settings.toolbar",
            [&] { return ImGui::Checkbox("##value", &toolbarDraft_); });
        row("显示状态栏", "Show status bar", "settings.status-bar",
            [&] { return ImGui::Checkbox("##value", &statusBarDraft_); });
        row("启用停靠", "Enable docking", "settings.docking",
            [&] { return ImGui::Checkbox("##value", &dockingDraft_); });
        row("允许独立窗口", "Allow detached windows", "settings.detached",
            [&] { return ImGui::Checkbox("##value", &settingsDraft_.detachedWindows); });
        finishForm();
        if (!sectionVisible)
            return;
        if (button(e, "重置停靠布局", "Reset docking layout", "settings.reset-layout"))
            resetDock_ = true;
        if (button(e, "重新加载控制偏好", "Reload control preferences", "settings.reload")) {
            if (e.reload_preferences())
                reloadDraft();
            else
                settingsMessage_ = e.settingsStatus_;
        }
        ImGui::TextWrapped("%s: %s", label(e, "项目", "Project"),
                           e.fileSystem_.root().string().c_str());
        ImGui::TextWrapped("%s: %s", label(e, "控制偏好", "Control preferences"),
                           editor_preferences_path);
        ImGui::TextWrapped("%s: %s", label(e, "样式", "Appearance profile"),
                           e.layout_.styleFile.c_str());
    });
    section(6, [&] {
        row("下次启动后端", "Renderer on next launch", "settings.backend", [&] {
            bool changed = false;
            if (ImGui::BeginCombo("##value", settingsDraft_.startupBackend.c_str())) {
                for (const auto *backend : {"dx11", "dx12", "vulkan"})
                    if (ImGui::Selectable(backend, settingsDraft_.startupBackend == backend)) {
                        settingsDraft_.startupBackend = backend;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            return changed;
        });
        finishForm();
        if (!sectionVisible)
            return;
        const auto capabilities = renderer.capabilities();
        const auto stats = renderer.stats();
        ImGui::Text("%s: %s", label(e, "当前后端", "Current backend"),
                    capabilities.api == render::BackendApi::DirectX11   ? "DX11"
                    : capabilities.api == render::BackendApi::DirectX12 ? "DX12"
                    : capabilities.api == render::BackendApi::Vulkan    ? "Vulkan"
                                                                        : "Null");
        ImGui::Text("%s: %s", label(e, "设备", "Device"),
                    capabilities.deviceReady ? "Ready" : "Unavailable");
        ImGui::Text("Draws %llu | Dispatch %llu | Barriers %llu",
                    static_cast<unsigned long long>(stats.drawCalls),
                    static_cast<unsigned long long>(stats.dispatchCalls),
                    static_cast<unsigned long long>(stats.barriers));
        ImGui::TextWrapped(
            "%s", label(e, "后端选择由专用编辑器入口在下次启动时读取；命令行指定的后端优先。",
                        "The dedicated editor reads this backend on next launch; an explicit "
                        "command-line backend takes priority."));
        if (!renderer.last_error().empty())
            ImGui::TextWrapped("%s", renderer.last_error().c_str());
    });
    if (!visibleRows && !query.empty())
        ImGui::TextDisabled("%s", label(e, "没有匹配的设置", "No matching settings"));
    ImGui::EndChild();
    ImGui::Separator();
    const auto apply = [&] {
        std::string error;
        if (!settingsDraft_.valid(e.actionRegistry_, error)) {
            settingsMessage_ = error;
            return false;
        }
        if (!e.apply_editor_style(styleDraft_, languageDraft_) ||
            !e.set_preferences(settingsDraft_)) {
            settingsMessage_ = e.settingsStatus_;
            return false;
        }
        e.layout_.showToolbar = toolbarDraft_;
        e.layout_.showStatusBar = statusBarDraft_;
        e.layout_.allowDocking = dockingDraft_;
        settingsEdited_ = false;
        settingsMessage_ = label(e, "已应用，尚未保存", "Applied, not saved");
        return true;
    };
    const auto footerButton = [&](const char *cn, const char *en, const char *id,
                                  bool first = false) {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float width =
            ImGui::CalcTextSize(label(e, cn, en)).x + ImGui::GetStyle().FramePadding.x * 2;
        if (!first && right - ImGui::GetItemRectMax().x > width + ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine();
        return button(e, cn, en, id);
    };
    if (footerButton("应用", "Apply", "settings.apply", true))
        apply();
    if (footerButton("保存", "Save", "settings.save"))
        if (apply()) {
            const bool layout = e.save_layout(), preferences = e.save_preferences();
            settingsMessage_ =
                layout && preferences
                    ? label(e, "设置已保存", "Preferences saved")
                    : label(e, "部分设置保存失败，请检查路径和权限",
                            "Some settings could not be saved; check paths and permissions");
        }
    if (footerButton("还原", "Revert", "settings.revert"))
        reloadDraft();
    if (footerButton("本页默认", "Page defaults", "settings.defaults")) {
        const EditorPreferences defaults;
        switch (settingsPage_) {
        case 0:
            styleDraft_ = ui::UiStyleConfig{};
            languageDraft_ = "zh-CN";
            break;
        case 1:
            settingsDraft_.camera = defaults.camera;
            break;
        case 2:
            settingsDraft_.gizmo = defaults.gizmo;
            break;
        case 3:
            settingsDraft_.viewport = defaults.viewport;
            break;
        case 4:
            settingsDraft_.bindings = e.actionRegistry_.default_bindings();
            break;
        case 5:
            toolbarDraft_ = statusBarDraft_ = dockingDraft_ = true;
            settingsDraft_.detachedWindows = true;
            break;
        case 6:
            settingsDraft_.startupBackend = defaults.startupBackend;
            break;
        }
        settingsEdited_ = true;
        settingsMessage_.clear();
    }
    ImGui::TextWrapped("%s",
                       settingsMessage_.empty()
                           ? (settingsEdited_
                                  ? label(e, "有未应用的修改", "Unapplied changes")
                                  : label(e, "修改后点击应用或保存", "Apply or save after editing"))
                           : settingsMessage_.c_str());
    if (!captureRequest.empty()) {
        bindingCaptureAction_ = captureRequest;
        bindingCandidate_.reset();
        bindingConflict_.clear();
        ImGui::OpenPopup("shortcut-capture");
    }
    if (ImGui::BeginPopupModal("shortcut-capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto *action = e.actionRegistry_.find(bindingCaptureAction_);
        ImGui::Text("%s", action ? action->label.c_str() : bindingCaptureAction_.c_str());
        ImGui::TextWrapped("%s", label(e, "按下组合键，Esc 取消", "Press a chord; Escape cancels"));
        for (std::size_t key = 1; key < EditorInputFrame::keyCount; ++key) {
            const auto logical = static_cast<EditorKey>(key);
            if (logical == EditorKey::Escape || !inputFrame_.pressed[key])
                continue;
            const bool modifier = logical == EditorKey::Shift || logical == EditorKey::Control ||
                                  logical == EditorKey::Alt;
            if (modifier && (!action || !action->held))
                continue;
            bindingCandidate_ = EditorKeyBinding{
                bindingCaptureAction_, static_cast<std::uint16_t>(key),
                static_cast<std::uint8_t>(action && action->held ? 0 : inputFrame_.modifiers)};
            bindingConflict_ = EditorInputRouter::conflict(
                e.actionRegistry_, settingsDraft_.bindings, *bindingCandidate_);
            break;
        }
        const auto assign = [&](bool replace) {
            if (!bindingCandidate_)
                return;
            for (auto &binding : settingsDraft_.bindings) {
                if (binding.action == bindingCandidate_->action)
                    binding = *bindingCandidate_;
                else if (replace && binding.key == bindingCandidate_->key &&
                         binding.modifiers == bindingCandidate_->modifiers) {
                    const auto *other = e.actionRegistry_.find(binding.action);
                    if (action && other &&
                        EditorInputRouter::contexts_overlap(action->context, other->context)) {
                        binding.key = 0;
                        binding.modifiers = 0;
                    }
                }
            }
            settingsEdited_ = true;
            bindingCaptureAction_.clear();
            bindingCandidate_.reset();
            ImGui::CloseCurrentPopup();
        };
        if (bindingCandidate_ && bindingConflict_.empty())
            assign(false);
        else if (bindingCandidate_) {
            ImGui::TextWrapped("%s: %s", label(e, "绑定冲突", "Binding conflict"),
                               bindingConflict_.c_str());
            ImGui::TextWrapped(
                "%s", label(e, "替换会清除重叠上下文中的旧绑定。",
                            "Replacing clears existing bindings in overlapping contexts."));
            if (ImGui::Button(label(e, "替换冲突绑定", "Replace conflicting bindings")))
                assign(true);
        }
        if (ImGui::Button(label(e, "取消", "Cancel")) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            bindingCaptureAction_.clear();
            bindingCandidate_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}
} // namespace shinkou::editor
#endif
