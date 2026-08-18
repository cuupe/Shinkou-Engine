#include "shinkou/editor/EditorLayer.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/reflection/Serialization.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#endif

#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <backends/imgui_impl_win32.h>
#endif
#endif

namespace shinkou::editor {
namespace {

constexpr std::string_view kDarkTheme{"dark"};
constexpr std::string_view kLightTheme{"light"};
constexpr std::string_view kHighContrastTheme{"high-contrast"};

#if defined(SHINKOU_PLATFORM_WINDOWS)
enum NativeEditorMenuId : UINT {
    kNativeNewScene = 41001,
    kNativeOpenScene,
    kNativeSaveScene,
    kNativeExit,
    kNativeUndo,
    kNativeRedo,
    kNativeRefreshAssets,
    kNativeCreateEmpty,
    kNativeCreateChild,
    kNativeCreate3D,
    kNativeCreate2D,
    kNativeCreateUi,
    kNativeAddComponent,
    kNativeScenePage,
    kNativeGamePage,
    kNativeHierarchyPage,
    kNativeInspectorPage,
    kNativeProjectPage,
    kNativeConsolePage,
    kNativeProfilerPage,
    kNativeRenderGraphPage,
    kNativeDarkTheme,
    kNativeLightTheme,
    kNativeHighContrastTheme,
    kNativeAbout,
};

void append_native_item(HMENU menu, UINT id, const char* label) {
    AppendMenuA(menu, MF_STRING, id, label);
}

void append_native_submenu(HMENU menu, HMENU submenu, const char* label) {
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), label);
}
#endif

void register_layout_types(reflection::TypeRegistry& registry) {
    std::string ignored;
    registry.register_type(reflection::TypeBuilder<EditorLayoutState>("shinkou.editor.EditorLayoutState")
        .field("layoutVersion", &EditorLayoutState::layoutVersion)
        .field("theme", &EditorLayoutState::theme)
        .field("workspace", &EditorLayoutState::workspace)
        .field("projectRoot", &EditorLayoutState::projectRoot)
        .field("layoutFile", &EditorLayoutState::layoutFile)
        .field("selectedObject", &EditorLayoutState::selectedObject)
        .field("showHierarchy", &EditorLayoutState::showHierarchy)
        .field("showInspector", &EditorLayoutState::showInspector)
        .field("showViewport", &EditorLayoutState::showViewport)
        .field("showGame", &EditorLayoutState::showGame)
        .field("showAssets", &EditorLayoutState::showAssets)
        .field("showConsole", &EditorLayoutState::showConsole)
        .field("showProfiler", &EditorLayoutState::showProfiler)
        .field("showRenderGraph", &EditorLayoutState::showRenderGraph)
        .field("showSettings", &EditorLayoutState::showSettings)
        .take(), &ignored);
}

[[maybe_unused]] const char* backend_name(render::BackendApi api) noexcept {
    switch (api) {
    case render::BackendApi::DirectX11: return "DirectX 11";
    case render::BackendApi::DirectX12: return "DirectX 12";
    case render::BackendApi::Vulkan: return "Vulkan";
    default: return "Null";
    }
}

[[maybe_unused]] const char* device_state_name(render::RenderDeviceState state) noexcept {
    switch (state) {
    case render::RenderDeviceState::Ready: return "Ready";
    case render::RenderDeviceState::NeedsResize: return "Needs resize";
    case render::RenderDeviceState::Lost: return "Lost";
    default: return "Uninitialized";
    }
}

#if defined(SHINKOU_WITH_IMGUI)
void apply_vec2_property(const PropertyDescriptor& property, const math::Vec2& value) {
    float values[2]{value.x, value.y};
    if (ImGui::DragFloat2(property.displayName.c_str(), values, static_cast<float>(property.step))) {
        property.set(PropertyValue{math::Vec2{values[0], values[1]}});
    }
}

void apply_vec3_property(const PropertyDescriptor& property, const math::Vec3& value) {
    float values[3]{value.x, value.y, value.z};
    if (ImGui::DragFloat3(property.displayName.c_str(), values, static_cast<float>(property.step))) {
        property.set(PropertyValue{math::Vec3{values[0], values[1], values[2]}});
    }
}

