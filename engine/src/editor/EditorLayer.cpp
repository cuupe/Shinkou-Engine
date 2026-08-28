#include "shinkou/editor/EditorLayer.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/reflection/Serialization.h"
#include "shinkou/ui/ImGuiAdapter.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iomanip>
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
        .field("dockLayoutFile", &EditorLayoutState::dockLayoutFile)
        .field("styleFile", &EditorLayoutState::styleFile)
        .field("fontPath", &EditorLayoutState::fontPath)
        .field("fontSize", &EditorLayoutState::fontSize)
        .field("backgroundColor", &EditorLayoutState::backgroundColor)
        .field("backgroundImage", &EditorLayoutState::backgroundImage)
        .field("compactControls", &EditorLayoutState::compactControls)
        .field("reduceMotion", &EditorLayoutState::reduceMotion)
        .field("selectedObject", &EditorLayoutState::selectedObject)
        .field("uiScale", &EditorLayoutState::uiScale)
        .field("allowDocking", &EditorLayoutState::allowDocking)
        .field("showToolbar", &EditorLayoutState::showToolbar)
        .field("showStatusBar", &EditorLayoutState::showStatusBar)
        .field("showHierarchy", &EditorLayoutState::showHierarchy)
        .field("showInspector", &EditorLayoutState::showInspector)
        .field("showViewport", &EditorLayoutState::showViewport)
        .field("showGame", &EditorLayoutState::showGame)
        .field("showAssets", &EditorLayoutState::showAssets)
        .field("showConsole", &EditorLayoutState::showConsole)
        .field("showProfiler", &EditorLayoutState::showProfiler)
        .field("showRenderGraph", &EditorLayoutState::showRenderGraph)
        .field("showSettings", &EditorLayoutState::showSettings)
        .field("showMedia", &EditorLayoutState::showMedia)
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

ui::ThemeColor parse_hex_color(std::string_view value, ui::ThemeColor fallback) noexcept {
    if (value.size() != 7 || value.front() != '#') return fallback;
    unsigned result = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char character = value[index];
        const unsigned digit = character >= '0' && character <= '9' ? static_cast<unsigned>(character - '0') :
            character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10) :
            character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10) : 16u;
        if (digit > 15u) return fallback;
        result = (result << 4u) | digit;
    }
    return {((result >> 16u) & 0xffu) / 255.0f, ((result >> 8u) & 0xffu) / 255.0f,
            (result & 0xffu) / 255.0f, 1.0f};
}

