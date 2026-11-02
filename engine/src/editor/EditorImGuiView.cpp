#include "EditorImGuiView.h"
#include "EditorGlyphs.h"
#include "EditorImGuiWidgets.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#include "shinkou/input/InputSystem.h"
#include "shinkou/profile/Profiler.h"

#if defined(SHINKOU_WITH_IMGUI)
#include <ImGuizmo.h>
#include <hello_imgui/imgui_theme.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <backends/imgui_impl_win32.h>
#endif
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace shinkou::editor {
const char *EditorImGuiView::label(const EditorLayer &e, const char *zh, const char *en) const {
    return e.layout_.language == "zh-CN" ? zh : en;
}
std::string EditorImGuiView::title(const EditorLayer &e, const char *id) const {
    const std::string pageId = std::string(id) == "viewport" ? "scene"
                               : std::string(id) == "assets" ? "project"
                                                             : id;
    for (const auto &page : e.uiModel_.pages())
        if (page.id == pageId)
            return std::string(EditorLocalization(e.layout_.language).text(page.title)) + "###" +
                   id;
    for (const auto &panel : e.panels_)
        if (panel.id == id)
            return panel.title + "###" + id;
    return std::string(id) + "###" + id;
}
void EditorImGuiView::region(const EditorLayer &e, std::string_view id) {
    if (!qaEnabled_)
        return;
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImRect visible(a, b);
    visible.ClipWithFull(ImGui::GetCurrentWindow()->ClipRect);
    if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0)
        return;
    region_rect(e, id, {visible.Min.x, visible.Min.y, visible.GetWidth(), visible.GetHeight()});
    if (ImGui::IsItemFocused() || ImGui::IsItemActive())
        focused_ = id;
}
void EditorImGuiView::region_rect(const EditorLayer &e, std::string_view id, ui::Rect screen) {
    if (!qaEnabled_)
        return;
    const float s = std::max(0.25f, e.displayDpiScale_ * e.layout_.uiScale);
    const auto origin = ImGui::GetMainViewport()->Pos;
    const auto *surface = ImGui::GetWindowViewport();
    regions_[std::string(id)] = {(screen.x - origin.x) / s, (screen.y - origin.y) / s,
                                 screen.width / s, screen.height / s};
    regionSurfaces_[std::string(id)] = {
        reinterpret_cast<std::uintptr_t>(surface->PlatformHandleRaw),
        {screen.x - surface->Pos.x, screen.y - surface->Pos.y, screen.width, screen.height}};
}
void EditorImGuiView::alias_region(std::string_view alias, std::string_view source) {
    if (!qaEnabled_)
        return;
    const auto key = std::string(source);
    const auto logical = regions_.find(key);
    const auto surface = regionSurfaces_.find(key);
    if (logical == regions_.end() || surface == regionSurfaces_.end())
        return;
    regions_[std::string(alias)] = logical->second;
    regionSurfaces_[std::string(alias)] = surface->second;
}
bool EditorImGuiView::button(EditorLayer &e, const char *zh, const char *en, const char *id) {
    ++labeledControls_;
    ImGui::PushID(id);
    const std::string_view key = id;
    const char *icon = nullptr;
    if (key == "toolbar.play")
        icon = e.uiModel_.playing() ? glyph::Stop : glyph::Play;
    else if (key == "toolbar.pause")
        icon = e.uiModel_.paused() ? glyph::Play : glyph::Pause;
    else if (key == "toolbar.step")
        icon = glyph::Step;
    else if (key == "toolbar.save" || key == "settings.save")
        icon = glyph::Save;
    else if (key == "toolbar.build")
        icon = glyph::Build;
    else if (key == "toolbar.panels")
        icon = glyph::Layout;
    else if (key == "command:create-empty")
        icon = glyph::Add;
    else if (key == "asset-parent")
        icon = glyph::Up;
    else if (key == "asset-refresh")
        icon = glyph::Refresh;
    if (!iconsReady_)
        icon = nullptr;
    const bool clicked = ImGui::Button(icon ? icon : label(e, zh, en));
    if (icon && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", label(e, zh, en));
    region(e, id);
    if (qaEnabled_ && key.rfind("toolbar.", 0) == 0)
        alias_region("command:" + std::string(key.substr(8)), id);
    ImGui::PopID();
    return clicked;
}
bool EditorImGuiView::begin_panel(EditorLayer &e, const char *id, int flags) {
    const bool panelVisible = e.panel_visible(id);
    const auto [it, first] = panelWasVisible_.try_emplace(id, panelVisible);
    if (!first && !it->second && panelVisible)
        focusPanel_ = id;
    it->second = panelVisible;
    if (!panelVisible)
        return false;
    if (focusPanel_ == id) {
        ImGui::SetNextWindowFocus();
        focusPanel_.clear();
    }
    if (flags & ImGuiWindowFlags_NoBackground) {
        // A docked window still needs to publish a transparent node background.
        flags &= ~ImGuiWindowFlags_NoBackground;
        ImGui::SetNextWindowBgAlpha(0);
    }
    ++labeledControls_;
    bool open = true;
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::SetNextWindowClass(&windowClass);
    const bool visible =
        ImGui::Begin(title(e, id).c_str(), &open,
                     flags | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse);
    if (qaEnabled_)
        if (const auto *node = ImGui::GetCurrentWindow()->DockNode)
            if (const auto *bar = node->TabBar) {
                for (const auto &tab : bar->Tabs)
                    if (tab.Window == ImGui::GetCurrentWindow()) {
                        region_rect(e, std::string("tab:") + id,
                                    {bar->BarRect.Min.x + tab.Offset, bar->BarRect.Min.y, tab.Width,
                                     bar->BarRect.GetHeight()});
                    }
            }
    if (qaEnabled_ && !ImGui::GetCurrentWindow()->DockNode)
        region_rect(e, std::string("tab:") + id,
                    {ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowWidth(),
                     ImGui::GetFrameHeight()});
    if (!open) {
        e.finish_inspector_edit();
        e.set_panel_visible(id, false);
    }
    if (!visible)
        ImGui::End();
    return visible;
}
void EditorImGuiView::initialize(EditorLayer &e) {
    iniPath_ = e.layout_.layoutFile + ".imgui.ini";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(iniPath_).parent_path(), ec);
    auto &io = ImGui::GetIO();
    io.IniFilename = iniPath_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigViewportsNoTaskBarIcon = false;
    io.ConfigDockingWithShift = false;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    rebuild_fonts(e);
    ImPlot::CreateContext();
    plotOwned_ = true;
    apply_theme(e);
}
void EditorImGuiView::rebuild_fonts(EditorLayer &e) {
    hierarchyWidthRevision_ = ~std::uint64_t{0};
    auto &io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontDefault = nullptr;
    fontScale_ = std::clamp(e.displayDpiScale_ * e.layout_.uiScale, 0.5f, 4.0f);
    fontSize_ = std::clamp(e.layout_.fontSize, 12.0f, 32.0f);
    fontPath_ = e.layout_.fontPath;
    std::vector<std::filesystem::path> fonts;
    if (!e.layout_.fontPath.empty())
        fonts.emplace_back(e.layout_.fontPath);
    fonts.emplace_back("C:/Windows/Fonts/msyh.ttc");
    fonts.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    for (const auto &font : fonts) {
        ImFontConfig config;
        config.OversampleH = 1;
        config.OversampleV = 1;
        if (std::filesystem::exists(font) &&
            io.Fonts->AddFontFromFileTTF(font.string().c_str(), fontSize_ * fontScale_, &config,
                                         io.Fonts->GetGlyphRangesChineseFull()))
            break;
    }
    if (io.Fonts->Fonts.empty()) {
        ImFontConfig config;
        config.SizePixels = fontSize_ * fontScale_;
        io.Fonts->AddFontDefault(&config);
    }
    static const ImWchar iconRanges[] = {0xea60, 0xefff, 0};
    std::filesystem::path iconFont = SHINKOU_EDITOR_ICON_FONT_SOURCE;
    if (!std::filesystem::exists(iconFont))
        iconFont = std::filesystem::path(SHINKOU_EDITOR_ICON_FONT_INSTALL);
    iconsReady_ = false;
    if (std::filesystem::exists(iconFont)) {
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.GlyphMinAdvanceX = fontSize_ * fontScale_;
        iconsReady_ =
            io.Fonts->AddFontFromFileTTF(iconFont.string().c_str(), fontSize_ * fontScale_, &config,
                                         iconRanges) != nullptr;
    }
    io.Fonts->Build();
    io.FontGlobalScale = 1.0f;
}
void EditorImGuiView::apply_theme(EditorLayer &e) {
    const float nextScale = std::clamp(e.displayDpiScale_ * e.layout_.uiScale, 0.5f, 4.0f);
    if (theme_ == e.layout_.theme && compact_ == e.layout_.compactControls &&
        std::abs(scale_ - nextScale) < 0.001f)
        return;
    compact_ = e.layout_.compactControls;
    theme_ = e.layout_.theme;
    scale_ = nextScale;
    // Use the library's complete style, reset before scaling to avoid cumulative DPI drift.
    auto theme = ImGuiTheme::ImGuiTheme_PhotoshopStyle;
    if (theme_ == "light")
        theme = ImGuiTheme::ImGuiTheme_MicrosoftStyle;
    if (theme_ == "high-contrast")
        theme = ImGuiTheme::ImGuiTheme_BlackIsBlack;
    ImGui::GetStyle() = ImGuiTheme::ThemeToStyle(theme);
    auto &style = ImGui::GetStyle();
    style.WindowPadding = {8, 6};
    style.FramePadding = compact_ ? ImVec2(4, 2) : ImVec2(6, 3);
    style.ItemSpacing = compact_ ? ImVec2(4, 3) : ImVec2(6, 5);
    style.CellPadding = {3, 3};
    style.TabCloseButtonMinWidthSelected = FLT_MAX;
    style.TabCloseButtonMinWidthUnselected = FLT_MAX;
    style.WindowRounding = 0;
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.TabRounding = 3;
    style.DockingSeparatorSize = 5;
    style.ScaleAllSizes(scale_);
    ImGui::GetIO().FontGlobalScale = 1.0f;
    if (plotOwned_) {
        if (theme_ == "light")
            ImPlot::StyleColorsLight();
        else
            ImPlot::StyleColorsDark();
    }
}
void EditorImGuiView::workspace(EditorLayer &e, render::Renderer &, World &world) {
    const auto command = [&](EditorCommand cmd) { e.dispatch_command(cmd, {}, world); };
    auto *main = ImGui::GetMainViewport();
    float top = main->WorkPos.y;
    if (ImGui::BeginMainMenuBar()) {
        for (const auto &menu : e.uiModel_.menus()) {
            const EditorLocalization localization(e.layout_.language);
            const auto menuLabel = std::string(localization.text(menu.label));
            const bool menuOpen = ImGui::BeginMenu(menuLabel.c_str());
            if (qaEnabled_)
                region(e, "menu:" + menu.id);
            if (!menuOpen)
                continue;
            for (const auto &item : menu.items) {
                if (item.separator)
                    ImGui::Separator();
                else {
                    const auto *action = e.actionRegistry_.find_command(item.command, item.target);
                    const auto shortcut = action ? e.binding_label(action->id) : item.shortcut;
                    const bool enabled =
                        item.enabled && (!action || e.can_execute_action(action->id, world));
                    if (ImGui::MenuItem(std::string(localization.text(item.label)).c_str(),
                                        shortcut.c_str(), false, enabled)) {
                        if (action)
                            e.execute_action(action->id, world);
                        else
                            e.dispatch_command(item.command, item.target, world);
                    }
                    if (qaEnabled_)
                        region(e, "menu-item:" + item.id);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    top = main->WorkPos.y;
    const int fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                      ImGuiWindowFlags_NoScrollWithMouse;
    const float toolbarHeight = e.layout_.showToolbar ? 34.0f * scale_ : 0;
    if (e.layout_.showToolbar) {
        ImGui::SetNextWindowViewport(main->ID);
        ImGui::SetNextWindowPos({main->WorkPos.x, top});
        ImGui::SetNextWindowSize({main->WorkSize.x, toolbarHeight});
        ImGui::Begin("###editor-toolbar", nullptr, fixed);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("SHINKOU");
        ImGui::SameLine();
        const float buttonsX = std::max(230.0f * scale_, main->WorkSize.x * 0.35f);
        const auto project = "/ " + e.fileSystem_.root().filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        widgets::clipped_text(project.c_str(),
                              buttonsX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::PopStyleColor();
        ImGui::SameLine(buttonsX);
        if (button(e, e.uiModel_.playing() ? "停止" : "运行",
                   e.uiModel_.playing() ? "Stop" : "Play", "toolbar.play"))
            command(EditorCommand::Play);
        ImGui::SameLine();
        ImGui::BeginDisabled(!e.uiModel_.playing());
        if (button(e, e.uiModel_.paused() ? "继续" : "暂停",
                   e.uiModel_.paused() ? "Resume" : "Pause", "toolbar.pause"))
            command(EditorCommand::Pause);
        ImGui::SameLine();
        if (button(e, "单步", "Step", "toolbar.step"))
            command(EditorCommand::Step);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (button(e, "保存", "Save", "toolbar.save"))
            command(EditorCommand::SaveScene);
        ImGui::SameLine();
        if (button(e, "构建", "Build", "toolbar.build")) {
            e.set_panel_visible("build", true);
            command(EditorCommand::BuildProject);
        }
        ImGui::SameLine();
        if (button(e, "面板", "Panels", "toolbar.panels"))
            ImGui::OpenPopup("workspace-panels");
        if (ImGui::BeginPopup("workspace-panels")) {
            for (const auto &panel : e.panels_) {
                const auto text = title(e, panel.id.c_str());
                if (ImGui::MenuItem(text.c_str(), nullptr, e.panel_visible(panel.id))) {
                    const bool show = !e.panel_visible(panel.id);
                    e.set_panel_visible(panel.id, show);
                    if (show)
                        focusPanel_ = panel.id;
                }
                if (qaEnabled_)
                    region(e, "window:" + panel.id);
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }
    const float statusHeight = e.layout_.showStatusBar ? 21.0f * scale_ : 0;
    ImGui::SetNextWindowViewport(main->ID);
    ImGui::SetNextWindowPos({main->WorkPos.x, top + toolbarHeight});
    ImGui::SetNextWindowSize(
        {main->WorkSize.x, std::max(1.0f, main->WorkSize.y - toolbarHeight - statusHeight -
                                              (top - main->WorkPos.y))});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("###editor-dock-host", nullptr,
                 fixed | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    const auto dock = ImGui::GetID("ShinkouWorkspace");
    const auto dockSize = ImGui::GetContentRegionAvail();
    if (auto *root = ImGui::DockBuilderGetNode(dock)) {
        if (dockSize_.x != dockSize.x || dockSize_.y != dockSize.y) {
            // ImGui normally gives all resize delta to the central node.
            // Preserve user split proportions when changing monitor/window
            // resolution, so fixed sidebars cannot squeeze the world to zero.
            const auto scaleSplits = [&](const auto &self, ImGuiDockNode *node,
                                         ImVec2 size) -> void {
                if (!node || !node->ChildNodes[0] || !node->ChildNodes[1])
                    return;
                auto *first = node->ChildNodes[0];
                auto *second = node->ChildNodes[1];
                const int axis = node->SplitAxis;
                const float sum = first->SizeRef[axis] + second->SizeRef[axis];
                const float ratio = sum > 0 ? first->SizeRef[axis] / sum : 0.5f;
                const float available =
                    std::max(2.0f, size[axis] - ImGui::GetStyle().DockingSeparatorSize);
                ImVec2 a = size, b = size;
                a[axis] = std::max(1.0f, std::floor(available * ratio));
                b[axis] = std::max(1.0f, available - a[axis]);
                first->SizeRef = a;
                second->SizeRef = b;
                first->WantLockSizeOnce = second->WantLockSizeOnce = false;
                self(self, first, a);
                self(self, second, b);
            };
            scaleSplits(scaleSplits, root, dockSize);
        }
    }
    dockSize_ = {dockSize.x, dockSize.y};
    if (!ImGui::DockBuilderGetNode(dock) || resetDock_) {
        resetDock_ = false;
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace |
                                            ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetContentRegionAvail());
        auto center = dock;
        const auto right =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
        const auto left =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
        const auto bottom =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.31f, nullptr, &center);
        ImGui::DockBuilderDockWindow(title(e, "hierarchy").c_str(), left);
        ImGui::DockBuilderDockWindow(title(e, "inspector").c_str(), right);
        for (const auto *id : {"settings", "recovery", "media"})
            ImGui::DockBuilderDockWindow(title(e, id).c_str(), right);
        for (const auto *id : {"assets", "console", "profiler", "render-graph", "build"})
            ImGui::DockBuilderDockWindow(title(e, id).c_str(), bottom);
        ImGui::DockBuilderDockWindow(title(e, "viewport").c_str(), center);
        ImGui::DockBuilderDockWindow(title(e, "game").c_str(), center);
        ImGui::DockBuilderFinish(dock);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::DockSpace(dock, {},
                     ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoCloseButton |
                         ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::PopStyleColor();
    ImGui::End();
    if (e.layout_.showStatusBar) {
        ImGui::SetNextWindowViewport(main->ID);
        ImGui::SetNextWindowPos(
            {main->WorkPos.x, main->WorkPos.y + main->WorkSize.y - statusHeight});
        ImGui::SetNextWindowSize({main->WorkSize.x, statusHeight});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10 * scale_, 4 * scale_});
        ImGui::Begin("###editor-status", nullptr, fixed);
        const bool showMetrics = main->WorkSize.x > 750 * scale_;
        const float statusWidth = main->WorkSize.x - (showMetrics ? 240 * scale_ : 0) - 20 * scale_;
        widgets::clipped_text(
            e.lastStatus_.empty() ? label(e, "就绪", "Ready") : e.lastStatus_.c_str(), statusWidth);
        if (showMetrics) {
            ImGui::SameLine(main->WorkSize.x - 240 * scale_);
            ImGui::TextDisabled("%.1f ms | %llu draws",
                                e.frameTimes_.empty() ? 0 : e.frameTimes_.back(),
                                static_cast<unsigned long long>(frameDrawCalls_));
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}
void EditorImGuiView::draw(EditorLayer &e, render::Renderer &renderer, World &world, float dt,
                           std::uint64_t frame) {
    SHINKOU_PROFILE_SCOPE("Editor.UI.Build");
    const float desiredScale = std::clamp(e.displayDpiScale_ * e.layout_.uiScale, 0.5f, 4.0f);
    if (std::abs(fontScale_ - desiredScale) > 0.001f ||
        fontSize_ != std::clamp(e.layout_.fontSize, 12.0f, 32.0f) ||
        fontPath_ != e.layout_.fontPath) {
        renderer.invalidate_imgui_fonts();
        rebuild_fonts(e);
    }
    apply_theme(e);
    // DX12 installs its secondary-window callbacks during Init. Enable the
    // feature before the renderer's first initialization, not after NewFrame.
    if (e.preferences_.detachedWindows && renderer.capabilities().supportsEditorViewportScissor &&
        (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_PlatformHasViewports))
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    const bool gpuUi = renderer.capabilities().supportsImGui;
    if (gpuUi && !renderer.new_imgui_frame()) {
        e.push_console("ImGui renderer initialization failed: " + renderer.last_error());
        return;
    }
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (e.imguiPlatformInitialized_)
        ImGui_ImplWin32_NewFrame();
#endif
    auto &io = ImGui::GetIO();
    // Both platform and renderer must implement secondary windows.
    if (e.preferences_.detachedWindows && renderer.capabilities().supportsEditorViewportScissor &&
        (io.BackendFlags &
         (ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports)) ==
            (ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports))
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    if (e.layout_.allowDocking)
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
    static const bool targetedQaMouse =
        std::getenv("SHINKOU_UI_QA_TARGETED_MOUSE") && std::getenv("SHINKOU_UI_QA_SNAPSHOT");
    // The official geometry fallback uses the posted mouse coordinates when
    // unattended QA cannot move the hardware cursor. Production keeps Win32's
    // WindowFromPoint-based viewport detection unchanged.
    if (targetedQaMouse)
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseHoveredViewport;
    io.DisplaySize = {e.displayWidth_, e.displayHeight_};
    io.DeltaTime = std::max(0.001f, dt);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    const auto draws = renderer.stats().drawCalls;
    frameDrawCalls_ = draws >= lastDrawCalls_ ? draws - lastDrawCalls_ : draws;
    lastDrawCalls_ = draws;
    labeledControls_ = 0;
    performance_.hierarchySubmitted = 0;
    performance_.qaEnabled = qaEnabled_;
    regions_.clear();
    regionSurfaces_.clear();
    viewportVisible_ = false;
    sceneViewportVisible_ = false;
    shortcuts(e, world);
    workspace(e, renderer, world);
    hierarchy(e, world);
    inspector(e, renderer, world);
    assets(e);
    viewport(e, renderer, world, false);
    viewport(e, renderer, world, true);
    if (!sceneViewportVisible_) {
        pointerCapture_.end(false);
        if (gizmoEditing_) {
            e.finish_inspector_edit(true);
            gizmoEditing_ = false;
        }
        ImGuizmo::Enable(false);
        cameraMouseButton_ = -1;
        viewportLeftPress_ = false;
        gizmoConsumedPress_ = false;
    }
    console(e);
    profiler(e, renderer);
    render_graph(e, renderer);
    settings(e, renderer);
    build(e, world);
    media(e, renderer, world);
    recovery(e, world);
    registered_panels(e, renderer, world, frame, dt);
    command_palette(e, world);
    if (!viewportVisible_)
        renderer.clear_editor_viewport();
    {
        SHINKOU_PROFILE_SCOPE("Editor.UI.RenderData");
        ImGui::Render();
    }
    performance_.qaRegions = regions_.size();
    // Platform lifetime must advance even when a render-graph pass fails and
    // discards its GPU frame. Rendering is deferred until world submissions.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        ImGui::UpdatePlatformWindows();
    drawCommands_ = 0;
    for (const auto *list : ImGui::GetDrawData()->CmdLists)
        drawCommands_ += list->CmdBuffer.Size;
    if (gpuUi)
        renderer.set_imgui_draw_data(ImGui::GetDrawData());
}
void EditorImGuiView::registered_panels(EditorLayer &e, render::Renderer &renderer, World &world,
                                        std::uint64_t frame, float dt) {
    EditorPanelContext context{renderer, world, e.uiModel_, e.layout_, frame, dt};
    for (const auto &panel : e.panels_)
        if (panel.draw && begin_panel(e, panel.id.c_str())) {
            panel.draw(context);
            ImGui::End();
        }
}
void EditorImGuiView::process_input(const input::InputSystem &input) {
    auto &io = ImGui::GetIO();
    const auto key = [](std::string_view control) -> ImGuiKey {
        if (control.rfind("key:", 0) == 0)
            control.remove_prefix(4);
        if (control.size() == 1 && control[0] >= 'A' && control[0] <= 'Z')
            return static_cast<ImGuiKey>(ImGuiKey_A + control[0] - 'A');
        if (control.size() == 1 && control[0] >= '0' && control[0] <= '9')
            return static_cast<ImGuiKey>(ImGuiKey_0 + control[0] - '0');
        if (control == "Ctrl" || control == "LeftCtrl" || control == "Left Ctrl" ||
            control == "Right Ctrl")
            return ImGuiMod_Ctrl;
        if (control == "Shift" || control == "LeftShift" || control == "Left Shift" ||
            control == "Right Shift")
            return ImGuiMod_Shift;
        if (control == "Alt" || control == "LeftAlt" || control == "Left Alt" ||
            control == "Right Alt")
            return ImGuiMod_Alt;
        if (control == "Enter" || control == "Return")
            return ImGuiKey_Enter;
        if (control == "Escape")
            return ImGuiKey_Escape;
        if (control == "Backspace")
            return ImGuiKey_Backspace;
        if (control == "Delete")
            return ImGuiKey_Delete;
        if (control == "Tab")
            return ImGuiKey_Tab;
        if (control == "Space")
            return ImGuiKey_Space;
        if (control == "Left")
            return ImGuiKey_LeftArrow;
        if (control == "Right")
            return ImGuiKey_RightArrow;
        if (control == "Up")
            return ImGuiKey_UpArrow;
        if (control == "Down")
            return ImGuiKey_DownArrow;
        if (control == "Home")
            return ImGuiKey_Home;
        if (control == "End")
            return ImGuiKey_End;
        return ImGuiKey_None;
    };
    for (const auto &event : input.events()) {
        using Type = input::InputEventType;
        switch (event.type) {
        case Type::KeyDown:
        case Type::KeyUp:
            if (const auto k = key(event.control); k != ImGuiKey_None)
                io.AddKeyEvent(k, event.type == Type::KeyDown);
            break;
        case Type::TextInput:
            io.AddInputCharactersUTF8(event.text.c_str());
            break;
        case Type::MouseMove:
            io.AddMousePosEvent(event.position.x, event.position.y);
            break;
        case Type::MouseButtonDown:
        case Type::MouseButtonUp: {
            io.AddMousePosEvent(event.position.x, event.position.y);
            int button = event.control == "mouse:right"    ? 1
                         : event.control == "mouse:middle" ? 2
                                                           : 0;
            io.AddMouseButtonEvent(button, event.type == Type::MouseButtonDown);
            break;
        }
        case Type::MouseWheel:
            io.AddMouseWheelEvent(event.delta.x, event.delta.y);
            break;
        case Type::FocusLost:
            io.AddFocusEvent(false);
            break;
        default:
            break;
        }
    }
}
void EditorImGuiView::shutdown(render::Renderer *renderer) {
    pointerCapture_.end();
    if (renderer) {
        for (auto &view : worldViews_)
            view.release(*renderer);
        for (const auto &[key, image] : images_) {
            (void)key;
            renderer->destroy_resource(image.texture);
        }
        renderer->set_imgui_draw_data(nullptr);
        renderer->shutdown_imgui();
    }
    images_.clear();
    if (ImGui::GetCurrentContext())
        ImGui::SaveIniSettingsToDisk(iniPath_.c_str());
    if (plotOwned_) {
        ImPlot::DestroyContext();
        plotOwned_ = false;
    }
}
} // namespace shinkou::editor
#else
namespace shinkou::editor {
void EditorImGuiView::initialize(EditorLayer &) {
}
void EditorImGuiView::apply_theme(EditorLayer &) {
}
void EditorImGuiView::process_input(const input::InputSystem &) {
}
void EditorImGuiView::draw(EditorLayer &, render::Renderer &, World &, float, std::uint64_t) {
}
void EditorImGuiView::shutdown(render::Renderer *) {
}
} // namespace shinkou::editor
#endif