void draw_property(const PropertyDescriptor& property) {
    const auto value = property.get ? property.get() : PropertyValue{};
    const bool disabled = !property.editable();
    if (disabled) ImGui::BeginDisabled();
    if (const auto* boolean = std::get_if<bool>(&value)) {
        bool next = *boolean;
        if (ImGui::Checkbox(property.displayName.c_str(), &next) && property.editable()) property.set(PropertyValue{next});
    } else if (const auto* number = std::get_if<double>(&value)) {
        double next = *number;
        if (ImGui::DragScalar(property.displayName.c_str(), ImGuiDataType_Double, &next,
                              property.step > 0.0 ? static_cast<float>(property.step) : 0.01f,
                              property.minimum < property.maximum ? &property.minimum : nullptr,
                              property.minimum < property.maximum ? &property.maximum : nullptr) && property.editable()) {
            property.set(PropertyValue{next});
        }
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        std::int64_t next = *integer;
        if (ImGui::InputScalar(property.displayName.c_str(), ImGuiDataType_S64, &next) && property.editable()) property.set(PropertyValue{next});
    } else if (const auto* unsignedInteger = std::get_if<std::uint64_t>(&value)) {
        std::uint64_t next = *unsignedInteger;
        if (ImGui::InputScalar(property.displayName.c_str(), ImGuiDataType_U64, &next) && property.editable()) property.set(PropertyValue{next});
    } else if (const auto* string = std::get_if<std::string>(&value)) {
        std::array<char, 256> buffer{};
        std::strncpy(buffer.data(), string->c_str(), buffer.size() - 1);
        if (ImGui::InputText(property.displayName.c_str(), buffer.data(), buffer.size()) && property.editable()) {
            property.set(PropertyValue{std::string(buffer.data())});
        }
    } else if (const auto* vec2 = std::get_if<math::Vec2>(&value)) {
        apply_vec2_property(property, *vec2);
    } else if (const auto* vec3 = std::get_if<math::Vec3>(&value)) {
        apply_vec3_property(property, *vec3);
    } else if (const auto* quat = std::get_if<math::Quat>(&value)) {
        float values[4]{quat->x, quat->y, quat->z, quat->w};
        if (ImGui::DragFloat4(property.displayName.c_str(), values, 0.01f) && property.editable())
            property.set(PropertyValue{math::Quat{values[0], values[1], values[2], values[3]}});
    } else {
        ImGui::TextDisabled("%s: unsupported property", property.displayName.c_str());
    }
    if (disabled) ImGui::EndDisabled();
}
#endif

} // namespace

bool EditorLayer::initialize() {
    if (initialized_) return true;
    register_builtin_panels();
    load_layout_file();
    sync_page_visibility();
    refresh_asset_cache();
    install_native_menu();
#if defined(SHINKOU_WITH_IMGUI)
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        uiContextOwned_ = true;
    }
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#if defined(IMGUI_HAS_DOCK)
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
    if (io.Fonts && !io.Fonts->IsBuilt()) io.Fonts->Build();
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (nativeWindow_ && !ImGui_ImplWin32_Init(nativeWindow_)) {
        push_console("ImGui Win32 platform initialization failed");
    }
#endif
    apply_theme();
#endif
    initialized_ = true;
    push_console("Editor UI initialized");
    return true;
}

void EditorLayer::install_native_menu() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (!nativeWindow_ || nativeMenu_) return;
    const HWND window = static_cast<HWND>(nativeWindow_);
    HMENU mainMenu = CreateMenu();
    if (!mainMenu) return;

    HMENU fileMenu = CreatePopupMenu();
    append_native_item(fileMenu, kNativeNewScene, "New Scene");
    append_native_item(fileMenu, kNativeOpenScene, "Open Scene...");
    append_native_item(fileMenu, kNativeSaveScene, "Save Scene");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    append_native_item(fileMenu, kNativeExit, "Exit");
    append_native_submenu(mainMenu, fileMenu, "File");

    HMENU editMenu = CreatePopupMenu();
    append_native_item(editMenu, kNativeUndo, "Undo");
    append_native_item(editMenu, kNativeRedo, "Redo");
    append_native_submenu(mainMenu, editMenu, "Edit");

    HMENU assetsMenu = CreatePopupMenu();
    append_native_item(assetsMenu, kNativeRefreshAssets, "Refresh");
    append_native_submenu(mainMenu, assetsMenu, "Assets");

    HMENU gameObjectMenu = CreatePopupMenu();
    append_native_item(gameObjectMenu, kNativeCreateEmpty, "Create Empty");
    append_native_item(gameObjectMenu, kNativeCreateChild, "Create Child");
    HMENU create3dMenu = CreatePopupMenu();
    append_native_item(create3dMenu, kNativeCreate3D, "3D Object");
    append_native_submenu(gameObjectMenu, create3dMenu, "3D Object");
    HMENU create2dMenu = CreatePopupMenu();
    append_native_item(create2dMenu, kNativeCreate2D, "2D Object");
    append_native_submenu(gameObjectMenu, create2dMenu, "2D Object");
    append_native_item(gameObjectMenu, kNativeCreateUi, "UI Object");
    append_native_submenu(mainMenu, gameObjectMenu, "GameObject");

    HMENU componentMenu = CreatePopupMenu();
    append_native_item(componentMenu, kNativeAddComponent, "Add Component");
    append_native_submenu(mainMenu, componentMenu, "Component");

    HMENU windowMenu = CreatePopupMenu();
    append_native_item(windowMenu, kNativeScenePage, "Scene");
    append_native_item(windowMenu, kNativeGamePage, "Game");
    append_native_item(windowMenu, kNativeHierarchyPage, "Hierarchy");
    append_native_item(windowMenu, kNativeInspectorPage, "Inspector");
    append_native_item(windowMenu, kNativeProjectPage, "Project");
    append_native_item(windowMenu, kNativeConsolePage, "Console");
    append_native_item(windowMenu, kNativeProfilerPage, "Profiler");
    append_native_item(windowMenu, kNativeRenderGraphPage, "Render Graph");
    append_native_submenu(mainMenu, windowMenu, "Window");

    HMENU themeMenu = CreatePopupMenu();
    append_native_item(themeMenu, kNativeDarkTheme, "Dark");
    append_native_item(themeMenu, kNativeLightTheme, "Light");
    append_native_item(themeMenu, kNativeHighContrastTheme, "High Contrast");
    append_native_submenu(mainMenu, themeMenu, "Theme");

    HMENU helpMenu = CreatePopupMenu();
    append_native_item(helpMenu, kNativeAbout, "About ShinkouEngine");
    append_native_submenu(mainMenu, helpMenu, "Help");

    SetMenu(window, mainMenu);
    DrawMenuBar(window);
    nativeMenu_ = mainMenu;