std::string hex_color(ui::ThemeColor color) {
    const auto byte = [](float value) { return static_cast<unsigned>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
    std::ostringstream output;
    output << '#' << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << byte(color.r)
           << std::setw(2) << byte(color.g) << std::setw(2) << byte(color.b);
    return output.str();
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
    if (uiComponents_.size() == 0) {
        uiComponents_.emplace<ui::Button>("play", "Play");
        uiComponents_.emplace<ui::Button>("pause", "Pause");
        uiComponents_.emplace<ui::Button>("step", "Step");
        uiComponents_.emplace<ui::Button>("save", "Save");
        uiComponents_.emplace<ui::Button>("reset", "Reset");
    }
    build_default_workspace();
    load_layout_file();
    fileSystem_.set_root(layout_.projectRoot);
    load_style_file();
    sync_style_to_layout();
    load_workspace_file();
    if (!themeRegistry_.find(layout_.theme)) layout_.theme = std::string(kDarkTheme);
    themeRegistry_.switch_theme(layout_.theme);
    sync_page_visibility();
    sync_workspace_visibility();
    refresh_asset_cache();
    install_native_menu();
#if defined(SHINKOU_WITH_UIKIT)
    uiKitPanels_ = std::make_unique<UiKitPanelHost>();
    uiKitPanels_->set_command_handler([this](EditorCommand command, std::string_view target) {
        if (activeWorld_) dispatch_command(command, target, *activeWorld_);
    });
    uiKitPanels_->set_object_selection_handler([this](ObjectId id) {
        layout_.selectedObject = id;
        uiModel_.select_object(id);
    });
    uiKitPanels_->set_object_name_handler([this](ObjectId id, std::string name) {
        if (activeWorld_) {
            if (auto* object = activeWorld_->find_object(id)) object->set_name(std::move(name));
        }
    });
    uiKitPanels_->set_object_active_handler([this](ObjectId id, bool active) {
        if (activeWorld_) {
            if (auto* object = activeWorld_->find_object(id)) object->set_active(active);
        }
    });
    uiKitPanels_->set_media_command_handler([this](const ui::MediaCommand& command) {
        mediaPanel_.apply(command);
    });
    uiKitPanels_->set_object_filter_handler([this](std::string filter) {
        uiModel_.set_object_filter(std::move(filter));
    });
    uiKitPanels_->set_asset_selection_handler([this](std::string path) {
        selectedAsset_ = std::move(path);
    });
#endif
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

void EditorLayer::process_input(const input::InputSystem& input, World& world) {
    if (!initialized_) return;
    activeWorld_ = &world;
    // Keep hit-test rectangles current before routing this tick's events. The
    // next draw/layout pass will consume any dirty state caused by handlers.
    uiRuntime_.layout({displayWidth_, displayHeight_});
    uiInputStats_ = ui::InputBridge::dispatch(input, uiRuntime_);
#if defined(SHINKOU_WITH_UIKIT)
    if (uiKitPanels_) {
        uiKitPanels_->set_viewport(displayWidth_, displayHeight_);
        uiKitPanels_->process_input(input);
    }
#endif
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
    add("media", "Media Preview", false);
}

void EditorLayer::build_default_workspace() {
    auto leaf = [](std::string id, std::string title, bool visible, math::Vec2 minimum) {
        return DockNode::leaf(DockPanel{std::move(id), std::move(title), visible, true, minimum});
    };
    auto bottom = DockNode::split(leaf("assets", "Asset Browser", true, {180.0f, 90.0f}),
                                  leaf("console", "Console", true, {180.0f, 90.0f}),
                                  DockSplitOrientation::Horizontal, 0.5f);
    auto center = DockNode::split(leaf("viewport", "Viewport", true, {320.0f, 220.0f}),
                                  std::move(bottom), DockSplitOrientation::Vertical, 0.76f);
    auto right = DockNode::split(leaf("inspector", "Inspector", true, {240.0f, 220.0f}),
                                 leaf("profiler", "Profiler", false, {240.0f, 120.0f}),
                                 DockSplitOrientation::Vertical, 0.72f);
    auto middle = DockNode::split(std::move(center), std::move(right),
                                  DockSplitOrientation::Horizontal, 0.76f);
    dockWorkspace_.set_root(DockNode::split(leaf("hierarchy", "Hierarchy", true, {220.0f, 220.0f}),
                                            std::move(middle), DockSplitOrientation::Horizontal, 0.2f));
    dockWorkspace_.add_tab("viewport", DockPanel{"game", "Game", false, true, {320.0f, 220.0f}}, false);
    dockWorkspace_.add_tab("console", DockPanel{"render-graph", "Render Graph", false, true, {240.0f, 120.0f}}, false);
    dockWorkspace_.add_tab("console", DockPanel{"settings", "Editor Settings", false, true, {240.0f, 160.0f}}, false);
    dockWorkspace_.add_tab("viewport", DockPanel{"media", "Media Preview", false, true, {280.0f, 180.0f}}, false);
    const auto isBuiltin = [](std::string_view id) {
        return id == "hierarchy" || id == "inspector" || id == "viewport" || id == "game" || id == "assets" ||
            id == "console" || id == "profiler" || id == "render-graph" || id == "settings" || id == "media";
    };
    for (const auto& panel : panels_) {
        if (!isBuiltin(panel.id)) dockWorkspace_.add_tab("viewport", DockPanel{panel.id, panel.title, panel.defaultVisible, true, {160.0f, 100.0f}}, false);
    }
    sync_workspace_visibility();
}

void EditorLayer::sync_workspace_visibility() noexcept {
    for (const auto& [id, visible] : panelVisibility_) dockWorkspace_.set_panel_visibility(id, visible);
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
    uiModel_.set_page_visible("media", layout_.showMedia);
}

bool EditorLayer::register_panel(EditorPanel panel) {
    if (panel.id.empty() || panel.title.empty() || !panel.draw || panelVisibility_.find(panel.id) != panelVisibility_.end()) return false;
    panelVisibility_[panel.id] = panel.defaultVisible;
    const DockPanel dockPanel{panel.id, panel.title, panel.defaultVisible, panel.closeable, {panel.minWidth, panel.minHeight}};
    panels_.push_back(std::move(panel));
    if (initialized_) dockWorkspace_.add_tab("viewport", dockPanel, false);
    return true;
}

bool EditorLayer::unregister_panel(std::string_view id) {
    const auto it = std::find_if(panels_.begin(), panels_.end(), [id](const EditorPanel& panel) { return panel.id == id; });
    if (it == panels_.end() || it->id == "hierarchy" || it->id == "inspector" || it->id == "viewport") return false;
    dockWorkspace_.remove_tab(it->id);
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
    else if (id == "media") layout_.showMedia = visible;
    dockWorkspace_.set_panel_visibility(id, visible);
    sync_page_visibility();
    return true;
}

bool EditorLayer::panel_visible(std::string_view id) const noexcept {
    const auto it = panelVisibility_.find(std::string(id));
    return it != panelVisibility_.end() && it->second;
}

void EditorLayer::set_project_root(std::string path) {
    layout_.projectRoot = std::move(path);
    fileSystem_.set_root(layout_.projectRoot);
    assetsDirty_ = true;
}

void EditorLayer::set_layout_path(std::string path) {
    layout_.layoutFile = std::move(path);
    layout_.dockLayoutFile = layout_.layoutFile + ".dock.json";
    layout_.styleFile = layout_.layoutFile + ".style.json";
}

void EditorLayer::set_style_file(std::string path) {
    if (!path.empty()) layout_.styleFile = std::move(path);
}

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
    const auto temporaryPath = path.string() + ".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        lastStatus_ = "Cannot open layout file: " + temporaryPath;
        return false;
    }
    file << output;
    file.flush();
    if (!file) {
        lastStatus_ = "Layout write failed: " + path.string();
        file.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return false;
    }
    file.close();
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, path, renameError);
    if (renameError) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        renameError.clear();
        std::filesystem::rename(temporaryPath, path, renameError);
    }
    if (renameError) {
        lastStatus_ = "Cannot commit layout file: " + renameError.message();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return false;
    }
    lastStatus_ = "Layout saved: " + path.string();
    return true;
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
    if (loaded.layoutVersion > 2) {
        lastStatus_ = "Layout load failed: unsupported layout version";
        return false;
    }
    if (loaded.layoutVersion == 0) loaded.layoutVersion = 1;
    loaded.layoutVersion = 2;
    if (!std::isfinite(loaded.uiScale)) loaded.uiScale = 1.0f;
    loaded.uiScale = std::clamp(loaded.uiScale, 0.5f, 3.0f);
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
    set_panel_visible("media", layout_.showMedia);
    lastStatus_ = "Layout loaded: " + layout_.layoutFile;
    return true;
}

