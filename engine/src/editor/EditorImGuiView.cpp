#include "EditorImGuiView.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#include "EditorGlyphs.h"

#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <ImGuizmo.h>
#include <hello_imgui/imgui_theme.h>
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <backends/imgui_impl_win32.h>
#endif
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cfloat>
#include <cstdlib>

namespace shinkou::editor {
const char* EditorImGuiView::label(const EditorLayer& e, const char* zh, const char* en) const {
    return e.layout_.language == "zh-CN" ? zh : en;
}
std::string EditorImGuiView::title(const EditorLayer& e, const char* id) const {
    const std::string pageId = std::string(id)=="viewport" ? "scene" : std::string(id)=="assets" ? "project" : id;
    for (const auto& page : e.uiModel_.pages())
        if (page.id == pageId) return std::string(EditorLocalization(e.layout_.language).text(page.title)) + "###" + id;
    return std::string(id) + "###" + id;
}
void EditorImGuiView::region(const EditorLayer& e, const std::string& id) {
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    region_rect(e,id,{a.x,a.y,b.x-a.x,b.y-a.y});
    if (ImGui::IsItemFocused() || ImGui::IsItemActive()) focused_ = id;
}
void EditorImGuiView::region_rect(const EditorLayer& e,const std::string& id,ui::Rect screen) {
    const float s = std::max(0.25f, e.displayDpiScale_ * e.layout_.uiScale);
    const auto origin=ImGui::GetMainViewport()->Pos;
    const auto* surface=ImGui::GetWindowViewport();
    regions_[id] = {(screen.x-origin.x)/s,(screen.y-origin.y)/s,screen.width/s,screen.height/s};
    regionSurfaces_[id]={reinterpret_cast<std::uintptr_t>(surface->PlatformHandleRaw),
        {screen.x-surface->Pos.x,screen.y-surface->Pos.y,screen.width,screen.height}};
}
void EditorImGuiView::alias_region(const std::string& alias,const std::string& source) {
    regions_[alias]=regions_.at(source);regionSurfaces_[alias]=regionSurfaces_.at(source);
}
bool EditorImGuiView::button(EditorLayer& e, const char* zh, const char* en, const char* id) {
    ImGui::PushID(id);
    const std::string key=id;
    const char* icon=nullptr;
    if(key=="toolbar.play")icon=e.uiModel_.playing()?glyph::Stop:glyph::Play;
    else if(key=="toolbar.pause")icon=e.uiModel_.paused()?glyph::Play:glyph::Pause;
    else if(key=="toolbar.step")icon=glyph::Step;
    else if(key=="toolbar.save" || key=="settings.save")icon=glyph::Save;
    else if(key=="toolbar.build")icon=glyph::Build;
    else if(key=="toolbar.panels")icon=glyph::Layout;
    else if(key=="command:create-empty")icon=glyph::Add;
    else if(key=="asset-parent")icon=glyph::Up;
    else if(key=="asset-refresh")icon=glyph::Refresh;
    if(!iconsReady_)icon=nullptr;
    const bool clicked = ImGui::Button(icon?icon:label(e, zh, en));
    if(icon && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s",label(e,zh,en));
    region(e, id);
    if (std::string(id).rfind("toolbar.",0)==0) alias_region("command:"+std::string(id).substr(8),id);
    ImGui::PopID();
    return clicked;
}
bool EditorImGuiView::begin_panel(EditorLayer& e, const char* id, int flags) {
    const bool panelVisible=e.panel_visible(id);
    const auto [it,first]=panelWasVisible_.try_emplace(id,panelVisible);
    if (!first && !it->second && panelVisible) focusPanel_=id;
    it->second=panelVisible;
    if (!panelVisible) return false;
    if (focusPanel_==id) { ImGui::SetNextWindowFocus(); focusPanel_.clear(); }
    if (flags & ImGuiWindowFlags_NoBackground) {
        // A docked window still needs to publish a transparent node background.
        flags &= ~ImGuiWindowFlags_NoBackground;
        ImGui::SetNextWindowBgAlpha(0);
    }
    bool open = true;
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet=ImGuiDockNodeFlags_NoCloseButton|ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::SetNextWindowClass(&windowClass);
    const bool visible = ImGui::Begin(title(e,id).c_str(), &open, flags|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoCollapse);
    if (const auto* node=ImGui::GetCurrentWindow()->DockNode) if (const auto* bar=node->TabBar) {
        for (const auto& tab : bar->Tabs) if (tab.Window==ImGui::GetCurrentWindow()) {
            region_rect(e,std::string("tab:")+id,{bar->BarRect.Min.x+tab.Offset,bar->BarRect.Min.y,tab.Width,bar->BarRect.GetHeight()});
        }
    }
    if (!ImGui::GetCurrentWindow()->DockNode)
        region_rect(e,std::string("tab:")+id,{ImGui::GetWindowPos().x,ImGui::GetWindowPos().y,ImGui::GetWindowWidth(),ImGui::GetFrameHeight()});
    if (!open) {e.finish_inspector_edit();e.set_panel_visible(id, false);}
    if (!visible) ImGui::End();
    return visible;
}
void EditorImGuiView::initialize(EditorLayer& e) {
    iniPath_ = e.layout_.layoutFile + ".imgui.ini";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(iniPath_).parent_path(), ec);
    auto& io = ImGui::GetIO();
    io.IniFilename = iniPath_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigViewportsNoTaskBarIcon=false;
    io.ConfigDockingWithShift = false;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    rebuild_fonts(e);
    ImPlot::CreateContext();
    plotOwned_ = true;
    apply_theme(e);
}
void EditorImGuiView::rebuild_fonts(EditorLayer& e) {
    auto& io=ImGui::GetIO();
    io.Fonts->Clear();
    io.FontDefault=nullptr;
    fontScale_ = std::clamp(e.displayDpiScale_ * e.layout_.uiScale, 0.5f, 4.0f);
    fontSize_=std::clamp(e.layout_.fontSize,12.0f,32.0f);
    fontPath_=e.layout_.fontPath;
    std::vector<std::filesystem::path> fonts;
    if (!e.layout_.fontPath.empty()) fonts.emplace_back(e.layout_.fontPath);
    fonts.emplace_back("C:/Windows/Fonts/msyh.ttc");
    fonts.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    for (const auto& font : fonts) {
        ImFontConfig config;
        config.OversampleH=1; config.OversampleV=1;
        if (std::filesystem::exists(font) && io.Fonts->AddFontFromFileTTF(
                font.string().c_str(), fontSize_ * fontScale_, &config, io.Fonts->GetGlyphRangesChineseFull())) break;
    }
    if (io.Fonts->Fonts.empty()) { ImFontConfig config; config.SizePixels=fontSize_*fontScale_;io.Fonts->AddFontDefault(&config); }
    static const ImWchar iconRanges[]={0xea60,0xefff,0};
    std::filesystem::path iconFont=SHINKOU_EDITOR_ICON_FONT_SOURCE;
    if(!std::filesystem::exists(iconFont))iconFont=std::filesystem::path(SHINKOU_EDITOR_ICON_INSTALL_DIR).parent_path()/"fonts/codicon.ttf";
    iconsReady_=false;
    if (std::filesystem::exists(iconFont)) {
        ImFontConfig config;config.MergeMode=true;config.PixelSnapH=true;config.OversampleH=1;config.OversampleV=1;
        config.GlyphMinAdvanceX=fontSize_*fontScale_;
        iconsReady_=io.Fonts->AddFontFromFileTTF(iconFont.string().c_str(),fontSize_*fontScale_,&config,iconRanges)!=nullptr;
    }
    io.Fonts->Build();
    io.FontGlobalScale=1.0f;
}
void EditorImGuiView::apply_theme(EditorLayer& e) {
    const float nextScale = std::clamp(e.displayDpiScale_ * e.layout_.uiScale, 0.5f, 4.0f);
    if (theme_ == e.layout_.theme && std::abs(scale_ - nextScale) < 0.001f) return;
    theme_ = e.layout_.theme;
    scale_ = nextScale;
    // Use the library's complete style, reset before scaling to avoid cumulative DPI drift.
    auto theme = ImGuiTheme::ImGuiTheme_PhotoshopStyle;
    if (theme_ == "light") theme = ImGuiTheme::ImGuiTheme_MicrosoftStyle;
    if (theme_ == "high-contrast") theme = ImGuiTheme::ImGuiTheme_BlackIsBlack;
    ImGui::GetStyle() = ImGuiTheme::ThemeToStyle(theme);
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {8, 6};
    style.FramePadding = {6, 3};
    style.ItemSpacing = {6, 5};
    style.CellPadding = {3, 3};
    style.TabCloseButtonMinWidthSelected=FLT_MAX;
    style.TabCloseButtonMinWidthUnselected=FLT_MAX;
    style.WindowRounding = 0;
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.TabRounding = 3;
    style.DockingSeparatorSize = 5;
    style.ScaleAllSizes(scale_);
    ImGui::GetIO().FontGlobalScale = 1.0f;
    if (plotOwned_) {
        if (theme_ == "light") ImPlot::StyleColorsLight();
        else ImPlot::StyleColorsDark();
    }
}
void EditorImGuiView::shortcuts(EditorLayer& e, World& world) {
    const auto command = [&](EditorCommand cmd) { e.dispatch_command(cmd, {}, world); };
    const auto& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) command(io.KeyShift ? EditorCommand::SaveSceneAs : EditorCommand::SaveScene);
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) command(EditorCommand::Undo);
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) command(EditorCommand::Redo);
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) command(EditorCommand::NewScene);
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) command(EditorCommand::OpenScene);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) command(EditorCommand::FrameSelection);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && focused_.rfind("hierarchy:", 0)==0)
        command(EditorCommand::DeleteObject);
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) operation_ = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) operation_ = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) operation_ = 2;
}
void EditorImGuiView::workspace(EditorLayer& e, render::Renderer&, World& world) {
    const auto command = [&](EditorCommand cmd) { e.dispatch_command(cmd, {}, world); };
    auto* main = ImGui::GetMainViewport();
    float top = main->WorkPos.y;
    if (!e.nativeMenu_ && ImGui::BeginMainMenuBar()) {
        for (const auto& menu : e.uiModel_.menus()) {
            const EditorLocalization localization(e.layout_.language);
            const auto menuLabel=std::string(localization.text(menu.label));
            if (!ImGui::BeginMenu(menuLabel.c_str())) continue;
            for (const auto& item : menu.items) {
                if (item.separator) ImGui::Separator();
                else if (ImGui::MenuItem(std::string(localization.text(item.label)).c_str(), item.shortcut.c_str(), false, item.enabled))
                    e.dispatch_command(item.command, item.target, world);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    top=main->WorkPos.y;
    const int fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    const float toolbarHeight = e.layout_.showToolbar ? 34.0f * scale_ : 0;
    if (e.layout_.showToolbar) {
        ImGui::SetNextWindowViewport(main->ID);
        ImGui::SetNextWindowPos({main->WorkPos.x, top});
        ImGui::SetNextWindowSize({main->WorkSize.x, toolbarHeight});
        ImGui::Begin("###editor-toolbar", nullptr, fixed);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("SHINKOU");
        ImGui::SameLine(); ImGui::TextDisabled("/ %s", e.fileSystem_.root().filename().string().c_str());
        ImGui::SameLine(std::max(230.0f*scale_, main->WorkSize.x*0.35f));
        if (button(e, e.uiModel_.playing()?"停止":"运行", e.uiModel_.playing()?"Stop":"Play", "toolbar.play")) command(EditorCommand::Play);
        ImGui::SameLine();
        ImGui::BeginDisabled(!e.uiModel_.playing());
        if (button(e, e.uiModel_.paused()?"继续":"暂停", e.uiModel_.paused()?"Resume":"Pause", "toolbar.pause")) command(EditorCommand::Pause);
        ImGui::SameLine();
        if (button(e, "单步", "Step", "toolbar.step")) command(EditorCommand::Step);
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
        if (button(e,"保存","Save","toolbar.save")) command(EditorCommand::SaveScene);
        ImGui::SameLine();
        if (button(e,"构建","Build","toolbar.build")) { e.set_panel_visible("build",true); command(EditorCommand::BuildProject); }
        ImGui::SameLine();
        if (button(e,"面板","Panels","toolbar.panels")) ImGui::OpenPopup("workspace-panels");
        if (ImGui::BeginPopup("workspace-panels")) {
            for (const auto& panel : e.panels_) {
                const auto text=title(e,panel.id.c_str());
                if (ImGui::MenuItem(text.c_str(),nullptr,e.panel_visible(panel.id))) {
                    const bool show=!e.panel_visible(panel.id);e.set_panel_visible(panel.id,show);if(show)focusPanel_=panel.id;
                }
                region(e,"window:"+panel.id);
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }
    const float statusHeight = e.layout_.showStatusBar ? 21.0f*scale_ : 0;
    ImGui::SetNextWindowViewport(main->ID);
    ImGui::SetNextWindowPos({main->WorkPos.x, top + toolbarHeight});
    ImGui::SetNextWindowSize({main->WorkSize.x, std::max(1.0f, main->WorkSize.y - toolbarHeight - statusHeight - (top-main->WorkPos.y))});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("###editor-dock-host", nullptr, fixed | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    const auto dock = ImGui::GetID("ShinkouWorkspace");
    const auto dockSize=ImGui::GetContentRegionAvail();
    if (auto* root=ImGui::DockBuilderGetNode(dock)) {
        if (dockSize_.x!=dockSize.x || dockSize_.y!=dockSize.y) {
            // ImGui normally gives all resize delta to the central node.
            // Preserve user split proportions when changing monitor/window
            // resolution, so fixed sidebars cannot squeeze the world to zero.
            const auto scaleSplits=[&](const auto& self,ImGuiDockNode* node,ImVec2 size)->void {
                if (!node || !node->ChildNodes[0] || !node->ChildNodes[1]) return;
                auto* first=node->ChildNodes[0];auto* second=node->ChildNodes[1];
                const int axis=node->SplitAxis;
                const float sum=first->SizeRef[axis]+second->SizeRef[axis];
                const float ratio=sum>0 ? first->SizeRef[axis]/sum : 0.5f;
                const float available=std::max(2.0f,size[axis]-ImGui::GetStyle().DockingSeparatorSize);
                ImVec2 a=size,b=size;a[axis]=std::max(1.0f,std::floor(available*ratio));b[axis]=std::max(1.0f,available-a[axis]);
                first->SizeRef=a;second->SizeRef=b;
                first->WantLockSizeOnce=second->WantLockSizeOnce=false;
                self(self,first,a);self(self,second,b);
            };
            scaleSplits(scaleSplits,root,dockSize);
        }
    }
    dockSize_={dockSize.x,dockSize.y};
    if (!ImGui::DockBuilderGetNode(dock) || resetDock_) {
        resetDock_ = false;
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetContentRegionAvail());
        auto center = dock;
        const auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
        const auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
        const auto bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.31f, nullptr, &center);
        ImGui::DockBuilderDockWindow(title(e,"hierarchy").c_str(),left);
        ImGui::DockBuilderDockWindow(title(e,"inspector").c_str(),right);
        for (const auto* id : {"settings", "recovery", "media"}) ImGui::DockBuilderDockWindow(title(e,id).c_str(),right);
        for (const auto* id : {"assets", "console", "profiler", "render-graph", "build"})
            ImGui::DockBuilderDockWindow(title(e,id).c_str(),bottom);
        ImGui::DockBuilderDockWindow(title(e,"viewport").c_str(),center);
        ImGui::DockBuilderDockWindow(title(e,"game").c_str(),center);
        ImGui::DockBuilderFinish(dock);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0,0,0,0));
    ImGui::DockSpace(dock, {}, ImGuiDockNodeFlags_PassthruCentralNode|ImGuiDockNodeFlags_NoCloseButton|ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::PopStyleColor();
    ImGui::End();
    if (e.layout_.showStatusBar) {
        ImGui::SetNextWindowViewport(main->ID);
        ImGui::SetNextWindowPos({main->WorkPos.x, main->WorkPos.y+main->WorkSize.y-statusHeight});
        ImGui::SetNextWindowSize({main->WorkSize.x,statusHeight});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{10*scale_,4*scale_});
        ImGui::Begin("###editor-status",nullptr,fixed);
        ImGui::TextUnformatted(e.lastStatus_.empty()?label(e,"就绪","Ready"):e.lastStatus_.c_str());
        if (main->WorkSize.x > 750*scale_) {
            ImGui::SameLine(main->WorkSize.x-240*scale_);
            ImGui::TextDisabled("%.1f ms | %llu draws", e.frameTimes_.empty()?0:e.frameTimes_.back(), static_cast<unsigned long long>(frameDrawCalls_));
        }
        ImGui::End(); ImGui::PopStyleVar();
    }
}
void EditorImGuiView::draw(EditorLayer& e, render::Renderer& renderer, World& world, float dt, std::uint64_t frame) {
    const float desiredScale=std::clamp(e.displayDpiScale_*e.layout_.uiScale,0.5f,4.0f);
    if (std::abs(fontScale_-desiredScale)>0.001f || fontSize_!=std::clamp(e.layout_.fontSize,12.0f,32.0f) || fontPath_!=e.layout_.fontPath) {
        renderer.invalidate_imgui_fonts();
        rebuild_fonts(e);
    }
    apply_theme(e);
    if (!renderer.new_imgui_frame()) {
        e.push_console("ImGui renderer initialization failed: " + renderer.last_error());
        return;
    }
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (e.imguiPlatformInitialized_) ImGui_ImplWin32_NewFrame();
#endif
    auto& io = ImGui::GetIO();
    // Both platform and renderer must implement secondary windows. DX11 is the
    // editor's verified world-rendering backend; other backends stay dockable.
    if (renderer.capabilities().supportsEditorViewportScissor && (io.BackendFlags & (ImGuiBackendFlags_PlatformHasViewports|ImGuiBackendFlags_RendererHasViewports)) ==
        (ImGuiBackendFlags_PlatformHasViewports|ImGuiBackendFlags_RendererHasViewports))
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    static const bool targetedQaMouse=std::getenv("SHINKOU_UI_QA_TARGETED_MOUSE")&&std::getenv("SHINKOU_UI_QA_SNAPSHOT");
    // The official geometry fallback uses the posted mouse coordinates when
    // unattended QA cannot move the hardware cursor. Production keeps Win32's
    // WindowFromPoint-based viewport detection unchanged.
    if(targetedQaMouse)io.BackendFlags &= ~ImGuiBackendFlags_HasMouseHoveredViewport;
    io.DisplaySize = {e.displayWidth_,e.displayHeight_};
    io.DeltaTime = std::max(0.001f,dt);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    const auto draws=renderer.stats().drawCalls;
    frameDrawCalls_=draws>=lastDrawCalls_ ? draws-lastDrawCalls_ : draws;
    lastDrawCalls_=draws;
    regions_.clear();
    regionSurfaces_.clear();
    viewportVisible_ = false;
    shortcuts(e,world);
    workspace(e,renderer,world);
    hierarchy(e,world);
    inspector(e,renderer,world);
    assets(e);
    viewport(e,renderer,world,false);
    viewport(e,renderer,world,true);
    console(e);
    profiler(e,renderer);
    render_graph(e,renderer);
    settings(e);
    build(e,world);
    media(e,renderer,world);
    recovery(e,world);
    e.draw_registered_panels(renderer,world,frame,dt);
    if (!viewportVisible_) renderer.clear_editor_viewport();
    ImGui::Render();
    drawCommands_ = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists) drawCommands_ += list->CmdBuffer.Size;
    renderer.set_editor_ui_render_list(nullptr);
    renderer.set_imgui_draw_data(ImGui::GetDrawData());
}
void EditorImGuiView::shutdown(render::Renderer* renderer) {
    if (renderer) {
        for (auto& view:worldViews_) view.release(*renderer);
        for (const auto& [key, image] : images_) { (void)key; renderer->destroy_resource(image.texture); }
        renderer->set_imgui_draw_data(nullptr);
        renderer->shutdown_imgui();
    }
    images_.clear();
    if (ImGui::GetCurrentContext()) ImGui::SaveIniSettingsToDisk(iniPath_.c_str());
    if (plotOwned_) { ImPlot::DestroyContext(); plotOwned_ = false; }
}
}
#else
namespace shinkou::editor {
void EditorImGuiView::initialize(EditorLayer&) {}
void EditorImGuiView::apply_theme(EditorLayer&) {}
void EditorImGuiView::draw(EditorLayer&,render::Renderer&,World&,float,std::uint64_t) {}
void EditorImGuiView::shutdown(render::Renderer*) {}
}
#endif