#endif
}

void EditorLayer::uninstall_native_menu() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (!nativeWindow_ || !nativeMenu_) return;
    const HWND window = static_cast<HWND>(nativeWindow_);
    SetMenu(window, nullptr);
    DestroyMenu(static_cast<HMENU>(nativeMenu_));
    nativeMenu_ = nullptr;
#endif
}

void EditorLayer::handle_native_menu_command(std::uint32_t command, World& world) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    switch (command) {
    case kNativeNewScene: dispatch_command(EditorCommand::NewScene, {}, world); break;
    case kNativeOpenScene: dispatch_command(EditorCommand::OpenScene, {}, world); break;
    case kNativeSaveScene: dispatch_command(EditorCommand::SaveScene, {}, world); break;
    case kNativeExit: dispatch_command(EditorCommand::Quit, {}, world); break;
    case kNativeUndo: dispatch_command(EditorCommand::Undo, {}, world); break;
    case kNativeRedo: dispatch_command(EditorCommand::Redo, {}, world); break;
    case kNativeRefreshAssets: dispatch_command(EditorCommand::RefreshAssets, {}, world); break;
    case kNativeCreateEmpty: dispatch_command(EditorCommand::CreateEmpty, {}, world); break;
    case kNativeCreateChild: dispatch_command(EditorCommand::CreateChild, {}, world); break;
    case kNativeCreate3D: dispatch_command(EditorCommand::Create3DObject, {}, world); break;
    case kNativeCreate2D: dispatch_command(EditorCommand::Create2DObject, {}, world); break;
    case kNativeCreateUi: dispatch_command(EditorCommand::CreateUiObject, {}, world); break;
    case kNativeAddComponent: dispatch_command(EditorCommand::AddComponent, {}, world); break;
    case kNativeScenePage: dispatch_command(EditorCommand::TogglePage, "scene", world); break;
    case kNativeGamePage: dispatch_command(EditorCommand::TogglePage, "game", world); break;
    case kNativeHierarchyPage: dispatch_command(EditorCommand::TogglePage, "hierarchy", world); break;
    case kNativeInspectorPage: dispatch_command(EditorCommand::TogglePage, "inspector", world); break;
    case kNativeProjectPage: dispatch_command(EditorCommand::TogglePage, "project", world); break;
    case kNativeConsolePage: dispatch_command(EditorCommand::TogglePage, "console", world); break;
    case kNativeProfilerPage: dispatch_command(EditorCommand::TogglePage, "profiler", world); break;
    case kNativeRenderGraphPage: dispatch_command(EditorCommand::TogglePage, "render-graph", world); break;
    case kNativeDarkTheme: dispatch_command(EditorCommand::SetDarkTheme, {}, world); break;
    case kNativeLightTheme: dispatch_command(EditorCommand::SetLightTheme, {}, world); break;
    case kNativeHighContrastTheme: dispatch_command(EditorCommand::SetHighContrastTheme, {}, world); break;
    case kNativeAbout: push_console("ShinkouEngine Editor"); break;
    default: break;
    }
#else
    (void)command;
    (void)world;