bool EditorLayer::save_workspace_file() {
    const std::filesystem::path path(layout_.dockLayoutFile);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const auto temporaryPath = path.string() + ".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << dockWorkspace_.to_json(true);
    file.flush();
    file.close();
    if (!file) return false;
    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
    }
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return false;
    }
    return true;
}

bool EditorLayer::load_workspace_file() {
    std::ifstream file(layout_.dockLayoutFile, std::ios::binary);
    if (!file) return false;
    std::stringstream input;
    input << file.rdbuf();
    DockWorkspace loaded = dockWorkspace_;
    const auto result = loaded.from_json(input.str());
    if (!result) {
        lastStatus_ = "Dock layout load failed: " + result.message;
        return false;
    }
    dockWorkspace_ = std::move(loaded);
    return true;
}

void EditorLayer::sync_style_to_layout() noexcept {
    layout_.theme = styleConfig_.activeTheme;
    layout_.uiScale = styleConfig_.uiScale;
    layout_.fontPath = styleConfig_.fontPath;
    layout_.fontSize = styleConfig_.fontSize;
    layout_.backgroundColor = hex_color(styleConfig_.background.primary);
    layout_.backgroundImage = styleConfig_.background.imagePath;
    layout_.compactControls = styleConfig_.compactControls;
    layout_.reduceMotion = styleConfig_.reduceMotion;
}

void EditorLayer::sync_layout_to_style() noexcept {
    styleConfig_.activeTheme = layout_.theme;
    styleConfig_.uiScale = std::clamp(std::isfinite(layout_.uiScale) ? layout_.uiScale : 1.0f, 0.5f, 3.0f);
    styleConfig_.fontPath = layout_.fontPath;
    styleConfig_.fontSize = std::clamp(std::isfinite(layout_.fontSize) ? layout_.fontSize : 14.0f, 6.0f, 96.0f);
    styleConfig_.background.primary = parse_hex_color(layout_.backgroundColor, styleConfig_.background.primary);
    styleConfig_.background.imagePath = layout_.backgroundImage;
    styleConfig_.compactControls = layout_.compactControls;
    styleConfig_.reduceMotion = layout_.reduceMotion;
}

bool EditorLayer::load_style_file() {
    std::error_code existsError;
    if (!std::filesystem::exists(layout_.styleFile, existsError)) {
        sync_layout_to_style();
        return false;
    }
    std::string error;
    if (!ui::load_style_file(layout_.styleFile, styleConfig_, &error)) {
        lastStatus_ = "Style load failed: " + error;
        sync_layout_to_style();
        return false;
    }
    return true;
}

bool EditorLayer::save_style_file() {
    sync_layout_to_style();
    std::string error;
    if (!ui::save_style_file(layout_.styleFile, styleConfig_, &error)) {
        lastStatus_ = "Style save failed: " + error;
        return false;
    }
    lastStatus_ = "Style saved: " + layout_.styleFile;
    return true;
}

bool EditorLayer::save_layout() {
#if defined(SHINKOU_WITH_IMGUI)
    const auto imguiPath = layout_.layoutFile + ".imgui.ini";
    if (ImGui::GetCurrentContext()) ImGui::SaveIniSettingsToDisk(imguiPath.c_str());
#endif
    const bool savedLayout = save_layout_file();
    const bool savedDock = save_workspace_file();
    const bool savedStyle = save_style_file();
    if (!savedDock && savedLayout) lastStatus_ = "Layout saved, dock layout could not be committed";
    return savedLayout && savedDock && savedStyle;
}