#endif
}

void EditorLayer::register_builtin_panels() {
    if (!panels_.empty()) return;
    const auto add = [this](std::string id, std::string title, bool visible) {
        panels_.push_back(EditorPanel{std::move(id), std::move(title), visible, {}});
        panelVisibility_[panels_.back().id] = visible;
    };
    add("hierarchy", "Hierarchy", true);
    add("inspector", "Inspector", true);
    add("viewport", "Viewport", true);
    add("game", "Game", false);
    add("assets", "Asset Browser", true);
    add("console", "Console", true);
    add("profiler", "Profiler", false);
    add("render-graph", "Render Graph", false);
    add("settings", "Editor Settings", false);
}

void EditorLayer::sync_page_visibility() noexcept {
    uiModel_.set_page_visible("scene", layout_.showViewport);
    uiModel_.set_page_visible("game", layout_.showGame);
    uiModel_.set_page_visible("hierarchy", layout_.showHierarchy);
    uiModel_.set_page_visible("inspector", layout_.showInspector);
    uiModel_.set_page_visible("project", layout_.showAssets);
    uiModel_.set_page_visible("console", layout_.showConsole);
    uiModel_.set_page_visible("profiler", layout_.showProfiler);
    uiModel_.set_page_visible("render-graph", layout_.showRenderGraph);
    uiModel_.set_page_visible("settings", layout_.showSettings);
}

bool EditorLayer::register_panel(EditorPanel panel) {
    if (panel.id.empty() || panel.title.empty() || !panel.draw || panelVisibility_.find(panel.id) != panelVisibility_.end()) return false;
    panelVisibility_[panel.id] = panel.defaultVisible;
    panels_.push_back(std::move(panel));
    return true;
}

bool EditorLayer::unregister_panel(std::string_view id) {
    const auto it = std::find_if(panels_.begin(), panels_.end(), [id](const EditorPanel& panel) { return panel.id == id; });
    if (it == panels_.end() || it->id == "hierarchy" || it->id == "inspector" || it->id == "viewport") return false;
    panelVisibility_.erase(it->id);
    panels_.erase(it);
    return true;
}

bool EditorLayer::set_panel_visible(std::string_view id, bool visible) {
    const auto it = panelVisibility_.find(std::string(id));
    if (it == panelVisibility_.end()) return false;
    it->second = visible;
    if (id == "hierarchy") layout_.showHierarchy = visible;
    else if (id == "inspector") layout_.showInspector = visible;
    else if (id == "viewport") layout_.showViewport = visible;
    else if (id == "game") layout_.showGame = visible;
    else if (id == "assets") layout_.showAssets = visible;
    else if (id == "console") layout_.showConsole = visible;
    else if (id == "profiler") layout_.showProfiler = visible;
    else if (id == "render-graph") layout_.showRenderGraph = visible;
    else if (id == "settings") layout_.showSettings = visible;
    sync_page_visibility();
    return true;
}

bool EditorLayer::panel_visible(std::string_view id) const noexcept {
    const auto it = panelVisibility_.find(std::string(id));
    return it != panelVisibility_.end() && it->second;
}

void EditorLayer::set_project_root(std::string path) {
    layout_.projectRoot = std::move(path);
    assetsDirty_ = true;
}

void EditorLayer::set_layout_path(std::string path) { layout_.layoutFile = std::move(path); }

void EditorLayer::set_display_size(float width, float height) noexcept {
    displayWidth_ = std::max(width, 1.0f);
    displayHeight_ = std::max(height, 1.0f);
}

bool EditorLayer::save_layout_file() {
    const std::filesystem::path path(layout_.layoutFile);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::string output;
    reflection::TypeRegistry registry;
    register_layout_types(registry);
    const auto result = reflection::serialize_json(registry, reflection::fnv1a("shinkou.editor.EditorLayoutState"), &layout_, output,
        reflection::JsonOptions{true, false, false, 32});
    if (!result) {
        lastStatus_ = "Layout serialization failed: " + result.message;
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        lastStatus_ = "Cannot open layout file: " + path.string();
        return false;
    }
    file << output;
    lastStatus_ = "Layout saved: " + path.string();
    return static_cast<bool>(file);
}

bool EditorLayer::load_layout_file() {
    std::ifstream file(layout_.layoutFile, std::ios::binary);
    if (!file) return false;
    std::stringstream input;
    input << file.rdbuf();
    reflection::TypeRegistry registry;
    register_layout_types(registry);
    EditorLayoutState loaded = layout_;
    const auto result = reflection::deserialize_json(registry, reflection::fnv1a("shinkou.editor.EditorLayoutState"), input.str(), &loaded,
        reflection::JsonOptions{false, false, false, 32});
    if (!result) {
        lastStatus_ = "Layout load failed: " + result.message;
        return false;
    }
    layout_ = std::move(loaded);
    set_panel_visible("hierarchy", layout_.showHierarchy);
    set_panel_visible("inspector", layout_.showInspector);
    set_panel_visible("viewport", layout_.showViewport);
    set_panel_visible("game", layout_.showGame);
    set_panel_visible("assets", layout_.showAssets);
    set_panel_visible("console", layout_.showConsole);
    set_panel_visible("profiler", layout_.showProfiler);
    set_panel_visible("render-graph", layout_.showRenderGraph);
    set_panel_visible("settings", layout_.showSettings);
    lastStatus_ = "Layout loaded: " + layout_.layoutFile;
    return true;
}

bool EditorLayer::save_layout() {
#if defined(SHINKOU_WITH_IMGUI)
    const auto imguiPath = layout_.layoutFile + ".imgui.ini";
    if (ImGui::GetCurrentContext()) ImGui::SaveIniSettingsToDisk(imguiPath.c_str());
#endif
    return save_layout_file();
}

bool EditorLayer::load_layout() {
    const bool loaded = load_layout_file();
#if defined(SHINKOU_WITH_IMGUI)
    const auto imguiPath = layout_.layoutFile + ".imgui.ini";
    if (loaded && ImGui::GetCurrentContext()) ImGui::LoadIniSettingsFromDisk(imguiPath.c_str());
    apply_theme();
#endif
    return loaded;
}

void EditorLayer::reset_layout() {
    layout_ = EditorLayoutState{};
    for (const auto& panel : panels_) panelVisibility_[panel.id] = panel.defaultVisible;
    sync_page_visibility();
    lastStatus_ = "Layout reset";
#if defined(SHINKOU_WITH_IMGUI)
    apply_theme();
#endif
}

void EditorLayer::set_theme(std::string theme) {
    if (theme != kDarkTheme && theme != kLightTheme && theme != kHighContrastTheme) theme = std::string(kDarkTheme);
    layout_.theme = std::move(theme);
#if defined(SHINKOU_WITH_IMGUI)
    apply_theme();
#endif
}

void EditorLayer::push_console(std::string message) {
    if (message.empty()) return;
    consoleEntries_.push_back(std::move(message));
    if (consoleEntries_.size() > 256) consoleEntries_.erase(consoleEntries_.begin(), consoleEntries_.begin() + 64);
}

void EditorLayer::dispatch_command(EditorCommand command, std::string_view target, World& world) {
    uiModel_.execute(command);
    switch (command) {
    case EditorCommand::SaveLayout: save_layout(); break;
    case EditorCommand::ReloadLayout: load_layout(); break;
    case EditorCommand::ResetLayout: reset_layout(); break;
    case EditorCommand::TogglePage: {
        const auto page = target == "project" ? std::string_view{"assets"} : target;
        bool visible = page == "scene" ? layout_.showViewport : page == "game" ? layout_.showGame :
            page == "hierarchy" ? layout_.showHierarchy : page == "inspector" ? layout_.showInspector :
            page == "assets" ? layout_.showAssets : page == "console" ? layout_.showConsole :
            page == "profiler" ? layout_.showProfiler : page == "render-graph" ? layout_.showRenderGraph :
            page == "settings" ? layout_.showSettings : false;
        if (page == "scene") set_panel_visible("viewport", !visible);
        else set_panel_visible(page, !visible);
        break;
    }
    case EditorCommand::CreateEmpty:
    case EditorCommand::Create3DObject:
    case EditorCommand::Create2DObject:
    case EditorCommand::CreateUiObject: {
        const char* name = command == EditorCommand::Create3DObject ? "3D Object" :
            command == EditorCommand::Create2DObject ? "2D Object" :
            command == EditorCommand::CreateUiObject ? "UI Object" : "GameObject";
        auto& object = world.create_object(name);
        layout_.selectedObject = object.id();
        uiModel_.select_object(object.id());
        push_console(std::string("Created ") + name);
        break;
    }
    case EditorCommand::CreateChild: {
        auto* parent = world.find_object(layout_.selectedObject);
        auto& object = parent ? parent->create_child("GameObject") : world.create_object("GameObject");
        layout_.selectedObject = object.id();
        uiModel_.select_object(object.id());
        push_console("Created child GameObject");
        break;
    }
    case EditorCommand::ProjectSettings: set_panel_visible("settings", true); break;
    case EditorCommand::RefreshAssets: assetsDirty_ = true; push_console("Asset browser refresh requested"); break;
    case EditorCommand::SetDarkTheme: set_theme("dark"); break;
    case EditorCommand::SetLightTheme: set_theme("light"); break;
    case EditorCommand::SetHighContrastTheme: set_theme("high-contrast"); break;
    case EditorCommand::Play: push_console("Play mode requested"); break;
    case EditorCommand::Pause: push_console("Pause mode requested"); break;
    case EditorCommand::Step: push_console("Single frame step requested"); break;
    case EditorCommand::Undo: push_console("Undo requested"); break;
    case EditorCommand::Redo: push_console("Redo requested"); break;
    case EditorCommand::NewScene: push_console("New scene requested"); break;
    case EditorCommand::OpenScene: push_console("Open scene requested"); break;
    case EditorCommand::SaveScene: push_console("Save scene requested"); break;
    case EditorCommand::SaveSceneAs: push_console("Save scene as requested"); break;
    case EditorCommand::FrameSelection: push_console("Frame selection requested"); break;
    case EditorCommand::AddComponent: push_console("Add Component requested"); break;
    case EditorCommand::Quit: push_console("Quit requested by editor menu"); break;
    default: break;
    }
}