bool EditorLayer::load_layout() {
    const bool loaded = load_layout_file();
    fileSystem_.set_root(layout_.projectRoot);
    load_style_file();
    std::error_code dockFileError;
    const bool hasDockFile = std::filesystem::exists(layout_.dockLayoutFile, dockFileError);
    const bool dockLoaded = !hasDockFile || load_workspace_file();
#if defined(SHINKOU_WITH_IMGUI)
    const auto imguiPath = layout_.layoutFile + ".imgui.ini";
    if (loaded && ImGui::GetCurrentContext()) ImGui::LoadIniSettingsFromDisk(imguiPath.c_str());
    apply_theme();
#endif
    sync_workspace_visibility();
    return loaded && dockLoaded;
}

void EditorLayer::reset_layout() {
    layout_ = EditorLayoutState{};
    styleConfig_ = ui::UiStyleConfig{};
    fileSystem_.set_root(layout_.projectRoot);
    for (const auto& panel : panels_) panelVisibility_[panel.id] = panel.defaultVisible;
    build_default_workspace();
    sync_page_visibility();
    lastStatus_ = "Layout reset";
#if defined(SHINKOU_WITH_IMGUI)
    apply_theme();
#endif
}

void EditorLayer::set_theme(std::string theme) {
    if (!themeRegistry_.find(theme)) theme = std::string(kDarkTheme);
    layout_.theme = std::move(theme);
    themeRegistry_.switch_theme(layout_.theme);
    styleConfig_.activeTheme = layout_.theme;
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
    projectFiles_ = fileSystem_.list({}, true, 512);
    for (const auto& entry : projectFiles_) {
        if (assetEntries_.size() >= 256) break;
        assetEntries_.push_back(entry.relativePath.generic_string() + (entry.directory ? "/" : ""));
    }
    assetsDirty_ = false;
}

void EditorLayer::poll_editor_files() {
    const auto changes = fileSystem_.poll_changes(true, 8192);
    if (changes.empty()) return;
    bool styleChanged = false;
    for (const auto& change : changes) {
        if (change.relativePath == std::filesystem::path(layout_.styleFile)) styleChanged = true;
        if (change.relativePath.extension() != ".tmp") assetsDirty_ = true;
    }
    if (styleChanged && load_style_file()) {
        sync_style_to_layout();
        if (themeRegistry_.find(layout_.theme)) themeRegistry_.switch_theme(layout_.theme);
#if defined(SHINKOU_WITH_IMGUI)
        apply_theme();
#endif
        push_console("UI style hot reloaded");
    }
}