void EditorLayer::refresh_asset_cache() {
    assetEntries_.clear();
    const std::filesystem::path root(layout_.projectRoot);
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        assetsDirty_ = false;
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || assetEntries_.size() >= 256) break;
        assetEntries_.push_back(entry.path().filename().string() + (entry.is_directory() ? "/" : ""));
    }
    std::sort(assetEntries_.begin(), assetEntries_.end());
    assetsDirty_ = false;
}

void EditorLayer::draw(render::Renderer& renderer, World& world, Seconds dt, FrameIndex frame) {
    if (!initialized_) return;
    if (uiModel_.selected_object() != layout_.selectedObject) uiModel_.select_object(layout_.selectedObject);
    uiModel_.sync(world);
    layout_.selectedObject = uiModel_.selected_object();
    frameTimes_.push_back(std::max(dt, 0.0f) * 1000.0f);
    if (frameTimes_.size() > 120) frameTimes_.erase(frameTimes_.begin());
#if defined(SHINKOU_WITH_IMGUI)
    if (!ImGui::GetCurrentContext()) return;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(displayWidth_, displayHeight_);
    io.DeltaTime = std::max(dt, 1.0f / 1000.0f);
#if defined(SHINKOU_PLATFORM_WINDOWS)
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();
    draw_toolbar(renderer, world);
    if (layout_.showHierarchy) draw_hierarchy(world);
    if (layout_.showInspector) draw_inspector(world);
    if (layout_.showViewport) draw_viewport(renderer);
    if (layout_.showGame) draw_game_view(renderer);
    if (layout_.showAssets) draw_assets();
    if (layout_.showConsole) draw_console(renderer);
    if (layout_.showProfiler) draw_profiler(renderer);
    if (layout_.showRenderGraph) draw_render_graph(renderer);
    if (layout_.showSettings) draw_settings();
    draw_registered_panels(renderer, world, frame, dt);
    draw_status_bar(renderer);
    ImGui::Render();
    renderer.set_imgui_draw_data(ImGui::GetDrawData());
#else
    (void)renderer;
    (void)world;
    (void)frame;
#endif
}

#if defined(SHINKOU_WITH_IMGUI)
void EditorLayer::draw_toolbar(render::Renderer& renderer, World& world) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
#else
    ImGui::SetNextWindowPos(ImVec2(0.0f, 20.0f), ImGuiCond_FirstUseEver);
#endif
    ImGui::SetNextWindowSize(ImVec2(displayWidth_, 42.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (ImGui::Button("Play")) dispatch_command(EditorCommand::Play, {}, world);
    ImGui::SameLine();
    if (ImGui::Button("Pause")) dispatch_command(EditorCommand::Pause, {}, world);
    ImGui::SameLine();
    if (ImGui::Button("Step")) dispatch_command(EditorCommand::Step, {}, world);
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    if (ImGui::Button("Save")) dispatch_command(EditorCommand::SaveLayout, {}, world);
    ImGui::SameLine();
    const auto capabilities = renderer.capabilities();
    ImGui::Text("Backend: %s | Device: %s", backend_name(capabilities.api), device_state_name(capabilities.deviceState));
    ImGui::End();
}

void EditorLayer::draw_hierarchy(World& world) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 64.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280.0f, displayHeight_ - 194.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy", &layout_.showHierarchy);
    std::array<char, 128> filter{};
    std::strncpy(filter.data(), uiModel_.object_filter().c_str(), filter.size() - 1);
    if (ImGui::InputTextWithHint("##hierarchy-filter", "Filter objects...", filter.data(), filter.size())) {
        uiModel_.set_object_filter(filter.data());
        uiModel_.sync(world);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
        auto& object = world.create_object("GameObject");
        layout_.selectedObject = object.id();
        uiModel_.select_object(object.id());
    }
    ImGui::Separator();
    for (const auto& object : uiModel_.object_roots()) draw_hierarchy_object(object);
    ImGui::End();
}

void EditorLayer::draw_hierarchy_object(const EditorObjectTreeNode& object) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (object.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (object.selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!object.active) flags |= ImGuiTreeNodeFlags_Bullet;
    const auto expanded = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(object.id)), flags, "%s", object.name.c_str());
    if (ImGui::IsItemClicked()) {
        layout_.selectedObject = object.id;
        uiModel_.select_object(object.id);
    }
    if (expanded) {
        for (const auto& child : object.children) draw_hierarchy_object(child);
        ImGui::TreePop();
    }
}

void EditorLayer::draw_inspector(World& world) {
    ImGui::SetNextWindowPos(ImVec2(displayWidth_ - 300.0f, 64.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300.0f, displayHeight_ - 194.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector", &layout_.showInspector);
    auto* object = world.find_object(layout_.selectedObject);
    if (!object) {
        ImGui::TextDisabled("Select an object to inspect it.");
        ImGui::End();
        return;
    }
    std::array<char, 256> name{};
    std::strncpy(name.data(), std::string(object->name()).c_str(), name.size() - 1);
    if (ImGui::InputText("Name", name.data(), name.size())) object->set_name(name.data());
    bool active = object->active_self();
    if (ImGui::Checkbox("Active", &active)) object->set_active(active);
    ImGui::Text("Object ID: %llu", static_cast<unsigned long long>(object->id()));
    ImGui::Separator();
    object->each_component([](Component& component) {
        const std::string label = std::string(component.registered_type_name()) + "##component" + std::to_string(component.id());
        if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool enabled = component.enabled();
            if (ImGui::Checkbox("Enabled", &enabled)) component.set_enabled(enabled);
            for (const auto& property : component.properties()) if (!has_flag(property.flags, PropertyFlags::Hidden)) draw_property(property);
        }
    });
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("add-component");
    if (ImGui::BeginPopup("add-component")) {
        for (const auto& type : world.component_types()) if (ImGui::MenuItem(type.c_str())) object->add_component(type);
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorLayer::draw_viewport(render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(280.0f, 64.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::max(displayWidth_ - 580.0f, 360.0f), displayHeight_ - 194.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Viewport", &layout_.showViewport);
    const auto size = ImGui::GetContentRegionAvail();
    ImGui::Text("Scene viewport (%ux%u)", static_cast<unsigned>(size.x), static_cast<unsigned>(size.y));
    ImGui::TextDisabled("Renderer target embedding is isolated here and can be connected without changing RenderGraph.");
    ImGui::Separator();
    ImGui::BeginChild("viewport-surface", size, true, ImGuiWindowFlags_NoScrollbar);
    const auto drawList = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 24, 30, 255));
    drawList->AddText(ImVec2(origin.x + 16.0f, origin.y + 16.0f), IM_COL32(160, 174, 190, 255), "Render target preview");
    ImGui::Dummy(size);
    ImGui::EndChild();
    (void)renderer;
    ImGui::End();
}

void EditorLayer::draw_game_view(render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(320.0f, 110.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::max(displayWidth_ - 640.0f, 360.0f), displayHeight_ - 260.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Game", &layout_.showGame);
    ImGui::Text("Game view");
    ImGui::SameLine();
    ImGui::TextDisabled(uiModel_.playing() ? (uiModel_.paused() ? "Paused" : "Playing") : "Edit mode");
    ImGui::Separator();
    const auto size = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("game-surface", size, true, ImGuiWindowFlags_NoScrollbar);
    const auto drawList = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(12, 16, 20, 255));
    drawList->AddText(ImVec2(origin.x + 16.0f, origin.y + 16.0f), IM_COL32(160, 174, 190, 255), "Runtime game output");
    ImGui::Dummy(size);
    ImGui::EndChild();
    (void)renderer;
    ImGui::End();
}

void EditorLayer::draw_assets() {
    ImGui::SetNextWindowPos(ImVec2(0.0f, displayHeight_ - 130.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(displayWidth_ * 0.42f, 130.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Asset Browser", &layout_.showAssets);
    ImGui::Text("Root: %s", layout_.projectRoot.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) assetsDirty_ = true;
    if (assetsDirty_) refresh_asset_cache();
    ImGui::Separator();
    for (const auto& entry : assetEntries_) ImGui::Selectable(entry.c_str());
    ImGui::End();
}

void EditorLayer::draw_console(const render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(displayWidth_ * 0.42f, displayHeight_ - 130.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(displayWidth_ * 0.38f, 130.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Console", &layout_.showConsole);
    if (ImGui::SmallButton("Clear")) consoleEntries_.clear();
    ImGui::Separator();
    for (const auto& entry : consoleEntries_) ImGui::TextUnformatted(entry.c_str());
    if (!renderer.last_error().empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", renderer.last_error().c_str());
    const auto stats = renderer.stats();
    for (const auto& diagnostic : stats.validationDiagnostics) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", diagnostic.c_str());
    ImGui::End();
}

void EditorLayer::draw_profiler(const render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(280.0f, 64.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 260.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Profiler", &layout_.showProfiler);
    const auto stats = renderer.stats();
    ImGui::Text("Frame time: %.2f ms", frameTimes_.empty() ? 0.0f : frameTimes_.back());
    if (!frameTimes_.empty()) ImGui::PlotLines("##frame-time", frameTimes_.data(), static_cast<int>(frameTimes_.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(-1.0f, 80.0f));
    ImGui::Text("Frames: %llu | Draws: %llu | Passes: %llu", static_cast<unsigned long long>(stats.frames),
        static_cast<unsigned long long>(stats.drawCalls), static_cast<unsigned long long>(stats.passes));
    ImGui::Text("Resources: +%llu / -%llu | Barriers: %llu", static_cast<unsigned long long>(stats.resourceCreates),
        static_cast<unsigned long long>(stats.resourceDestroys), static_cast<unsigned long long>(stats.barriers));
    ImGui::End();
}

void EditorLayer::draw_render_graph(render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(280.0f, 330.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 260.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Render Graph", &layout_.showRenderGraph);
    const auto& graph = renderer.graph();
    ImGui::Text("Passes: %zu | Resources: %zu | Transitions: %zu", graph.passes().size(), graph.resources().size(), graph.transitions().size());
    for (const auto& pass : graph.passes()) ImGui::BulletText("%s", pass.name.c_str());
    const auto& diagnostics = graph.diagnostics();
    if (diagnostics.executionFailed) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", diagnostics.lastError.c_str());
    ImGui::End();
}

void EditorLayer::draw_settings() {
    ImGui::SetNextWindowPos(ImVec2(420.0f, 160.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Editor Settings", &layout_.showSettings);
    ImGui::Text("Workspace");
    std::array<char, 128> workspace{};
    std::strncpy(workspace.data(), layout_.workspace.c_str(), workspace.size() - 1);
    if (ImGui::InputText("Name", workspace.data(), workspace.size())) layout_.workspace = workspace.data();
    ImGui::Text("Layout file: %s", layout_.layoutFile.c_str());
    ImGui::Text("Theme: %s", layout_.theme.c_str());
    if (ImGui::Button("Save workspace")) save_layout();
    ImGui::SameLine();
    if (ImGui::Button("Reset workspace")) reset_layout();
    ImGui::End();
}

void EditorLayer::draw_registered_panels(render::Renderer& renderer, World& world, FrameIndex frame, Seconds dt) {
    EditorPanelContext context{renderer, world, uiModel_, layout_, frame, dt};
    for (auto& panel : panels_) {
        if (!panel.draw || !panel_visible(panel.id)) continue;
        bool visible = true;
        if (!ImGui::Begin(panel.title.c_str(), &visible)) {
            ImGui::End();
            continue;
        }
        panel.draw(context);
        ImGui::End();
        if (!visible) panelVisibility_[panel.id] = false;
    }
}

void EditorLayer::draw_status_bar(const render::Renderer& renderer) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, displayHeight_ - 28.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(displayWidth_, 28.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    const auto stats = renderer.stats();
    ImGui::Text("%s | %s | Frame %llu | %s", layout_.workspace.c_str(), backend_name(renderer.capabilities().api),
        static_cast<unsigned long long>(stats.frames), lastStatus_.empty() ? "Ready" : lastStatus_.c_str());
    ImGui::End();
}

void EditorLayer::apply_theme() {
    auto& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    if (layout_.theme == kLightTheme) ImGui::StyleColorsLight(&style);
    if (layout_.theme == kHighContrastTheme) {
        ImGui::StyleColorsDark(&style);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.02f, 0.02f, 1.0f);
        style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.35f, 0.65f, 1.0f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.5f, 0.9f, 1.0f);
    }
}
#endif

void EditorLayer::shutdown() {
    if (!initialized_) return;
    save_layout();
    uninstall_native_menu();
#if defined(SHINKOU_WITH_IMGUI)
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (nativeWindow_) ImGui_ImplWin32_Shutdown();
#endif
    if (uiContextOwned_ && ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
        uiContextOwned_ = false;
    }
#endif
    initialized_ = false;
}

} // namespace shinkou::editor