void EditorLayer::draw(render::Renderer& renderer, World& world, Seconds dt, FrameIndex frame) {
    if (!initialized_) return;
    if (uiModel_.selected_object() != layout_.selectedObject) uiModel_.select_object(layout_.selectedObject);
    uiModel_.sync(world);
    mediaPanel_.update(dt);
    poll_editor_files();
    if (assetsDirty_) refresh_asset_cache();
    layout_.selectedObject = uiModel_.selected_object();
    frameTimes_.push_back(std::max(dt, 0.0f) * 1000.0f);
    if (frameTimes_.size() > 120) frameTimes_.erase(frameTimes_.begin());
#if defined(SHINKOU_WITH_UIKIT)
    activeWorld_ = &world;
    if (uiKitPanels_) {
        uiKitPanels_->set_viewport(displayWidth_, displayHeight_);
        uiKitPanels_->rebuild(uiModel_, projectFiles_, renderer, mediaPanel_, layout_, consoleEntries_, lastStatus_, dt);
        renderer.set_ui_render_list(&uiKitPanels_->render_list());
    }
#if defined(SHINKOU_WITH_IMGUI)
    if (!ImGui::GetCurrentContext()) return;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(displayWidth_, displayHeight_);
    io.DeltaTime = std::max(dt, 1.0f / 1000.0f);
#if defined(SHINKOU_PLATFORM_WINDOWS)
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();
    if (uiKitPanels_) uiKitPanels_->draw_imgui();
    ImGui::Render();
    renderer.set_imgui_draw_data(ImGui::GetDrawData());
#endif
    (void)frame;
#elif defined(SHINKOU_WITH_IMGUI)
    if (!ImGui::GetCurrentContext()) return;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(displayWidth_, displayHeight_);
    io.DeltaTime = std::max(dt, 1.0f / 1000.0f);
#if defined(SHINKOU_PLATFORM_WINDOWS)
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();
    draw_main_menu(renderer, world);
#if defined(IMGUI_HAS_DOCK)
    if (layout_.allowDocking) {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        constexpr ImGuiWindowFlags dockHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##ShinkouDockHost", nullptr, dockHostFlags);
        ImGui::PopStyleVar(2);
        ImGui::DockSpace(ImGui::GetID("ShinkouEditorDockSpace"), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
    }
#endif
    if (layout_.showToolbar) draw_toolbar(renderer, world);
    if (layout_.showHierarchy) draw_hierarchy(world);
    if (layout_.showInspector) draw_inspector(world);
    if (layout_.showViewport) draw_viewport(renderer);
    if (layout_.showGame) draw_game_view(renderer);
    if (layout_.showAssets) draw_assets();
    if (layout_.showConsole) draw_console(renderer);
    if (layout_.showProfiler) draw_profiler(renderer);
    if (layout_.showRenderGraph) draw_render_graph(renderer);
    if (layout_.showSettings) draw_settings();
    if (layout_.showMedia) draw_media();
    draw_registered_panels(renderer, world, frame, dt);
    if (layout_.showStatusBar) draw_status_bar(renderer);
    panelVisibility_["hierarchy"] = layout_.showHierarchy;
    panelVisibility_["inspector"] = layout_.showInspector;
    panelVisibility_["viewport"] = layout_.showViewport;
    panelVisibility_["game"] = layout_.showGame;
    panelVisibility_["assets"] = layout_.showAssets;
    panelVisibility_["console"] = layout_.showConsole;
    panelVisibility_["profiler"] = layout_.showProfiler;
    panelVisibility_["render-graph"] = layout_.showRenderGraph;
    panelVisibility_["settings"] = layout_.showSettings;
    panelVisibility_["media"] = layout_.showMedia;
    sync_workspace_visibility();
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
    ImGui::TextColored(ImVec4(0.42f, 0.72f, 1.0f, 1.0f), "SHINKOU");
    ImGui::SameLine();
    ImGui::TextDisabled("/ Editor");
    ImGui::SameLine();
    const auto button = [this, &world](const char* id, EditorCommand command) {
        auto* control = dynamic_cast<ui::Button*>(uiComponents_.find(id));
        if (!control) return;
        if (ImGui::Button(control->label().data())) { control->click(); dispatch_command(command, {}, world); }
    };
    button("play", EditorCommand::Play);
    ImGui::SameLine();
    button("pause", EditorCommand::Pause);
    ImGui::SameLine();
    button("step", EditorCommand::Step);
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    button("save", EditorCommand::SaveLayout);
    ImGui::SameLine();
    button("reset", EditorCommand::ResetLayout);
    ImGui::SameLine();
    const auto capabilities = renderer.capabilities();
    ImGui::TextDisabled("Backend %s  ·  Device %s", backend_name(capabilities.api), device_state_name(capabilities.deviceState));
    ImGui::End();
}

void EditorLayer::draw_main_menu(render::Renderer& renderer, World& world) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Workspace", "Ctrl+S")) dispatch_command(EditorCommand::SaveLayout, {}, world);
        if (ImGui::MenuItem("Reload Workspace")) dispatch_command(EditorCommand::ReloadLayout, {}, world);
        if (ImGui::MenuItem("Reset Workspace")) dispatch_command(EditorCommand::ResetLayout, {}, world);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) push_console("Close the editor window to exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Empty Object")) dispatch_command(EditorCommand::CreateEmpty, {}, world);
        if (ImGui::MenuItem("3D Object")) dispatch_command(EditorCommand::Create3DObject, {}, world);
        if (ImGui::MenuItem("2D Object")) dispatch_command(EditorCommand::Create2DObject, {}, world);
        if (ImGui::MenuItem("UI Object")) dispatch_command(EditorCommand::CreateUiObject, {}, world);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        const auto menu = [this](const char* label, const char* id) {
            bool visible = panel_visible(id);
            if (ImGui::MenuItem(label, nullptr, &visible)) set_panel_visible(id, visible);
        };
        menu("Scene", "viewport"); menu("Game", "game"); menu("Hierarchy", "hierarchy");
        menu("Inspector", "inspector"); menu("Assets", "assets"); menu("Console", "console");
        menu("Profiler", "profiler"); menu("Render Graph", "render-graph"); menu("Settings", "settings");
        menu("Media Preview", "media");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Theme")) {
        for (const auto& id : themeRegistry_.ids()) {
            bool active = id == layout_.theme;
            if (ImGui::MenuItem(id.c_str(), nullptr, &active)) set_theme(id);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("F1  Toggle Settings");
        ImGui::TextDisabled("Ctrl+S  Save workspace");
        ImGui::TextDisabled("UI input events: %zu handled", uiInputStats_.handled);
        ImGui::EndMenu();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 235.0f);
    ImGui::TextDisabled("%s  |  %.0fx%.0f", layout_.workspace.c_str(), displayWidth_, displayHeight_);
    (void)renderer;
    ImGui::EndMainMenuBar();
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
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Scene");
    ImGui::SameLine();
    ImGui::TextDisabled("%ux%u  ·  %s", static_cast<unsigned>(std::max(size.x, 0.0f)),
                       static_cast<unsigned>(std::max(size.y, 0.0f)), uiModel_.playing() ? "Play mode" : "Edit mode");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130.0f);
    ImGui::SmallButton("Perspective");
    ImGui::SameLine();
    ImGui::SmallButton("Grid");
    ImGui::Separator();
    const auto surfaceSize = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("viewport-surface", surfaceSize, true, ImGuiWindowFlags_NoScrollbar);
    const auto drawList = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto background = parse_hex_color(layout_.backgroundColor, {0.055f, 0.067f, 0.09f, 1.0f});
    const auto to_u8 = [](float value) { return static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
    if (styleConfig_.background.mode == ui::BackgroundMode::Gradient) {
        constexpr int strips = 32;
        for (int index = 0; index < strips; ++index) {
            const float t = static_cast<float>(index) / static_cast<float>(strips - 1);
            const auto mix = [t](float first, float second) { return first + (second - first) * t; };
            drawList->AddRectFilled(ImVec2(origin.x, origin.y + surfaceSize.y * t),
                ImVec2(origin.x + surfaceSize.x, origin.y + surfaceSize.y * (t + 1.0f / strips)),
                IM_COL32(to_u8(mix(styleConfig_.background.primary.r, styleConfig_.background.secondary.r)),
                         to_u8(mix(styleConfig_.background.primary.g, styleConfig_.background.secondary.g)),
                         to_u8(mix(styleConfig_.background.primary.b, styleConfig_.background.secondary.b)), 255));
        }
    } else {
        drawList->AddRectFilled(origin, ImVec2(origin.x + surfaceSize.x, origin.y + surfaceSize.y),
                                IM_COL32(to_u8(background.r), to_u8(background.g), to_u8(background.b), 255));
    }
    const auto gridColor = IM_COL32(255, 255, 255, 16);
    const float grid = std::max(16.0f, 24.0f * layout_.uiScale);
    for (float x = origin.x; x < origin.x + surfaceSize.x; x += grid) drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + surfaceSize.y), gridColor);
    for (float y = origin.y; y < origin.y + surfaceSize.y; y += grid) drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + surfaceSize.x, y), gridColor);
    drawList->AddLine(ImVec2(origin.x + surfaceSize.x * 0.5f, origin.y), ImVec2(origin.x + surfaceSize.x * 0.5f, origin.y + surfaceSize.y), IM_COL32(100, 180, 255, 60));
    drawList->AddLine(ImVec2(origin.x, origin.y + surfaceSize.y * 0.5f), ImVec2(origin.x + surfaceSize.x, origin.y + surfaceSize.y * 0.5f), IM_COL32(100, 180, 255, 60));
    drawList->AddText(ImVec2(origin.x + 16.0f, origin.y + 16.0f), IM_COL32(200, 210, 225, 220), "Render target preview");
    drawList->AddText(ImVec2(origin.x + 16.0f, origin.y + 36.0f), IM_COL32(145, 160, 180, 200), "Drag panels from their title bars to customize this workspace");
    ImGui::Dummy(surfaceSize);
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
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Project");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", fileSystem_.root().string().c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 78.0f);
    if (ImGui::SmallButton("Refresh")) assetsDirty_ = true;
    std::array<char, 256> filter{};
    std::strncpy(filter.data(), assetFilter_.c_str(), filter.size() - 1);
    if (ImGui::InputTextWithHint("##asset-filter", "Filter assets...", filter.data(), filter.size())) assetFilter_ = filter.data();
    if (assetsDirty_) refresh_asset_cache();
    ImGui::Separator();
    ImGui::BeginChild("asset-list", ImVec2(0.0f, 0.0f), false);
    for (const auto& entry : projectFiles_) {
        const auto path = entry.relativePath.generic_string();
        if (!assetFilter_.empty() && path.find(assetFilter_) == std::string::npos) continue;
        const std::string label = entry.directory ? "▸  " + path : "    " + path;
        const bool selected = selectedAsset_ == path;
        if (ImGui::Selectable(label.c_str(), selected)) selectedAsset_ = path;
        if (selected) {
            ImGui::SameLine();
            ImGui::TextDisabled(entry.directory ? "folder" : "%llu B", static_cast<unsigned long long>(entry.size));
        }
    }
    ImGui::EndChild();
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
    ImGui::TextDisabled("Editor configuration is persisted as layout + style files");
    if (!ImGui::BeginTabBar("settings-tabs")) { ImGui::End(); return; }
    if (ImGui::BeginTabItem("Appearance")) {
        ImGui::Text("Theme");
        if (ImGui::BeginCombo("##theme", layout_.theme.c_str())) {
            for (const auto& id : themeRegistry_.ids()) {
                const bool selected = id == layout_.theme;
                if (ImGui::Selectable(id.c_str(), selected)) set_theme(id);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        float uiScale = styleConfig_.uiScale;
        if (ImGui::SliderFloat("UI scale", &uiScale, 0.5f, 3.0f, "%.2fx")) {
            styleConfig_.uiScale = uiScale;
            sync_style_to_layout();
            apply_theme();
        }
        float fontSize = styleConfig_.fontSize;
        if (ImGui::SliderFloat("Font size", &fontSize, 8.0f, 32.0f, "%.0f px")) {
            styleConfig_.fontSize = fontSize;
            sync_style_to_layout();
            apply_theme();
        }
        std::array<char, 512> fontPath{};
        std::strncpy(fontPath.data(), styleConfig_.fontPath.c_str(), fontPath.size() - 1);
        if (ImGui::InputTextWithHint("Font file", "Fonts/Editor.ttf", fontPath.data(), fontPath.size())) styleConfig_.fontPath = fontPath.data();
        bool compact = styleConfig_.compactControls;
        if (ImGui::Checkbox("Compact controls", &compact)) { styleConfig_.compactControls = compact; sync_style_to_layout(); apply_theme(); }
        bool reduceMotion = styleConfig_.reduceMotion;
        if (ImGui::Checkbox("Reduce motion", &reduceMotion)) styleConfig_.reduceMotion = reduceMotion;
        ImGui::SeparatorText("Background");
        int mode = static_cast<int>(styleConfig_.background.mode);
        const char* modes[] = {"Solid", "Gradient", "Image"};
        if (ImGui::Combo("Mode", &mode, modes, 3)) styleConfig_.background.mode = static_cast<ui::BackgroundMode>(mode);
        float primary[4]{styleConfig_.background.primary.r, styleConfig_.background.primary.g, styleConfig_.background.primary.b, styleConfig_.background.primary.a};
        if (ImGui::ColorEdit4("Primary", primary, ImGuiColorEditFlags_NoInputs)) {
            styleConfig_.background.primary = {primary[0], primary[1], primary[2], primary[3]};
            sync_style_to_layout();
        }
        float secondary[4]{styleConfig_.background.secondary.r, styleConfig_.background.secondary.g, styleConfig_.background.secondary.b, styleConfig_.background.secondary.a};
        if (ImGui::ColorEdit4("Secondary", secondary, ImGuiColorEditFlags_NoInputs)) styleConfig_.background.secondary = {secondary[0], secondary[1], secondary[2], secondary[3]};
        std::array<char, 512> backgroundPath{};
        std::strncpy(backgroundPath.data(), styleConfig_.background.imagePath.c_str(), backgroundPath.size() - 1);
        if (ImGui::InputText("Background image", backgroundPath.data(), backgroundPath.size())) {
            styleConfig_.background.imagePath = backgroundPath.data();
            sync_style_to_layout();
        }
        if (ImGui::Button("Apply appearance")) { sync_style_to_layout(); apply_theme(); }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Workspace")) {
        ImGui::Text("Workspace");
        std::array<char, 128> workspace{};
        std::strncpy(workspace.data(), layout_.workspace.c_str(), workspace.size() - 1);
        if (ImGui::InputText("Name", workspace.data(), workspace.size())) layout_.workspace = workspace.data();
        ImGui::Text("Layout file: %s", layout_.layoutFile.c_str());
        ImGui::Text("Dock file: %s", layout_.dockLayoutFile.c_str());
        ImGui::Text("Style file: %s", layout_.styleFile.c_str());
        bool allowDocking = layout_.allowDocking;
        if (ImGui::Checkbox("Allow docking", &allowDocking)) layout_.allowDocking = allowDocking;
        bool showToolbar = layout_.showToolbar;
        if (ImGui::Checkbox("Show toolbar", &showToolbar)) layout_.showToolbar = showToolbar;
        bool showStatusBar = layout_.showStatusBar;
        if (ImGui::Checkbox("Show status bar", &showStatusBar)) layout_.showStatusBar = showStatusBar;
        ImGui::SeparatorText("Panels");
        const char* panelNames[] = {"Hierarchy", "Inspector", "Scene", "Game", "Assets", "Console", "Profiler", "Render Graph"};
        const char* panelIds[] = {"hierarchy", "inspector", "viewport", "game", "assets", "console", "profiler", "render-graph"};
        for (int index = 0; index < 8; ++index) {
            bool visible = panel_visible(panelIds[index]);
            if (ImGui::Checkbox(panelNames[index], &visible)) set_panel_visible(panelIds[index], visible);
            if ((index & 1) == 0) ImGui::SameLine(180.0f);
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Files")) {
        ImGui::TextWrapped("The editor reads and writes only inside the configured project root.");
        std::array<char, 512> root{};
        std::strncpy(root.data(), layout_.projectRoot.c_str(), root.size() - 1);
        if (ImGui::InputText("Project root", root.data(), root.size())) set_project_root(root.data());
        ImGui::Text("Resolved root: %s", fileSystem_.root().string().c_str());
        ImGui::TextDisabled("Changes are polled and style files are hot-reloaded at frame boundaries.");
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::Separator();
    if (ImGui::Button("Save all configuration")) save_layout();
    ImGui::SameLine();
    if (ImGui::Button("Reset workspace")) reset_layout();
    ImGui::End();
}

void EditorLayer::draw_media() {
    ImGui::SetNextWindowSize(ImVec2(440.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Media Preview", &layout_.showMedia);
    auto& description = mediaPanel_.description();
    auto& playback = mediaPanel_.playback();
    std::array<char, 512> uri{};
    std::strncpy(uri.data(), description.resource.uri.c_str(), uri.size() - 1);
    if (ImGui::InputTextWithHint("Source URI", "asset://...", uri.data(), uri.size())) description.resource.uri = uri.data();
    ImGui::TextDisabled("Backend-neutral media seam: decoder and GPU texture adapters attach here.");
    int kind = static_cast<int>(description.kind);
    const char* kinds[] = {"Image", "Video", "Audio"};
    if (ImGui::Combo("Type", &kind, kinds, 3)) description.kind = static_cast<ui::MediaKind>(kind);
    ImGui::Separator();
    if (description.kind == ui::MediaKind::Image) {
        ImGui::BeginChild("image-preview", ImVec2(0.0f, 120.0f), true);
        ImGui::Text("Image surface");
        ImGui::TextDisabled("%s", description.resource.uri.c_str());
        ImGui::EndChild();
    } else {
        if (ImGui::Button(playback.state == ui::MediaPlaybackState::Playing ? "Pause" : "Play"))
            mediaPanel_.apply(playback.state == ui::MediaPlaybackState::Playing ? ui::MediaCommand::pause() : ui::MediaCommand::play());
        ImGui::SameLine();
        if (ImGui::Button("Stop")) mediaPanel_.apply(ui::MediaCommand::stop());
        ImGui::SameLine();
        bool loop = playback.loop;
        if (ImGui::Checkbox("Loop", &loop)) mediaPanel_.apply(ui::MediaCommand::set_loop(loop));
        if (description.showTimeline) {
            float duration = static_cast<float>(playback.duration);
            if (duration > 0.0f) {
                float current = static_cast<float>(playback.currentTime);
                if (ImGui::SliderFloat("Timeline", &current, 0.0f, duration, "%.2f s")) mediaPanel_.apply(ui::MediaCommand::seek(current));
            } else ImGui::TextDisabled("Timeline unavailable until the decoder reports duration");
        }
        if (description.showVolume) {
            float volume = static_cast<float>(playback.volume);
            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.0f%%")) mediaPanel_.apply(ui::MediaCommand::set_volume(volume));
        }
        ImGui::TextDisabled("State: %s  |  URI: %s", ui::media_playback_state_name(playback.state), description.resource.uri.c_str());
    }
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
    sync_layout_to_style();
    ui::ImGuiApplyOptions options;
    options.projectRoot = fileSystem_.root();
    options.applyFont = false;
    options.defaultFontFamily = "Microsoft YaHei";
    const auto result = ui::ImGuiAdapter::apply(themeRegistry_, styleConfig_, options);
    const float requestedScale = result.scale;
    if (std::abs(requestedScale - appliedUiScale_) > 0.001f) {
        ImGui::GetStyle().ScaleAllSizes(requestedScale / appliedUiScale_);
        appliedUiScale_ = requestedScale;
    }
    auto& io = ImGui::GetIO();
    io.FontGlobalScale = requestedScale;
    if (io.Fonts && (styleConfig_.fontPath != appliedFontPath_ || std::abs(styleConfig_.fontSize - appliedFontSize_) > 0.01f)) {
        io.Fonts->Clear();
        bool loaded = false;
        std::vector<std::filesystem::path> fontCandidates;
        if (!styleConfig_.fontPath.empty()) fontCandidates.emplace_back(styleConfig_.fontPath);
        if (!styleConfig_.fallbackFontPath.empty()) fontCandidates.emplace_back(styleConfig_.fallbackFontPath);
        fontCandidates.emplace_back(fileSystem_.root() / "Fonts" / "Microsoft YaHei.ttf");
        fontCandidates.emplace_back("C:/Windows/Fonts/msyh.ttc");
        fontCandidates.emplace_back("C:/Windows/Fonts/msyh.ttf");
        for (auto fontPath : fontCandidates) {
            if (fontPath.is_relative()) fontPath = fileSystem_.root() / fontPath;
            if (std::filesystem::exists(fontPath)) {
                loaded = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), styleConfig_.fontSize) != nullptr;
                if (loaded) break;
            }
        }
        if (!loaded) io.Fonts->AddFontDefault();
        io.Fonts->Build();
        appliedFontPath_ = styleConfig_.fontPath;
        appliedFontSize_ = styleConfig_.fontSize;
        push_console(loaded ? "Custom editor font loaded" : "Using default editor font");
    }
}
#endif

void EditorLayer::shutdown() {
    if (!initialized_) return;
    save_layout();
    uninstall_native_menu();
#if defined(SHINKOU_WITH_UIKIT)
    uiKitPanels_.reset();
#endif
#if defined(SHINKOU_WITH_IMGUI)
#if defined(SHINKOU_PLATFORM_WINDOWS)
    if (nativeWindow_) ImGui_ImplWin32_Shutdown();
#endif
    if (uiContextOwned_ && ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
        uiContextOwned_ = false;
    }
#endif
    activeWorld_ = nullptr;
    initialized_ = false;
}

} // namespace shinkou::editor
