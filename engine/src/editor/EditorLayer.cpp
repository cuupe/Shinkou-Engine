#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorGltfPreview.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderScene.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/reflection/Serialization.h"
#include "shinkou/ui/ImGuiAdapter.h"
#include "shinkou/ui/Performance.h"
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#include <commdlg.h>
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

float effective_editor_ui_scale(float dpiScale, float userScale) noexcept {
    const float dpi = std::clamp(std::isfinite(dpiScale) ? dpiScale : 1.0f, 0.25f, 8.0f);
    const float user = std::clamp(std::isfinite(userScale) ? userScale : 1.0f, 0.5f, 3.0f);
    return std::clamp(dpi * user, 0.25f, 8.0f);
}

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
    kNativeBuildPage,
    kNativeDarkTheme,
    kNativeLightTheme,
    kNativeHighContrastTheme,
    kNativeAbout,
    kNativeSaveAs, kNativeSaveLayout, kNativeReloadLayout, kNativeResetLayout,
    kNativeSettings, kNativeDeleteObject, kNativeFrameSelection,
    kNativeBuildProject, kNativeCancelBuild, kNativeRefreshBuildTools,
    kNativeRefreshProjectFiles, kNativeAssociateProjectFile, kNativeGenerateClangdConfig,
    kNativeImportCompileCommands, kNativeExportCompileCommands,
    kNativeOpenVisualStudio, kNativeOpenRider, kNativeOpenVisualStudioCode,
    kNativeSaveBuildProfile, kNativeReloadBuildProfile,
};

void append_native_item(HMENU menu, UINT id, const char* label, bool enabled = true) {
    AppendMenuA(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), id, label);
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
        .field("showBuild", &EditorLayoutState::showBuild)
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

bool EditorLayer::initialize(bool enableImGui) {
    (void)enableImGui;
    if (initialized_) return true;
    register_builtin_panels();
    if (uiComponents_.size() == 0) {
        uiComponents_.emplace<ui::Button>("play", "Play");
        uiComponents_.emplace<ui::Button>("pause", "Pause");
        uiComponents_.emplace<ui::Button>("step", "Step");
        uiComponents_.emplace<ui::Button>("save", "Save");
        uiComponents_.emplace<ui::Button>("build", "Build");
        uiComponents_.emplace<ui::Button>("reset", "Reset");
    }
    build_default_workspace();
    load_layout_file();
    if (!projectRootOverride_.empty()) layout_.projectRoot = projectRootOverride_;
    fileSystem_.set_root(layout_.projectRoot);
    buildSystem_.set_project_root(fileSystem_.root());
    buildSystem_.refresh_toolchains();
    load_build_profile();
    refresh_ide_tools();
    refresh_project_integration();
    set_asset_directory("assets");
    load_style_file();
    sync_style_to_layout();
    load_workspace_file();
    if (!themeRegistry_.find(layout_.theme)) layout_.theme = std::string(kDarkTheme);
    themeRegistry_.switch_theme(layout_.theme);
    sync_page_visibility();
    sync_workspace_visibility();
    refresh_asset_cache();
    install_native_menu();
    // The retained UI owns the portable menu on platforms without a native
    // menu integration. Inject only the capability result so the UI core
    // stays independent of Win32/Cocoa/GTK/etc.
    editorUi_.set_native_main_menu_available(nativeMenu_ != nullptr);
    editorUi_.initialize({
        [this](EditorCommand command, std::string_view target) {
            if (activeWorld_) dispatch_command(command, target, *activeWorld_);
        },
        [this](ObjectId id) {
            layout_.selectedObject = id;
            uiModel_.select_object(id);
            set_selected_asset({});
        },
        [this](ObjectId id, std::string name) {
            if (activeWorld_) {
                if (auto* object = activeWorld_->find_object(id)) {
                    object->set_name(std::move(name));
                    uiModel_.invalidate();
                }
            }
        },
        [this](ObjectId id, bool active) {
            if (activeWorld_) {
                if (auto* object = activeWorld_->find_object(id)) {
                    object->set_active(active);
                    uiModel_.invalidate();
                }
            }
        },
        [this](std::string filter) { uiModel_.set_object_filter(std::move(filter)); },
        [this](std::string path) { set_selected_asset(std::move(path)); },
        [this](EditorAssetAction action, std::string path, std::string value) {
            handle_asset_action(action, std::move(path), std::move(value));
        },
        [this](std::string_view mode) {
            renderViewMode_ = std::string(mode);
            if (activeWorld_) apply_render_view_mode(*activeWorld_, renderViewMode_);
        },
        [this](std::string_view panelId) {
            // Closing a tab is a visibility operation, so it remains
            // reversible through the Window menu and preserves the user's
            // dock topology instead of destroying the panel registration.
            set_panel_visible(panelId, false);
        },
        [this](std::string_view id, std::string_view value) {
            if (id.rfind("build-profile.", 0) == 0) return edit_build_profile_field(id, value);
            return edit_field(id, value);
        },
        [this](ViewportNavigation action, math::Vec2 delta) { navigate_viewport(action, delta); },
        [this](ViewportNavigation action, math::Vec2 delta) { navigate_model_preview(action, delta); },
        [this]() { reset_model_preview(); },
        [this](std::int32_t delta) { select_model_material(delta); },
        [this](std::int32_t delta) { select_model_texture(delta); },
        [this](std::string path, math::Vec2 point) { drop_asset_to_viewport(std::move(path), point); }
    });
#if defined(SHINKOU_WITH_IMGUI)
    imguiEnabled_ = enableImGui;
    if (imguiEnabled_) {
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
        if (nativeWindow_) {
            imguiPlatformInitialized_ = ImGui_ImplWin32_Init(nativeWindow_);
            if (!imguiPlatformInitialized_) push_console("ImGui Win32 platform initialization failed");
        }
#endif
        apply_theme();
    }
#endif
    initialized_ = true;
    push_console("Editor UI initialized");
    return true;
}

void EditorLayer::process_input(const input::InputSystem& input, World& world) {
    if (!initialized_) return;
    activeWorld_ = &world;
    // The component is registered on every active world so a dropped asset
    // reference can survive scene document capture/restore and editor undo.
    world.register_component_type<AssetReferenceComponent>("AssetReference");
    poll_build();
    poll_ide_process();
    // EditorUi is the authoritative editor input route. The previous
    // compatibility runtime was laid out and dispatched every event even
    // though it had no editor host, doubling input work on the frame thread.
    uiInputStats_ = {};
    editorUi_.set_display_size(displayWidth_, displayHeight_, effective_editor_ui_scale(displayDpiScale_, layout_.uiScale));
    editorUi_.process_input(input);
}

void EditorLayer::navigate_viewport(ViewportNavigation action, math::Vec2 delta) {
    if (!activeWorld_ || !math::IsFinite(delta.x) || !math::IsFinite(delta.y)) return;
    bool moved = false;
    activeWorld_->ecs().each<render::CameraComponent, render::TransformComponent>(
        [&](Entity, auto& camera, auto& transform) {
            if (moved || !camera.active || !transform.visible) return;
            moved = true;
            auto& view = transform.local;
            const auto forward = math::Rotate(view.rotation, {0,0,1});
            const auto right = math::Rotate(view.rotation, {1,0,0});
            const auto up = math::Rotate(view.rotation, {0,1,0});
            if (action == ViewportNavigation::Zoom) {
                const float factor = std::exp(-std::clamp(delta.y, -10.0f, 10.0f) * 0.15f);
                if (camera.orthographic) camera.orthographicSize = std::clamp(camera.orthographicSize * factor, 0.05f, 10000.0f);
                else {
                    const auto distance = std::clamp(orbitDistance_ * factor, 0.1f, 10000.0f);
                    view.position = view.position + forward * (orbitDistance_ - distance);
                    orbitDistance_ = distance;
                }
            } else if (action == ViewportNavigation::Pan || camera.orthographic) {
                const auto height = std::max(1.0f, editorUi_.viewport_rect().height);
                const float units = (camera.orthographic ? camera.orthographicSize : 2.0f * orbitDistance_ * std::tan(camera.verticalFieldOfView * 0.5f)) / height;
                view.position = view.position + (right * -delta.x + up * delta.y) * units;
            } else {
                const auto pivot = view.position + forward * orbitDistance_;
                const auto yaw = math::FromAxisAngle({0,1,0}, delta.x * 0.006f);
                const auto pitch = math::FromAxisAngle(right, delta.y * 0.006f);
                view.rotation = math::Normalize(math::Multiply(yaw, math::Multiply(pitch, view.rotation)));
                view.position = pivot - math::Rotate(view.rotation, {0,0,1}) * orbitDistance_;
            }
        });
    if (!moved) lastStatus_ = "No active scene camera";
}

void EditorLayer::apply_render_view_mode(World& world, std::string_view mode) {
    orbitDistance_ = 5.0f;
    bool applied = false;
    world.ecs().each<render::CameraComponent, render::TransformComponent>(
        [mode, &applied](Entity, render::CameraComponent& camera, render::TransformComponent& transform) {
            if (applied || !camera.active || !transform.visible) return;
            applied = true;
            camera.orthographic = mode != "Perspective";
            if (mode == "Front") {
                transform.local.position = {0.0f, 0.0f, -5.0f};
                transform.local.rotation = math::Quat::Identity();
            } else if (mode == "Side" || mode == "Right") {
                transform.local.position = {5.0f, 0.0f, 0.0f};
                transform.local.rotation = math::FromEulerXYZ({0.0f, -math::Pi * 0.5f, 0.0f});
            } else if (mode == "Left") {
                transform.local.position = {-5,0,0}; transform.local.rotation = math::FromEulerXYZ({0,math::Pi*0.5f,0});
            } else if (mode == "Back") {
                transform.local.position = {0,0,5}; transform.local.rotation = math::FromEulerXYZ({0,math::Pi,0});
            } else if (mode == "Bottom") {
                transform.local.position = {0,-5,0}; transform.local.rotation = math::FromEulerXYZ({-math::Pi*0.5f,0,0});
            } else if (mode == "Top") {
                transform.local.position = {0.0f, 5.0f, 0.0f};
                transform.local.rotation = math::FromEulerXYZ({math::Pi * 0.5f, 0.0f, 0.0f});
            } else {
                transform.local.position = {0.0f, 0.0f, -5.0f};
                transform.local.rotation = math::Quat::Identity();
            }
    });
}

void EditorLayer::prepare_frame(render::Renderer& renderer, World& world) {
    if (!initialized_) return;
    activeWorld_ = &world;
    const float uiScale = effective_editor_ui_scale(displayDpiScale_, layout_.uiScale);
    editorUi_.set_display_size(displayWidth_, displayHeight_, uiScale);
    editorUi_.prepare_layout(layout_, dockWorkspace_);

    const auto viewport = editorUi_.viewport_rect();
    if (!layout_.showViewport || viewport.width <= 1.0f || viewport.height <= 1.0f) {
        renderer.clear_editor_viewport();
        return;
    }

    // DockLayout is expressed in logical Slate-style coordinates. Renderer
    // seams are expressed in physical client pixels, just like D3D viewports.
    render::EditorViewportSeam seam;
    seam.viewport = {viewport.x * uiScale, viewport.y * uiScale,
                     viewport.width * uiScale, viewport.height * uiScale};
    seam.scissor = seam.viewport;
    seam.enabled = true;
    seam.strictTarget = true;
    if (!renderer.set_editor_viewport(seam) && !uiPresentationWarningEmitted_) {
        push_console("Editor RenderView seam rejected: " + renderer.last_error());
        uiPresentationWarningEmitted_ = true;
    }
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
    append_native_item(fileMenu, kNativeSaveAs, "Save Scene As...");
    append_native_item(fileMenu, kNativeSaveLayout, "Save Layout");
    append_native_item(fileMenu, kNativeReloadLayout, "Reload Layout");
    append_native_item(fileMenu, kNativeResetLayout, "Reset Layout");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    append_native_item(fileMenu, kNativeExit, "Exit");
    append_native_submenu(mainMenu, fileMenu, "File");

    HMENU editMenu = CreatePopupMenu();
    append_native_item(editMenu, kNativeUndo, "Undo");
    append_native_item(editMenu, kNativeRedo, "Redo");
    append_native_item(editMenu, kNativeDeleteObject, "Delete Object");
    append_native_item(editMenu, kNativeFrameSelection, "Frame Selected");
    append_native_item(editMenu, kNativeSettings, "Editor Settings");
    append_native_submenu(mainMenu, editMenu, "Edit");

    HMENU assetsMenu = CreatePopupMenu();
    append_native_item(assetsMenu, kNativeRefreshAssets, "Refresh");
    append_native_submenu(mainMenu, assetsMenu, "Assets");

    HMENU buildMenu = CreatePopupMenu();
    append_native_item(buildMenu, kNativeBuildProject, "Build Project");
    append_native_item(buildMenu, kNativeCancelBuild, "Cancel Build");
    append_native_item(buildMenu, kNativeRefreshBuildTools, "Refresh Toolchains");
    append_native_item(buildMenu, kNativeRefreshProjectFiles, "Discover Project Files");
    append_native_item(buildMenu, kNativeAssociateProjectFile, "Associate Recommended Project");
    append_native_item(buildMenu, kNativeGenerateClangdConfig, "Generate .clangd");
    append_native_item(buildMenu, kNativeImportCompileCommands, "Import compile_commands.json");
    append_native_item(buildMenu, kNativeExportCompileCommands, "Export compile_commands.json");
    append_native_item(buildMenu, kNativeOpenVisualStudio, "Open in Visual Studio");
    append_native_item(buildMenu, kNativeOpenRider, "Open in Rider");
    append_native_item(buildMenu, kNativeOpenVisualStudioCode, "Open in VS Code");
    append_native_item(buildMenu, kNativeSaveBuildProfile, "Save Build Profile");
    append_native_item(buildMenu, kNativeReloadBuildProfile, "Reload Build Profile");
    append_native_submenu(mainMenu, buildMenu, "Build");

    HMENU gameObjectMenu = CreatePopupMenu();
    append_native_item(gameObjectMenu, kNativeCreateEmpty, "Create Empty");
    append_native_item(gameObjectMenu, kNativeCreateChild, "Create Child");
    HMENU create3dMenu = CreatePopupMenu();
    append_native_item(create3dMenu, kNativeCreate3D, "3D Object (factory unavailable)", false);
    append_native_submenu(gameObjectMenu, create3dMenu, "3D Object");
    HMENU create2dMenu = CreatePopupMenu();
    append_native_item(create2dMenu, kNativeCreate2D, "2D Object (factory unavailable)", false);
    append_native_submenu(gameObjectMenu, create2dMenu, "2D Object");
    append_native_item(gameObjectMenu, kNativeCreateUi, "UI Object (factory unavailable)", false);
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
    append_native_item(windowMenu, kNativeBuildPage, "Build");
    append_native_item(windowMenu, kNativeSettings, "Editor Settings");
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
    if (command == 49000) {
        if (const char* path = std::getenv("SHINKOU_UI_QA_SNAPSHOT")) {
            std::ofstream out(path);
            const char* view = editorUi_.asset_view()==EditorAssetView::Tree ? "tree" : editorUi_.asset_view()==EditorAssetView::LargeIcons ? "large" : "small";
            out << "state " << editorUi_.dpi_scale() << ' ' << std::quoted(editorUi_.selected_asset()) << ' ' << std::quoted(lastStatus_)
                << " view=" << view << " scroll=" << editorUi_.asset_scroll_offset() << " theme=" << layout_.theme << " playing=" << uiModel_.playing() << " paused=" << uiModel_.paused() << '\n';
            world.ecs().each<render::CameraComponent, render::TransformComponent>([&](Entity, const auto& camera, const auto& pose) {
                if(camera.active) out << "camera " << property_text(pose.local.position) << ' ' << property_text(pose.local.rotation) << ' ' << camera.orthographicSize << '\n';
            });
            for (const auto& pair : editorUi_.interaction_regions())
                out << "region " << std::quoted(pair.first) << ' ' << pair.second.x << ' ' << pair.second.y << ' ' << pair.second.width << ' ' << pair.second.height << '\n';
            if (auto* object = world.find_object(layout_.selectedObject)) {
                out << "object " << std::quoted(std::string(object->name())) << '\n';
                for (const auto& field : uiModel_.inspector_fields()) out << "field " << std::quoted(field.label) << ' ' << std::quoted(field.value) << '\n';
            }
            out << "end\n";
        }
        return;
    }
#if defined(SHINKOU_PLATFORM_WINDOWS)
    switch (command) {
    case kNativeNewScene: dispatch_command(EditorCommand::NewScene, {}, world); break;
    case kNativeOpenScene: dispatch_command(EditorCommand::OpenScene, {}, world); break;
    case kNativeSaveScene: dispatch_command(EditorCommand::SaveScene, {}, world); break;
    case kNativeSaveAs: dispatch_command(EditorCommand::SaveSceneAs, {}, world); break;
    case kNativeSaveLayout: dispatch_command(EditorCommand::SaveLayout, {}, world); break;
    case kNativeReloadLayout: dispatch_command(EditorCommand::ReloadLayout, {}, world); break;
    case kNativeResetLayout: dispatch_command(EditorCommand::ResetLayout, {}, world); break;
    case kNativeSettings: dispatch_command(EditorCommand::ProjectSettings, {}, world); break;
    case kNativeDeleteObject: dispatch_command(EditorCommand::DeleteObject, {}, world); break;
    case kNativeFrameSelection: dispatch_command(EditorCommand::FrameSelection, {}, world); break;
    case kNativeExit: dispatch_command(EditorCommand::Quit, {}, world); break;
    case kNativeUndo: dispatch_command(EditorCommand::Undo, {}, world); break;
    case kNativeRedo: dispatch_command(EditorCommand::Redo, {}, world); break;
    case kNativeRefreshAssets: dispatch_command(EditorCommand::RefreshAssets, {}, world); break;
    case kNativeBuildProject: dispatch_command(EditorCommand::BuildProject, {}, world); break;
    case kNativeCancelBuild: dispatch_command(EditorCommand::CancelBuild, {}, world); break;
    case kNativeRefreshBuildTools: dispatch_command(EditorCommand::RefreshBuildTools, {}, world); break;
    case kNativeRefreshProjectFiles: dispatch_command(EditorCommand::RefreshProjectFiles, {}, world); break;
    case kNativeAssociateProjectFile: dispatch_command(EditorCommand::AssociateProjectFile, {}, world); break;
    case kNativeGenerateClangdConfig: dispatch_command(EditorCommand::GenerateClangdConfig, {}, world); break;
    case kNativeImportCompileCommands: dispatch_command(EditorCommand::ImportCompileCommands, {}, world); break;
    case kNativeExportCompileCommands: dispatch_command(EditorCommand::ExportCompileCommands, {}, world); break;
    case kNativeOpenVisualStudio: dispatch_command(EditorCommand::OpenProjectInIde, "visual-studio", world); break;
    case kNativeOpenRider: dispatch_command(EditorCommand::OpenProjectInIde, "rider", world); break;
    case kNativeOpenVisualStudioCode: dispatch_command(EditorCommand::OpenProjectInIde, "vscode", world); break;
    case kNativeSaveBuildProfile: dispatch_command(EditorCommand::SaveBuildProfile, {}, world); break;
    case kNativeReloadBuildProfile: dispatch_command(EditorCommand::ReloadBuildProfile, {}, world); break;
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
    case kNativeBuildPage: dispatch_command(EditorCommand::TogglePage, "build", world); break;
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
    add("build", "Build", false);
}

void EditorLayer::build_default_workspace() {
    auto leaf = [](std::string id, std::string title, bool visible, math::Vec2 minimum) {
        return DockNode::leaf(DockPanel{std::move(id), std::move(title), visible, true, minimum});
    };
    auto bottom = DockNode::tab_stack({
        DockPanel{"assets", "Asset Browser", true, true, {180.0f, 90.0f}},
        DockPanel{"console", "Console", true, true, {180.0f, 90.0f}},
        DockPanel{"build", "Build", false, true, {220.0f, 180.0f}},
    }, 0);
    auto center = DockNode::split(leaf("viewport", "Viewport", true, {320.0f, 220.0f}),
                                  std::move(bottom), DockSplitOrientation::Vertical, 0.64f);
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
            id == "console" || id == "profiler" || id == "render-graph" || id == "settings" || id == "media" || id == "build";
    };
    for (const auto& panel : panels_) {
        if (!isBuiltin(panel.id)) dockWorkspace_.add_tab("viewport", DockPanel{panel.id, panel.title, panel.defaultVisible, true, {160.0f, 100.0f}}, false);
    }
    sync_workspace_visibility();
}

void EditorLayer::sync_workspace_visibility() noexcept {
    for (const auto& [id, visible] : panelVisibility_) dockWorkspace_.set_panel_visibility(id, visible);
    editorUi_.invalidate_layout();
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
    uiModel_.set_page_visible("build", layout_.showBuild);
}

bool EditorLayer::register_panel(EditorPanel panel) {
    if (panel.id.empty() || panel.title.empty() || !panel.draw || panelVisibility_.find(panel.id) != panelVisibility_.end()) return false;
    panelVisibility_[panel.id] = panel.defaultVisible;
    const DockPanel dockPanel{panel.id, panel.title, panel.defaultVisible, panel.closeable, {panel.minWidth, panel.minHeight}};
    panels_.push_back(std::move(panel));
    if (initialized_) {
        dockWorkspace_.add_tab("viewport", dockPanel, false);
        editorUi_.invalidate_layout();
    }
    return true;
}

bool EditorLayer::unregister_panel(std::string_view id) {
    const auto it = std::find_if(panels_.begin(), panels_.end(), [id](const EditorPanel& panel) { return panel.id == id; });
    if (it == panels_.end() || it->id == "hierarchy" || it->id == "inspector" || it->id == "viewport") return false;
    dockWorkspace_.remove_tab(it->id);
    editorUi_.invalidate_layout();
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
    else if (id == "build") layout_.showBuild = visible;
    dockWorkspace_.set_panel_visibility(id, visible);
    editorUi_.invalidate_layout();
    sync_page_visibility();
    return true;
}

bool EditorLayer::panel_visible(std::string_view id) const noexcept {
    const auto it = panelVisibility_.find(std::string(id));
    return it != panelVisibility_.end() && it->second;
}

void EditorLayer::set_project_root(std::string path) {
    clear_media_preview();
    clear_audio_asset_bindings();
    reset_model_scene_assets();
    if (initialized_ && assetSystem_) assetSystemRootNeedsRestart_ = true;
    reset_asset_system_preview("AssetSystem root changed; waiting for resource index...");
    reset_asset_manifest(assetSystemRootNeedsRestart_ ?
        "AssetSystem root changed; restart editor to reconnect resources..." :
        "AssetSystem manifest root changed; waiting for rescan...");
    layout_.projectRoot = std::move(path);
    if (!initialized_) {
        projectRootOverride_ = layout_.projectRoot;
        const auto root = std::filesystem::absolute(layout_.projectRoot).lexically_normal();
        if (std::filesystem::path(layout_.layoutFile).is_relative()) set_layout_path((root/layout_.layoutFile).generic_string());
    }
    fileSystem_.set_root(layout_.projectRoot);
    buildSystem_.set_project_root(fileSystem_.root());
    load_build_profile();
    refresh_ide_tools();
    refresh_project_integration();
    ++assetPreviewGeneration_;
    assetPreviewStamp_ = 0;
    ++imagePreviewGeneration_;
    imagePreviewStamp_ = 0;
    ++audioPreviewGeneration_;
    audioPreviewStamp_ = 0;
    if (audioPreviewCancel_) audioPreviewCancel_->store(true, std::memory_order_relaxed);
    audioPreviewCancel_.reset();
    audioPreviewSnapshot_.reset();
    audioPreviewStatus_ = "Audio preview not loaded";
    ++videoPreviewGeneration_;
    videoPreviewStamp_ = 0;
    if (videoPreviewCancel_) videoPreviewCancel_->store(true, std::memory_order_relaxed);
    videoPreviewCancel_.reset();
    if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
    videoFrameCancel_.reset();
    pendingVideoSeekSeconds_ = -1.0;
    videoPreviewSnapshot_.reset();
    videoPreviewStatus_ = "Video preview not loaded";
    ++modelPreviewGeneration_;
    modelPreviewStamp_ = 0;
    if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
    modelPreviewCancel_.reset();
    modelPreviewSnapshot_.reset();
    modelPreviewScene_.clear();
    modelPreviewStatus_ = "Model preview not loaded";
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    modelTexturePreviewPath_.clear();
    modelMaterialSelection_ = -1;
    modelTextureSelection_ = -1;
    selectedAsset_.clear();
    editorUi_.select_asset({});
    assetPreviewState_ = {};
    uiModel_.set_asset_preview(assetPreviewState_);
    compileCommands_.clear();
    compileCommandsStatus_ = "Not loaded";
    buildUiDirty_ = true;
    assetIndex_.reset();
    assetDirectory_.clear();
    editorUi_.set_asset_directory({});
    set_asset_directory("assets");
    ++fileScanGeneration_;
    assetsDirty_ = true;
    if (initialized_) request_asset_manifest_scan();
}

void EditorLayer::set_asset_system(assets::AssetSystem* assetSystem) {
    if (assetSystem_ == assetSystem) {
        if (initialized_) request_asset_manifest_scan();
        return;
    }
    // A manifest scan captures the current AssetSystem pointer in its worker.
    // Drain it before allowing the owner to replace or destroy that system.
    if (assetManifestFuture_.valid()) {
        assetManifestFuture_.wait();
        try { assetManifestFuture_.get(); } catch (...) { }
    }
    if (assetSystemPreviewFuture_.valid()) {
        assetSystemPreviewFuture_.wait();
        try { assetSystemPreviewFuture_.get(); } catch (...) { }
    }
    // Image preview workers may wait on an AssetSystem request and then decode
    // its immutable bytes. Drain that chain before replacing the owner pointer.
    if (imagePreviewFuture_.valid()) {
        imagePreviewFuture_.wait();
        try { imagePreviewFuture_.get(); } catch (...) { }
    }
    if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
    if (modelPreviewFuture_.valid()) {
        modelPreviewFuture_.wait();
        try { modelPreviewFuture_.get(); } catch (...) { }
    }
    clear_audio_asset_bindings();
    reset_model_scene_assets();
    assetSystem_ = assetSystem;
    if (!initialized_) assetSystemRootNeedsRestart_ = false;
    reset_asset_manifest(assetSystem_ ? "Waiting for AssetSystem manifest..." :
        "AssetSystem manifest not connected");
    if (initialized_) request_asset_manifest_scan();
}

void EditorLayer::set_audio_system(audio::AudioSystem* audioSystem) noexcept {
    if (audioSystem_ == audioSystem) return;
    clear_audio_asset_bindings();
    audioSystem_ = audioSystem;
}

void EditorLayer::set_layout_path(std::string path) {
    layout_.layoutFile = std::move(path);
    layout_.dockLayoutFile = layout_.layoutFile + ".dock.json";
    layout_.styleFile = layout_.layoutFile + ".style.json";
}

void EditorLayer::set_style_file(std::string path) {
    if (!path.empty()) layout_.styleFile = std::move(path);
}

void EditorLayer::set_display_size(float width, float height, float dpiScale) noexcept {
    displayWidth_ = std::max(width, 1.0f);
    displayHeight_ = std::max(height, 1.0f);
    displayDpiScale_ = std::clamp(dpiScale, 0.25f, 8.0f);
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
    if (loaded.layoutVersion > 3) {
        lastStatus_ = "Layout load failed: unsupported layout version";
        return false;
    }
    if (loaded.layoutVersion == 0) loaded.layoutVersion = 1;
    loaded.layoutVersion = 3;
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
    set_panel_visible("build", layout_.showBuild);
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
    clear_media_preview();
    const bool loaded = load_layout_file();
    if (!projectRootOverride_.empty()) layout_.projectRoot = projectRootOverride_;
    fileSystem_.set_root(layout_.projectRoot);
    buildSystem_.set_project_root(fileSystem_.root());
    load_build_profile();
    refresh_ide_tools();
    refresh_project_integration();
    ++assetPreviewGeneration_;
    assetPreviewStamp_ = 0;
    ++imagePreviewGeneration_;
    imagePreviewStamp_ = 0;
    ++audioPreviewGeneration_;
    audioPreviewStamp_ = 0;
    if (audioPreviewCancel_) audioPreviewCancel_->store(true, std::memory_order_relaxed);
    audioPreviewCancel_.reset();
    audioPreviewSnapshot_.reset();
    audioPreviewStatus_ = "Audio preview not loaded";
    ++videoPreviewGeneration_;
    videoPreviewStamp_ = 0;
    if (videoPreviewCancel_) videoPreviewCancel_->store(true, std::memory_order_relaxed);
    videoPreviewCancel_.reset();
    if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
    videoFrameCancel_.reset();
    pendingVideoSeekSeconds_ = -1.0;
    videoPreviewSnapshot_.reset();
    videoPreviewStatus_ = "Video preview not loaded";
    ++modelPreviewGeneration_;
    modelPreviewStamp_ = 0;
    if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
    modelPreviewCancel_.reset();
    modelPreviewSnapshot_.reset();
    modelPreviewScene_.clear();
    modelPreviewStatus_ = "Model preview not loaded";
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    modelTexturePreviewPath_.clear();
    modelMaterialSelection_ = -1;
    modelTextureSelection_ = -1;
    assetPreviewState_ = {};
    uiModel_.set_asset_preview(assetPreviewState_);
    compileCommands_.clear();
    compileCommandsStatus_ = "Not loaded";
    buildUiDirty_ = true;
    assetIndex_.reset();
    assetDirectory_.clear();
    editorUi_.set_asset_directory({});
    set_asset_directory("assets");
    ++fileScanGeneration_;
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
    clear_media_preview();
    const auto layoutPath = layout_.layoutFile;
    const auto dockPath = layout_.dockLayoutFile;
    const auto stylePath = layout_.styleFile;
    const auto selection = layout_.selectedObject;
    layout_ = EditorLayoutState{};
    layout_.layoutFile = layoutPath; layout_.dockLayoutFile = dockPath; layout_.styleFile = stylePath;
    layout_.selectedObject = selection;
    if (!projectRootOverride_.empty()) layout_.projectRoot = projectRootOverride_;
    styleConfig_ = ui::UiStyleConfig{};
    ++assetPreviewGeneration_;
    assetPreviewStamp_ = 0;
    ++imagePreviewGeneration_;
    imagePreviewStamp_ = 0;
    ++audioPreviewGeneration_;
    audioPreviewStamp_ = 0;
    if (audioPreviewCancel_) audioPreviewCancel_->store(true, std::memory_order_relaxed);
    audioPreviewCancel_.reset();
    audioPreviewSnapshot_.reset();
    audioPreviewStatus_ = "Audio preview not loaded";
    ++videoPreviewGeneration_;
    videoPreviewStamp_ = 0;
    if (videoPreviewCancel_) videoPreviewCancel_->store(true, std::memory_order_relaxed);
    videoPreviewCancel_.reset();
    if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
    videoFrameCancel_.reset();
    pendingVideoSeekSeconds_ = -1.0;
    videoPreviewSnapshot_.reset();
    videoPreviewStatus_ = "Video preview not loaded";
    ++modelPreviewGeneration_;
    modelPreviewStamp_ = 0;
    if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
    modelPreviewCancel_.reset();
    modelPreviewSnapshot_.reset();
    modelPreviewScene_.clear();
    modelPreviewStatus_ = "Model preview not loaded";
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    modelTexturePreviewPath_.clear();
    modelMaterialSelection_ = -1;
    modelTextureSelection_ = -1;
    assetPreviewState_ = {};
    uiModel_.set_asset_preview(assetPreviewState_);
    fileSystem_.set_root(layout_.projectRoot);
    buildSystem_.set_project_root(fileSystem_.root());
    load_build_profile();
    refresh_ide_tools();
    refresh_project_integration();
    compileCommands_.clear();
    compileCommandsStatus_ = "Not loaded";
    buildUiDirty_ = true;
    assetIndex_.reset();
    assetDirectory_.clear();
    editorUi_.set_asset_directory({});
    set_asset_directory("assets");
    ++fileScanGeneration_;
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
    editorUi_.invalidate_layout();
#if defined(SHINKOU_WITH_IMGUI)
    apply_theme();
#endif
}

void EditorLayer::push_console(std::string message) {
    if (message.empty()) return;
    consoleEntries_.push_back(std::move(message));
    if (consoleEntries_.size() > 256) consoleEntries_.erase(consoleEntries_.begin(), consoleEntries_.begin() + 64);
}

bool EditorLayer::checkpoint(World& world) {
    EditorDocument document; std::string error;
    if (!EditorDocument::capture(world, layout_.selectedObject, document, error)) { lastStatus_ = error; return false; }
    undo_.push_back(std::move(document));
    if (undo_.size() > 64) undo_.erase(undo_.begin());
    return true;
}

void EditorLayer::document_changed(bool preserveRedo) {
    if (!preserveRedo) redo_.clear();
    sceneDirty_ = true;
    selectedAsset_.clear(); editorUi_.select_asset({});
    uiModel_.select_object(layout_.selectedObject); uiModel_.invalidate();
    editorUi_.invalidate_layout();
}

bool EditorLayer::consume_simulation_step() noexcept {
    if (stepPending_) { stepPending_ = false; return true; }
    return uiModel_.playing() && !uiModel_.paused();
}

bool EditorLayer::edit_field(std::string_view id, std::string_view value) {
    if (!activeWorld_) return false;
    auto* object = activeWorld_->find_object(layout_.selectedObject);
    if (!object) return false;
    PropertyValue parsed;
    std::function<bool()> apply;
    if (id == "name") {
        if (value.empty()) return false;
        apply = [object, value] { object->set_name(std::string(value)); return true; };
    } else if (id == "active") {
        if (!parse_property_text(PropertyType::Bool, value, parsed)) return false;
        apply = [&] { object->set_active(std::get<bool>(parsed)); return true; };
    } else {
        const bool enabled = id.substr(0, 8) == "enabled:";
        const auto colon = id.find(':');
        if (colon == std::string_view::npos) return false;
        ComponentId cid{};
        try { cid = std::stoull(std::string(enabled ? id.substr(8) : id.substr(0, colon))); } catch (...) { return false; }
        auto* component = activeWorld_->find_component(cid);
        if (!component || component->try_game_object() != object) return false;
        if (enabled) {
            if (!parse_property_text(PropertyType::Bool, value, parsed)) return false;
            apply = [&, component] { component->set_enabled(std::get<bool>(parsed)); return true; };
        } else {
            const auto name = id.substr(colon + 1);
            for (auto& p : component->properties()) {
                if (p.name != name || !p.editable()) continue;
                if (!parse_property_text(p.type, value, parsed)) return false;
                apply = [setter = p.set, parsed] { return setter(parsed); }; break;
            }
        }
    }
    if (!apply || !checkpoint(*activeWorld_)) return false;
    if (!apply()) { undo_.pop_back(); lastStatus_ = "Invalid property value or range"; return false; }
    document_changed(); lastStatus_ = "Property updated"; return true;
}

void EditorLayer::dispatch_command(EditorCommand command, std::string_view target, World& world) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    std::string selectedPath;
    if (nativeWindow_ && target.empty() && (command == EditorCommand::OpenScene || command == EditorCommand::SaveSceneAs)) {
        wchar_t path[32768]{};
        const auto initial = (fileSystem_.root()/scenePath_).wstring();
        std::copy_n(initial.c_str(),std::min(initial.size(),std::size(path)-1),path);
        OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=static_cast<HWND>(nativeWindow_);
        dialog.lpstrFilter=L"Shinkou scene\0*.scene\0All files\0*.*\0"; dialog.lpstrFile=path; dialog.nMaxFile=static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt=L"scene"; dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(command==EditorCommand::OpenScene ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
        const bool chosen=command==EditorCommand::OpenScene ? GetOpenFileNameW(&dialog)!=0 : GetSaveFileNameW(&dialog)!=0;
        if (!chosen) return;
        selectedPath=std::filesystem::path(path).lexically_relative(fileSystem_.root()).generic_string(); target=selectedPath;
    }
    if (command==EditorCommand::Quit && sceneDirty_ && nativeWindow_) {
        const auto answer=MessageBoxW(static_cast<HWND>(nativeWindow_),L"Save scene changes before closing?",L"Shinkou Editor",MB_YESNOCANCEL|MB_ICONQUESTION);
        if(answer==IDCANCEL) return;
        if(answer==IDYES) { dispatch_command(EditorCommand::SaveScene,{},world); if(sceneDirty_) return; }
    }
#endif
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
            page == "settings" ? layout_.showSettings : page == "build" ? layout_.showBuild :
            page == "media" ? layout_.showMedia : false;
        const auto panelId = page == "scene" ? std::string_view{"viewport"} :
            page == "project" ? std::string_view{"assets"} : page;
        if (!visible && page == "build" && !dockWorkspace_.activate_tab(panelId)) {
            if (!dockWorkspace_.add_tab("console", DockPanel{"build", "Build", false, true, {220.0f, 180.0f}}, false))
                dockWorkspace_.add_tab("viewport", DockPanel{"build", "Build", false, true, {220.0f, 180.0f}}, false);
        }
        if (page == "scene") set_panel_visible("viewport", !visible);
        else set_panel_visible(page == "project" ? "assets" : page, !visible);
        if (!visible) dockWorkspace_.activate_tab(panelId);
        break;
    }
    case EditorCommand::Create3DObject:
    case EditorCommand::Create2DObject:
    case EditorCommand::CreateUiObject:
        lastStatus_ = "This object factory is not registered"; break;
    case EditorCommand::CreateEmpty: {
        if (!checkpoint(world)) break;
        const char* name = "GameObject";
        auto& object = world.create_object(name);
        layout_.selectedObject = object.id();
        uiModel_.select_object(object.id());
        uiModel_.invalidate();
        document_changed();
        push_console(std::string("Created ") + name);
        break;
    }
    case EditorCommand::CreateChild: {
        if (!checkpoint(world)) break;
        auto* parent = world.find_object(layout_.selectedObject);
        auto& object = parent ? parent->create_child("GameObject") : world.create_object("GameObject");
        layout_.selectedObject = object.id();
        uiModel_.select_object(object.id());
        uiModel_.invalidate();
        document_changed();
        push_console("Created child GameObject");
        break;
    }
    case EditorCommand::ProjectSettings: set_panel_visible("settings", true); dockWorkspace_.activate_tab("settings"); break;
    case EditorCommand::RefreshAssets:
        reset_asset_manifest("AssetSystem manifest refresh requested");
        assetsDirty_ = true;
        push_console("Asset browser refresh requested");
        break;
    case EditorCommand::OpenAsset: {
        const auto path = std::filesystem::path(target).lexically_normal();
        bool directory = false;
        if (target.empty() || path.is_absolute() || !fileSystem_.exists(path, &directory) || directory) {
            lastStatus_ = "Resource is outside the project or unavailable";
            break;
        }
        set_selected_asset(path.generic_string());
        set_panel_visible("inspector", true);
        dockWorkspace_.activate_tab("inspector");
        lastStatus_ = "Opened resource " + path.generic_string();
        break;
    }
    case EditorCommand::BuildProject:
        if (!dockWorkspace_.activate_tab("build")) {
            if (!dockWorkspace_.add_tab("console", DockPanel{"build", "Build", false, true, {220.0f, 180.0f}}, false))
                dockWorkspace_.add_tab("viewport", DockPanel{"build", "Build", false, true, {220.0f, 180.0f}}, false);
        }
        set_panel_visible("build", true);
        dockWorkspace_.activate_tab("build");
        start_build();
        break;
    case EditorCommand::CancelBuild: cancel_build(); break;
    case EditorCommand::RefreshBuildTools:
        buildSystem_.refresh_toolchains();
        refresh_ide_tools();
        refresh_project_integration();
        buildStatus_ = "Toolchains refreshed";
        buildUiDirty_ = true;
        lastStatus_ = "Build toolchains refreshed";
        push_console(lastStatus_);
        break;
    case EditorCommand::RefreshProjectFiles:
        refresh_project_integration();
        lastStatus_ = projectIntegrationStatus_;
        push_console(lastStatus_);
        break;
    case EditorCommand::AssociateProjectFile:
        associate_project_file(target);
        break;
    case EditorCommand::GenerateClangdConfig:
        generate_clangd_config();
        break;
    case EditorCommand::ImportCompileCommands:
        import_compile_commands(target);
        break;
    case EditorCommand::ExportCompileCommands:
        export_compile_commands(target);
        break;
    case EditorCommand::OpenProjectInIde:
        open_project_in_ide(target);
        break;
    case EditorCommand::SaveBuildProfile:
        save_build_profile();
        break;
    case EditorCommand::ReloadBuildProfile:
        load_build_profile();
        refresh_project_integration();
        break;
    case EditorCommand::SelectBuildProfile:
        select_build_profile(target);
        break;
    case EditorCommand::SetDiagnosticFilter:
        set_diagnostic_filter(target);
        break;
    case EditorCommand::SelectBuildDiagnostic:
        select_build_diagnostic(target);
        break;
    case EditorCommand::OpenDiagnosticInIde:
        open_diagnostic_in_ide(target);
        break;
    case EditorCommand::MediaPlay:
        start_audio_preview();
        break;
    case EditorCommand::MediaPause:
        if (audioSystem_ && audioPreviewVoice_ != 0 &&
            audioSystem_->state(audioPreviewVoice_) == audio::AudioVoiceState::Playing) {
            audioSystem_->pause(audioPreviewVoice_);
            mediaPanel_.playback().state = ui::MediaPlaybackState::Paused;
            lastStatus_ = "Audio preview paused";
            sync_media_preview_state();
        }
        break;
    case EditorCommand::MediaStop:
        stop_audio_preview();
        lastStatus_ = "Audio preview stopped";
        sync_media_preview_state();
        break;
    case EditorCommand::MediaToggleLoop:
        mediaPanel_.playback().loop = !mediaPanel_.playback().loop;
        lastStatus_ = mediaPanel_.playback().loop ? "Audio preview loop enabled" : "Audio preview loop disabled";
        sync_media_preview_state();
        break;
    case EditorCommand::MediaSeek: {
        try {
            const auto normalized = std::stod(std::string(target));
            if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0 ||
                mediaPanel_.playback().duration <= 0.0) throw std::invalid_argument("seek");
            const auto seconds = normalized * mediaPanel_.playback().duration;
            mediaPanel_.apply(ui::MediaCommand::seek(seconds));
            mediaPanel_.playback().currentTime = seconds;
            if (mediaPanel_.description().kind == ui::MediaKind::Audio) {
                if (audioSystem_ && audioPreviewVoice_ != 0) audioSystem_->seek(audioPreviewVoice_, seconds);
            } else if (mediaPanel_.description().kind == ui::MediaKind::Video) {
                if (!videoPreviewSnapshot_ || !videoPreviewSnapshot_->valid())
                    throw std::invalid_argument("video frame unavailable");
                request_video_frame(seconds);
            }
            sync_media_preview_state();
        } catch (...) {
            lastStatus_ = "Media preview seek is unavailable";
        }
        break;
    }
    case EditorCommand::MediaSetVolume: {
        try {
            const auto value = std::stod(std::string(target));
            if (!std::isfinite(value) || value < 0.0 || value > 1.0) throw std::invalid_argument("volume");
            mediaPanel_.playback().volume = value;
            if (audioSystem_ && audioPreviewVoice_ != 0) audioSystem_->set_volume(audioPreviewVoice_, static_cast<float>(value));
            sync_media_preview_state();
        } catch (...) {
            lastStatus_ = "Audio preview volume must be between 0 and 1";
        }
        break;
    }
    case EditorCommand::SetDarkTheme: set_theme("dark"); break;
    case EditorCommand::SetLightTheme: set_theme("light"); break;
    case EditorCommand::SetHighContrastTheme: set_theme("high-contrast"); break;
    case EditorCommand::Play: lastStatus_ = uiModel_.playing() ? "Simulation running" : "Simulation stopped"; break;
    case EditorCommand::Pause: lastStatus_ = uiModel_.paused() ? "Simulation paused" : "Simulation running"; break;
    case EditorCommand::Step: stepPending_ = true; lastStatus_ = "Advance one simulation frame"; break;
    case EditorCommand::Undo:
    case EditorCommand::Redo: {
        auto& source = command == EditorCommand::Undo ? undo_ : redo_;
        auto& destination = command == EditorCommand::Undo ? redo_ : undo_;
        if (source.empty()) { lastStatus_ = "No edit history"; break; }
        EditorDocument current; std::string error;
        if (!EditorDocument::capture(world, layout_.selectedObject, current, error) ||
            !source.back().restore(world, layout_.selectedObject, error)) { lastStatus_ = error; break; }
        destination.push_back(std::move(current)); source.pop_back();
        document_changed(true); lastStatus_ = command == EditorCommand::Undo ? "Edit undone" : "Edit redone"; break;
    }
    case EditorCommand::NewScene: {
        if (!checkpoint(world)) break;
        EditorDocument empty; std::string error;
        if (empty.restore(world, layout_.selectedObject, error)) { document_changed(); lastStatus_ = "New scene (Undo to restore)"; }
        else lastStatus_ = error;
        break;
    }
    case EditorCommand::OpenScene: {
        const std::string path = target.empty() ? scenePath_ : std::string(target);
        std::string json, error; EditorDocument next;
        if (!fileSystem_.read_text(path, json, &error) || !EditorDocument::from_json(json, next, error)) { lastStatus_ = "Open failed: " + error; break; }
        if (!checkpoint(world)) break;
        if (!next.restore(world, layout_.selectedObject, error)) { undo_.pop_back(); lastStatus_ = "Open failed: " + error; break; }
        scenePath_ = path; document_changed(); sceneDirty_ = false; lastStatus_ = "Opened " + path; break;
    }
    case EditorCommand::SaveScene:
    case EditorCommand::SaveSceneAs: {
        const std::string path = target.empty() ? scenePath_ : std::string(target);
        EditorDocument document; std::string json, error;
        if (!EditorDocument::capture(world, layout_.selectedObject, document, error) || !document.to_json(json, error) ||
            !fileSystem_.write_text_atomic(path, json, &error)) { lastStatus_ = "Save failed: " + error; break; }
        scenePath_ = path; sceneDirty_ = false; assetsDirty_ = true; lastStatus_ = "Saved " + path; break;
    }
    case EditorCommand::FrameSelection: {
        auto* object = world.find_object(layout_.selectedObject);
        if (!object) { lastStatus_ = "Select an object first"; break; }
        const auto* transform = object->get_component<components::TransformComponent>();
        auto pose = transform ? transform->local : math::Transform{};
        for (auto* parent = object->parent(); parent; parent = parent->parent())
            if (const auto* t = parent->get_component<components::TransformComponent>()) pose = math::Combine(t->local, pose);
        const auto center = pose.position;
        orbitDistance_ = 5.0f;
        bool framed = false;
        world.ecs().each<render::CameraComponent, render::TransformComponent>([&](Entity, auto& camera, auto& view) {
            if (framed || !camera.active || !view.visible) return;
            framed = true;
            view.local.position = {center.x, center.y, center.z - 5.0f}; view.local.rotation = math::Quat::Identity();
        });
        lastStatus_ = framed ? "Framed " + std::string(object->name()) : "No active scene camera"; break;
    }
    case EditorCommand::AddComponent: {
        auto* object = world.find_object(layout_.selectedObject);
        if (!object) { lastStatus_ = "Select an object first"; break; }
        if (target.empty()) { set_panel_visible("inspector", true); lastStatus_ = "Choose a component in Inspector"; break; }
        if (!checkpoint(world)) break;
        if (!object->add_component(target)) { undo_.pop_back(); lastStatus_ = "Component is unknown or already attached"; break; }
        document_changed(); lastStatus_ = "Added " + std::string(target); break;
    }
    case EditorCommand::DeleteObject: {
        auto* object = world.find_object(layout_.selectedObject);
        if (object && checkpoint(world)) { object->destroy(); layout_.selectedObject = 0; document_changed(); lastStatus_ = "Deleted object (Undo to restore)"; }
        break;
    }
    case EditorCommand::ClearConsole: consoleEntries_.clear(); lastStatus_ = "Console cleared"; break;
    case EditorCommand::ToggleCompact: layout_.compactControls = !layout_.compactControls; editorUi_.invalidate_layout(); break;
    case EditorCommand::ToggleDocking: layout_.allowDocking = !layout_.allowDocking; editorUi_.invalidate_layout(); break;
    case EditorCommand::SetUiScale: {
        PropertyValue v;
        if (parse_property_text(PropertyType::Number, target, v)) { layout_.uiScale = std::clamp(static_cast<float>(std::get<double>(v)), 0.5f, 3.0f); editorUi_.invalidate_layout(); }
        break;
    }
    case EditorCommand::Quit: quitRequested_ = true; break;
    default: break;
    }
}

void EditorLayer::refresh_asset_cache() {
    request_file_scan();
    request_asset_manifest_scan();
}

void EditorLayer::poll_editor_files() {
    request_file_scan();
}

void EditorLayer::request_file_scan() {
    if (fileScanFuture_.valid()) return;
    const auto generation = fileScanGeneration_;
    const auto directory = assetDirectory_;
    auto scanner = fileSystem_;
    fileScanFuture_ = std::async(std::launch::async,
        [generation, directory, scanner = std::move(scanner)]() mutable {
            AsyncFileScan result;
            result.generation = generation;
            // A project root without an assets folder stays cheap to open.
            // Once the user enters a directory, that directory is scanned
            // recursively so Tree view can represent its real hierarchy.
            const auto scan = scanner.scan(directory, !directory.empty(), 32768);
            result.entries = scan.entries;
            result.changes = scan.changes;
            result.index = EditorAssetIndex::build(result.entries, generation);
            result.service = std::move(scanner);
            return result;
        });
}

void EditorLayer::set_asset_directory(std::filesystem::path directory) {
    directory = directory.lexically_normal();
    if (directory == ".") directory.clear();
    bool isDirectory = false;
    if (!directory.empty() && !fileSystem_.exists(directory, &isDirectory)) {
        lastStatus_ = "Folder does not exist: " + directory.generic_string();
        return;
    }
    if (!directory.empty() && !isDirectory) {
        lastStatus_ = "Not a folder: " + directory.generic_string();
        return;
    }
    if (assetDirectory_ == directory) return;
    assetDirectory_ = std::move(directory);
    editorUi_.set_asset_directory(assetDirectory_);
    ++fileScanGeneration_;
    assetsDirty_ = true;
    lastStatus_ = assetDirectory_.empty() ? "Opened project root" :
        "Opened " + assetDirectory_.generic_string();
}

void EditorLayer::reset_asset_manifest(std::string status) {
    ++assetManifestGeneration_;
    assetManifestDirty_ = true;
    assetManifest_.reset();
    assetManifestStatus_ = std::move(status);
}

void EditorLayer::request_asset_manifest_scan() {
    if (!assetManifestDirty_ && !assetManifestFuture_.valid()) return;
    if (!assetSystem_) {
        assetManifestStatus_ = "AssetSystem manifest not connected";
        return;
    }
    if (assetSystemRootNeedsRestart_) {
        assetManifestStatus_ = "AssetSystem root changed; restart editor to reconnect resources";
        return;
    }
    if (!assetSystem_->initialized()) {
        assetManifestStatus_ = "AssetSystem manifest unavailable";
        return;
    }
    if (assetManifestFuture_.valid()) return;
    const auto generation = assetManifestGeneration_;
    auto* system = assetSystem_;
    const auto manifestPath = fileSystem_.root() / ".shinkou" / "manifest.json";
    assetManifestStatus_ = "Scanning AssetSystem manifest...";
    try {
        assetManifestFuture_ = std::async(std::launch::async, [generation, system, manifestPath]() {
            AsyncAssetManifest result;
            result.generation = generation;
            try {
                std::error_code manifestError;
                if (std::filesystem::exists(manifestPath, manifestError) && !manifestError) {
                    const auto readback = assets::AssetSystem::read_manifest(manifestPath);
                    if (readback) {
                        std::string seedError;
                        if (system->seed_manifest_cache(readback.entries, &seedError)) {
                            result.readbackValidated = true;
                        } else {
                            result.readbackError = seedError.empty() ?
                                "manifest cache seed was rejected" : seedError;
                        }
                    } else {
                        result.readbackError = readback.error.empty() ?
                            "manifest readback failed" : readback.error;
                    }
                }
                auto entries = system->scan_sources();
                result.entries = std::make_shared<const std::vector<assets::AssetManifestEntry>>(std::move(entries));
            } catch (const std::exception& error) {
                result.error = error.what();
            } catch (...) {
                result.error = "unknown AssetSystem manifest worker failure";
            }
            return result;
        });
        assetManifestDirty_ = false;
    } catch (const std::exception& error) {
        assetManifestStatus_ = "AssetSystem manifest unavailable: " + std::string(error.what());
    } catch (...) {
        assetManifestStatus_ = "AssetSystem manifest unavailable";
    }
}

void EditorLayer::poll_asset_manifest_scan() {
    if (!assetManifestFuture_.valid() ||
        assetManifestFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    AsyncAssetManifest result;
    try { result = assetManifestFuture_.get(); }
    catch (const std::exception& error) {
        assetManifestDirty_ = false;
        assetManifestStatus_ = "AssetSystem manifest failed: " + std::string(error.what());
        return;
    } catch (...) {
        assetManifestDirty_ = false;
        assetManifestStatus_ = "AssetSystem manifest failed";
        return;
    }
    if (assetSystemRootNeedsRestart_) {
        assetManifest_.reset();
        assetManifestStatus_ = "AssetSystem root changed; restart editor to reconnect resources";
        return;
    }
    if (result.generation != assetManifestGeneration_) {
        assetManifestDirty_ = true;
        request_asset_manifest_scan();
        return;
    }
    if (!result.error.empty() || !result.entries) {
        assetManifestDirty_ = false;
        assetManifest_.reset();
        assetManifestStatus_ = result.error.empty() ?
            "AssetSystem manifest failed" : "AssetSystem manifest failed: " + result.error;
        lastStatus_ = assetManifestStatus_;
        push_console(lastStatus_);
        return;
    }
    assetManifest_ = std::move(result.entries);
    assetManifestDirty_ = false;
    assetManifestStatus_ = "AssetSystem manifest ready: " + std::to_string(assetManifest_->size()) + " resources";
    if (result.readbackValidated) assetManifestStatus_ += " (validated cache)";
    if (!result.readbackError.empty()) assetManifestStatus_ += " (readback fallback: " + result.readbackError + ")";
    lastStatus_ = assetManifestStatus_;
    push_console(lastStatus_);
}

void EditorLayer::set_selected_asset(std::string path) {
    if (selectedAsset_ == path && editorUi_.selected_asset() == path) {
        request_asset_preview();
        return;
    }
    selectedAsset_ = std::move(path);
    editorUi_.select_asset(selectedAsset_);
    reset_asset_system_preview(selectedAsset_.empty() ? "AssetSystem not connected" :
        "Waiting for AssetSystem resource index...");
    if (mediaPanel_.description().resource.uri != selectedAsset_) {
        clear_media_preview();
    }
    ++assetPreviewGeneration_;
    assetPreviewStamp_ = 0;
    ++imagePreviewGeneration_;
    imagePreviewStamp_ = 0;
    ++audioPreviewGeneration_;
    audioPreviewStamp_ = 0;
    if (audioPreviewCancel_) audioPreviewCancel_->store(true, std::memory_order_relaxed);
    audioPreviewCancel_.reset();
    audioPreviewSnapshot_.reset();
    audioPreviewStatus_ = "Audio preview not loaded";
    ++videoPreviewGeneration_;
    videoPreviewStamp_ = 0;
    if (videoPreviewCancel_) videoPreviewCancel_->store(true, std::memory_order_relaxed);
    videoPreviewCancel_.reset();
    if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
    videoFrameCancel_.reset();
    pendingVideoSeekSeconds_ = -1.0;
    videoPreviewSnapshot_.reset();
    videoPreviewStatus_ = "Video preview not loaded";
    ++modelPreviewGeneration_;
    modelPreviewStamp_ = 0;
    if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
    modelPreviewCancel_.reset();
    modelPreviewSnapshot_.reset();
    modelPreviewScene_.clear();
    modelPreviewStatus_ = "Model preview not loaded";
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    modelTexturePreviewPath_.clear();
    modelMaterialSelection_ = -1;
    modelTextureSelection_ = -1;
    assetPreviewState_ = {};
    assetPreviewState_.path = selectedAsset_;
    assetPreviewState_.loading = !selectedAsset_.empty();
    assetPreviewState_.status = selectedAsset_.empty() ? std::string{} : "Waiting for asset index...";
    uiModel_.set_asset_preview(assetPreviewState_);
    if (selectedAsset_.empty()) {
        clear_media_preview();
    }
    request_asset_preview();
}

void EditorLayer::request_asset_preview() {
    request_asset_system_preview();
    if (selectedAsset_.empty()) {
        if (!assetPreviewState_.path.empty()) {
            assetPreviewState_ = {};
            uiModel_.set_asset_preview(assetPreviewState_);
        }
        return;
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (!indexed) {
        if (projectFiles_.empty() || !assetIndex_) {
            assetPreviewState_.path = selectedAsset_;
            assetPreviewState_.loading = true;
            assetPreviewState_.status = "Waiting for asset index...";
        } else {
            assetPreviewState_.path = selectedAsset_;
            assetPreviewState_.loading = false;
            assetPreviewState_.status = "Resource is no longer available";
            assetPreviewState_.imageWidth = 0;
            assetPreviewState_.imageHeight = 0;
            assetPreviewState_.imageSnapshot.reset();
            assetPreviewState_.modelPreview.reset();
            assetPreviewState_.modelTextureWidth = 0;
            assetPreviewState_.modelTextureHeight = 0;
            assetPreviewState_.modelTextureSnapshot.reset();
            assetPreviewState_.modelTextureStatus.clear();
            assetPreviewState_.modelMaterialIndex = -1;
            assetPreviewState_.modelTextureIndex = -1;
            assetPreviewState_.modelTextureImageIndex = -1;
            assetPreviewState_.modelMaterialLabel.clear();
            assetPreviewState_.modelTextureLabel.clear();
            assetPreviewState_.modelTextureRole.clear();
            if (mediaPanel_.description().resource.uri == selectedAsset_) {
                clear_media_preview();
            }
        }
        uiModel_.set_asset_preview(assetPreviewState_);
        sync_media_preview_state();
        return;
    }
    const auto& descriptor = indexed->descriptor;
    if (descriptor.kind != AssetPreviewKind::Text) {
        const auto mediaKind = descriptor.kind == AssetPreviewKind::Audio ? ui::MediaKind::Audio :
            descriptor.kind == AssetPreviewKind::Video ? ui::MediaKind::Video : ui::MediaKind::Image;
        const bool mediaResource = descriptor.kind == AssetPreviewKind::Audio ||
            descriptor.kind == AssetPreviewKind::Image || descriptor.kind == AssetPreviewKind::Video;
        const auto currentUri = mediaPanel_.description().resource.uri;
        const auto currentKind = mediaPanel_.description().kind;
        if (mediaResource && (currentUri != selectedAsset_ || currentKind != mediaKind)) {
            clear_media_preview();
            auto description = mediaPanel_.description();
            description.title = std::string(descriptor.previewTitle);
            description.kind = mediaKind;
            description.resource = {selectedAsset_, descriptor.mimeType, descriptor.displayName};
            description.showVolume = descriptor.kind == AssetPreviewKind::Audio;
            mediaPanel_.set_description(std::move(description));
            mediaPanel_.playback() = {};
            if (descriptor.kind == AssetPreviewKind::Audio || descriptor.kind == AssetPreviewKind::Video) {
                set_panel_visible("media", true);
                dockWorkspace_.activate_tab("media");
            }
        } else if (!mediaResource && !currentUri.empty()) {
            clear_media_preview();
        }
        const bool imageReady = descriptor.kind == AssetPreviewKind::Image &&
            assetPreviewState_.path == selectedAsset_ &&
            imagePreviewStamp_ == indexed->writeStamp && assetPreviewState_.imageSnapshot &&
            !assetPreviewState_.loading;
        if (!imageReady) {
            assetPreviewStamp_ = indexed->writeStamp;
            assetPreviewState_.path = selectedAsset_;
            assetPreviewState_.kind = std::string(AssetPreviewCatalog::kind_name(descriptor.kind));
            assetPreviewState_.title = descriptor.previewTitle;
            assetPreviewState_.status = descriptor.statusMessage;
            assetPreviewState_.loading = descriptor.kind == AssetPreviewKind::Image ||
                descriptor.kind == AssetPreviewKind::Model;
            assetPreviewState_.truncated = false;
            assetPreviewState_.textLines.clear();
            assetPreviewState_.imageWidth = 0;
            assetPreviewState_.imageHeight = 0;
            assetPreviewState_.imageSnapshot.reset();
            assetPreviewState_.modelPreview.reset();
            assetPreviewState_.modelTextureWidth = 0;
            assetPreviewState_.modelTextureHeight = 0;
            assetPreviewState_.modelTextureSnapshot.reset();
            assetPreviewState_.modelTextureStatus.clear();
            assetPreviewState_.modelMaterialIndex = -1;
            assetPreviewState_.modelTextureIndex = -1;
            assetPreviewState_.modelTextureImageIndex = -1;
            assetPreviewState_.modelMaterialLabel.clear();
            assetPreviewState_.modelTextureLabel.clear();
            assetPreviewState_.modelTextureRole.clear();
            uiModel_.set_asset_preview(assetPreviewState_);
            if (descriptor.kind == AssetPreviewKind::Image) request_image_preview(*indexed);
            if (descriptor.kind == AssetPreviewKind::Audio) request_audio_preview(*indexed);
            if (descriptor.kind == AssetPreviewKind::Video) request_video_preview(*indexed);
            if (descriptor.kind == AssetPreviewKind::Model) request_model_preview(*indexed);
        }
        sync_media_preview_state();
        return;
    }
    if (!mediaPanel_.description().resource.uri.empty()) {
        clear_media_preview();
    }
    sync_media_preview_state();
    if (assetPreviewState_.path == selectedAsset_ && assetPreviewStamp_ == indexed->writeStamp &&
        !assetPreviewState_.loading) return;
    if (assetPreviewFuture_.valid()) return;

    assetPreviewState_.path = selectedAsset_;
    assetPreviewState_.kind = "Text";
    assetPreviewState_.title = "Text Resource";
    assetPreviewState_.status = "Loading text preview...";
    assetPreviewState_.loading = true;
    assetPreviewState_.truncated = false;
    assetPreviewState_.textLines.clear();
    assetPreviewState_.imageWidth = 0;
    assetPreviewState_.imageHeight = 0;
    assetPreviewState_.imageSnapshot.reset();
    assetPreviewState_.modelPreview.reset();
    assetPreviewState_.modelTextureWidth = 0;
    assetPreviewState_.modelTextureHeight = 0;
    assetPreviewState_.modelTextureSnapshot.reset();
    assetPreviewState_.modelTextureStatus.clear();
    assetPreviewState_.modelMaterialIndex = -1;
    assetPreviewState_.modelTextureIndex = -1;
    assetPreviewState_.modelTextureImageIndex = -1;
    assetPreviewState_.modelMaterialLabel.clear();
    assetPreviewState_.modelTextureLabel.clear();
    assetPreviewState_.modelTextureRole.clear();
    uiModel_.set_asset_preview(assetPreviewState_);
    const auto generation = assetPreviewGeneration_;
    const auto sourceStamp = indexed->writeStamp;
    const auto path = selectedAsset_;
    auto scanner = fileSystem_;
    assetPreviewFuture_ = std::async(std::launch::async,
        [generation, sourceStamp, path, scanner = std::move(scanner)]() mutable {
            AsyncAssetPreview result;
            result.generation = generation;
            result.sourceStamp = sourceStamp;
            result.path = path;
            bool truncated = false;
            if (!scanner.read_text_limited(path, 64u * 1024u, result.content, &truncated, &result.error)) return result;
            result.truncated = truncated;
            return result;
        });
}

void EditorLayer::reset_asset_system_preview(std::string status) {
    ++assetSystemPreviewGeneration_;
    assetSystemPreviewFuture_ = {};
    assetSystemPreviewStamp_ = 0;
    assetSystemPreviewPath_.clear();
    assetSystemPreviewStatus_ = std::move(status);
    assetSystemPreviewFormat_.clear();
    assetSystemPreviewMetadataFormat_.clear();
    assetSystemPreviewMetadataBytes_ = 0;
    assetSystemPreviewSourceHash_ = 0;
    assetSystemPreviewLoading_ = false;
    assetSystemPreviewReady_ = false;
    publish_asset_system_preview_state();
}

void EditorLayer::publish_asset_system_preview_state() {
    assetPreviewState_.assetSystemStatus = assetSystemPreviewStatus_;
    assetPreviewState_.assetSystemFormat = assetSystemPreviewFormat_;
    assetPreviewState_.assetSystemMetadataFormat = assetSystemPreviewMetadataFormat_;
    assetPreviewState_.assetSystemMetadataBytes = assetSystemPreviewMetadataBytes_;
    assetPreviewState_.assetSystemSourceHash = assetSystemPreviewSourceHash_;
    assetPreviewState_.assetSystemLoading = assetSystemPreviewLoading_;
    assetPreviewState_.assetSystemReady = assetSystemPreviewReady_;
    uiModel_.set_asset_preview(assetPreviewState_);
}

void EditorLayer::request_asset_system_preview() {
    if (selectedAsset_.empty()) {
        if (!assetSystemPreviewPath_.empty() || assetSystemPreviewStatus_ != "AssetSystem not connected")
            reset_asset_system_preview("AssetSystem not connected");
        return;
    }
    if (!assetSystem_) {
        if (assetSystemPreviewStatus_ != "AssetSystem not connected" || assetSystemPreviewPath_ != selectedAsset_)
            reset_asset_system_preview("AssetSystem not connected");
        assetSystemPreviewPath_ = selectedAsset_;
        publish_asset_system_preview_state();
        return;
    }
    if (assetSystemRootNeedsRestart_) {
        if (assetSystemPreviewStatus_ != "AssetSystem root changed; restart editor to reconnect resources..." ||
            assetSystemPreviewPath_ != selectedAsset_)
            reset_asset_system_preview("AssetSystem root changed; restart editor to reconnect resources...");
        assetSystemPreviewPath_ = selectedAsset_;
        publish_asset_system_preview_state();
        return;
    }
    if (!assetSystem_->initialized()) {
        if (assetSystemPreviewStatus_ != "AssetSystem unavailable" || assetSystemPreviewPath_ != selectedAsset_)
            reset_asset_system_preview("AssetSystem unavailable");
        assetSystemPreviewPath_ = selectedAsset_;
        publish_asset_system_preview_state();
        return;
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (!indexed) {
        const auto status = projectFiles_.empty() || !assetIndex_
            ? "Waiting for AssetSystem resource index..."
            : "Resource is not indexed by AssetSystem";
        if (assetSystemPreviewStatus_ != status || assetSystemPreviewPath_ != selectedAsset_)
            reset_asset_system_preview(status);
        assetSystemPreviewPath_ = selectedAsset_;
        publish_asset_system_preview_state();
        return;
    }
    const auto sourceStamp = indexed->writeStamp;
    if (assetSystemPreviewPath_ == selectedAsset_ && assetSystemPreviewStamp_ == sourceStamp &&
        assetSystemPreviewFuture_.valid()) {
        return;
    }
    assetSystemPreviewPath_ = selectedAsset_;
    assetSystemPreviewStamp_ = sourceStamp;
    assetSystemPreviewStatus_ = "Loading resource through AssetSystem...";
    assetSystemPreviewFormat_.clear();
    assetSystemPreviewMetadataFormat_.clear();
    assetSystemPreviewMetadataBytes_ = 0;
    assetSystemPreviewSourceHash_ = 0;
    assetSystemPreviewLoading_ = true;
    assetSystemPreviewReady_ = false;
    publish_asset_system_preview_state();
    // An empty type asks AssetSystem to canonicalize from the extension. The
    // typed source artifact carries bounded source descriptors, while the
    // image/audio/video/model providers still own decoded preview snapshots.
    assetSystemPreviewFuture_ = assetSystem_->request({selectedAsset_, {}}, {false, false, 5});
}

void EditorLayer::poll_asset_system_preview() {
    if (!assetSystemPreviewFuture_.valid() || assetSystemPreviewLoading_ == false ||
        assetSystemPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const auto result = assetSystemPreviewFuture_.get();
    if (assetSystemRootNeedsRestart_) return;
    assetSystemPreviewLoading_ = false;
    if (result) {
        assetSystemPreviewReady_ = true;
        assetSystemPreviewStatus_ = "AssetSystem ready";
        assetSystemPreviewFormat_ = result.data->format;
        assetSystemPreviewMetadataFormat_ = result.data->metadataFormat;
        assetSystemPreviewMetadataBytes_ = result.data->metadata ? result.data->metadata->size() : 0;
        assetSystemPreviewSourceHash_ = result.data->sourceHash;
    } else {
        assetSystemPreviewReady_ = false;
        assetSystemPreviewFormat_.clear();
        assetSystemPreviewMetadataFormat_.clear();
        assetSystemPreviewMetadataBytes_ = 0;
        assetSystemPreviewSourceHash_ = 0;
        assetSystemPreviewStatus_ = result.error.empty() ?
            "AssetSystem failed to load resource" : "AssetSystem failed: " + result.error;
    }
    publish_asset_system_preview_state();
}

void EditorLayer::request_image_preview(const EditorAssetIndexEntry& indexed) {
    if (selectedAsset_.empty() || indexed.descriptor.kind != AssetPreviewKind::Image) return;
    if (assetPreviewState_.path == selectedAsset_ && imagePreviewStamp_ == indexed.writeStamp &&
        assetPreviewState_.imageSnapshot && !assetPreviewState_.loading) return;
    if (imagePreviewFuture_.valid()) {
        if (imagePreviewFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_image_preview();
        if (imagePreviewFuture_.valid()) {
            assetPreviewState_.loading = true;
            assetPreviewState_.status = "Loading image preview...";
            uiModel_.set_asset_preview(assetPreviewState_);
            return;
        }
        if (assetPreviewState_.path == selectedAsset_ && imagePreviewStamp_ == indexed.writeStamp &&
            assetPreviewState_.imageSnapshot && !assetPreviewState_.loading) return;
    }
    assetPreviewState_.path = selectedAsset_;
    assetPreviewState_.kind = "Image";
    assetPreviewState_.title = indexed.descriptor.previewTitle;
    assetPreviewState_.status = "Loading image preview...";
    assetPreviewState_.loading = true;
    assetPreviewState_.truncated = false;
    assetPreviewState_.textLines.clear();
    assetPreviewState_.imageWidth = 0;
    assetPreviewState_.imageHeight = 0;
    assetPreviewState_.imageSnapshot.reset();
    uiModel_.set_asset_preview(assetPreviewState_);
    const auto generation = imagePreviewGeneration_;
    const auto sourceStamp = indexed.writeStamp;
    const auto path = selectedAsset_;
    auto normalizedExtension = std::filesystem::path(path).extension().string();
    std::transform(normalizedExtension.begin(), normalizedExtension.end(), normalizedExtension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool useAssetSystemTexture = assetSystem_ && assetSystem_->initialized() &&
        !assetSystemRootNeedsRestart_ &&
        (normalizedExtension == ".png" || normalizedExtension == ".jpg" || normalizedExtension == ".jpeg" ||
         normalizedExtension == ".tga" || normalizedExtension == ".dds" || normalizedExtension == ".ktx" ||
         normalizedExtension == ".ktx2");
    if (useAssetSystemTexture) {
        auto sourceFuture = assetSystem_->request({path, "texture"}, {false, false, 6});
        imagePreviewFuture_ = std::async(std::launch::async,
            [generation, sourceStamp, path, sourceFuture]() mutable {
                const auto loaded = sourceFuture.get();
                if (!loaded || !loaded.data || !loaded.data->bytes) {
                    EditorImagePreviewResult result;
                    result.generation = generation;
                    result.sourceStamp = sourceStamp;
                    result.path = path;
                    result.error = loaded.error.empty() ?
                        "AssetSystem did not publish image source bytes" : loaded.error;
                    return result;
                }
                return load_editor_image_preview_bytes(path, *loaded.data->bytes,
                    generation, sourceStamp);
            });
    } else {
        auto scanner = fileSystem_;
        imagePreviewFuture_ = std::async(std::launch::async,
            [generation, sourceStamp, path, scanner = std::move(scanner)]() mutable {
                return load_editor_image_preview(scanner, path, generation, sourceStamp);
            });
    }
}

void EditorLayer::stop_audio_preview() {
    if (audioPreviewVoice_ != 0 && audioSystem_) audioSystem_->stop(audioPreviewVoice_);
    // Path-only preview loads are editor-owned temporary clips. Manifest
    // backed clips stay cached for the current editor session and are
    // released by clear_audio_asset_bindings or an AssetSystem reconnect.
    if (audioPreviewAsset_ != 0 && audioPreviewAssetId_ == 0 && audioSystem_ &&
        audioSystem_->is_loaded(audioPreviewAsset_)) {
        audioSystem_->unload(audioPreviewAsset_);
    }
    audioPreviewVoice_ = 0;
    audioPreviewAsset_ = 0;
    audioPreviewAssetId_ = 0;
    audioPreviewPath_.clear();
    auto& playback = mediaPanel_.playback();
    playback.state = ui::MediaPlaybackState::Stopped;
    playback.currentTime = 0.0;
}

void EditorLayer::clear_audio_asset_bindings() {
    stop_audio_preview();
    if (audioSystem_) {
        for (const auto& [assetId, audioAsset] : audioAssetBindings_) {
            (void)assetId;
            if (audioAsset != 0 && audioSystem_->is_loaded(audioAsset)) audioSystem_->unload(audioAsset);
        }
    }
    audioAssetBindings_.clear();
    audioPreviewAsset_ = 0;
    audioPreviewAssetId_ = 0;
}

void EditorLayer::clear_media_preview() {
    stop_audio_preview();
    auto description = mediaPanel_.description();
    description.resource = {};
    description.title = "Media Preview";
    description.kind = ui::MediaKind::Image;
    description.showVolume = false;
    mediaPanel_.set_description(std::move(description));
    mediaPanel_.playback() = {};
    sync_media_preview_state();
}

void EditorLayer::start_audio_preview() {
    if (mediaPanel_.description().kind != ui::MediaKind::Audio || selectedAsset_.empty()) return;
    if (!audioSystem_ || !audioSystem_->initialized()) {
        mediaPanel_.playback().state = ui::MediaPlaybackState::Stopped;
        lastStatus_ = "Audio preview unavailable: AudioSystem is not connected";
        sync_media_preview_state();
        return;
    }
    const auto path = fileSystem_.resolve_existing(selectedAsset_);
    if (path.empty()) {
        mediaPanel_.playback().state = ui::MediaPlaybackState::Stopped;
        lastStatus_ = "Audio preview failed: resource is outside the project or unavailable";
        sync_media_preview_state();
        return;
    }
    if (audioPreviewVoice_ != 0) {
        const auto state = audioSystem_->state(audioPreviewVoice_);
        if (state == audio::AudioVoiceState::Paused) {
            audioSystem_->resume(audioPreviewVoice_);
            mediaPanel_.playback().state = ui::MediaPlaybackState::Playing;
            sync_media_preview_state();
            return;
        }
        if (state == audio::AudioVoiceState::Playing) return;
        stop_audio_preview();
    }
    assets::AssetId manifestAssetId = 0;
    if (assetManifest_) {
        for (const auto& entry : *assetManifest_) {
            if (entry.id != 0 && entry.key.type == "audio" && entry.key.uri == selectedAsset_) {
                manifestAssetId = entry.id;
                break;
            }
        }
    }
    audio::AudioAssetId audioAsset = 0;
    if (manifestAssetId != 0) {
        const auto cached = audioAssetBindings_.find(manifestAssetId);
        if (cached != audioAssetBindings_.end() && audioSystem_->is_loaded(cached->second)) {
            audioAsset = cached->second;
        } else {
            audioAsset = audioSystem_->load(path, true);
            if (audioAsset != 0) audioAssetBindings_[manifestAssetId] = audioAsset;
        }
    } else {
        audioAsset = audioSystem_->load(path, true);
    }
    if (audioAsset == 0) {
        mediaPanel_.playback().state = ui::MediaPlaybackState::Stopped;
        lastStatus_ = "Audio preview failed: " + audioSystem_->last_error();
        sync_media_preview_state();
        return;
    }
    audio::AudioPlayParams params;
    params.bus = audio::AudioBus::UI;
    params.loop = mediaPanel_.playback().loop;
    params.streaming = true;
    params.volume = static_cast<float>(std::clamp(mediaPanel_.playback().volume, 0.0, 1.0));
    audioPreviewVoice_ = audioSystem_->play(audioAsset, params);
    if (audioPreviewVoice_ == 0) {
        if (manifestAssetId == 0 && audioSystem_->is_loaded(audioAsset)) audioSystem_->unload(audioAsset);
        mediaPanel_.playback().state = ui::MediaPlaybackState::Stopped;
        lastStatus_ = "Audio preview failed: " + audioSystem_->last_error();
        sync_media_preview_state();
        return;
    }
    audioPreviewAsset_ = audioAsset;
    audioPreviewAssetId_ = manifestAssetId;
    audioPreviewPath_ = selectedAsset_;
    mediaPanel_.playback().state = ui::MediaPlaybackState::Playing;
    lastStatus_ = "Playing " + selectedAsset_;
    sync_media_preview_state();
}

void EditorLayer::sync_media_preview_state() {
    EditorMediaUiState state;
    const auto& description = mediaPanel_.description();
    const auto& playback = mediaPanel_.playback();
    if ((description.kind == ui::MediaKind::Audio || description.kind == ui::MediaKind::Video) &&
        !description.resource.uri.empty()) {
        state.path = description.resource.uri;
        state.kind = description.kind == ui::MediaKind::Audio ? "Audio" : "Video";
        state.volume = std::clamp(playback.volume, 0.0, 1.0);
        state.loop = playback.loop;
        state.duration = std::max(0.0, playback.duration);
        state.currentTime = std::clamp(playback.currentTime, 0.0, state.duration > 0.0 ? state.duration : playback.currentTime);
        if (description.kind == ui::MediaKind::Audio) {
            state.previewLoading = audioPreviewFuture_.valid() &&
                audioPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
            state.previewStatus = audioPreviewStatus_;
            state.audioPreview = audioPreviewSnapshot_;
            if (audioPreviewSnapshot_ && audioPreviewSnapshot_->valid()) {
                state.duration = audioPreviewSnapshot_->duration;
                state.currentTime = std::clamp(playback.currentTime, 0.0, state.duration);
            }
            // A non-empty resource URI is published only from an indexed file
            // entry. Avoid a canonical filesystem query on every editor frame;
            // scan publication handles removal and invalidation separately.
            state.available = audioSystem_ && audioSystem_->initialized();
            state.status = state.available ? "AudioSystem connected" :
                audioSystem_ ? "AudioSystem unavailable or resource missing" : "AudioSystem is not connected";
        } else {
            state.previewLoading = (videoPreviewFuture_.valid() &&
                videoPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) ||
                (videoFrameFuture_.valid() &&
                 videoFrameFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
            state.previewStatus = videoPreviewStatus_;
            state.videoPreview = videoPreviewSnapshot_;
            if (videoPreviewSnapshot_ && videoPreviewSnapshot_->valid()) {
                state.duration = videoPreviewSnapshot_->duration;
                state.currentTime = std::clamp(playback.currentTime, 0.0, state.duration);
            }
            state.available = videoPreviewSnapshot_ && videoPreviewSnapshot_->valid();
            state.status = state.available ? "Video preview ready" :
                state.previewLoading ? "Video preview loading" : "Video preview unavailable";
        }
        state.playbackState = ui::media_playback_state_name(playback.state);
        if (description.kind == ui::MediaKind::Audio && audioPreviewVoice_ != 0 && audioSystem_) {
            const auto cursor = audioSystem_->cursor_seconds(audioPreviewVoice_);
            if (audioSystem_->supports_cursor() && std::isfinite(cursor)) {
                mediaPanel_.playback().currentTime = std::clamp(cursor, 0.0,
                    audioPreviewSnapshot_ && audioPreviewSnapshot_->valid() ? audioPreviewSnapshot_->duration : cursor);
                state.currentTime = mediaPanel_.playback().currentTime;
            }
            const auto voiceState = audioSystem_->state(audioPreviewVoice_);
            if (voiceState == audio::AudioVoiceState::Playing) {
                state.playbackState = "playing";
            } else if (voiceState == audio::AudioVoiceState::Paused) {
                state.playbackState = "paused";
            } else if (voiceState == audio::AudioVoiceState::Stopped ||
                       voiceState == audio::AudioVoiceState::Finished ||
                       voiceState == audio::AudioVoiceState::Invalid) {
                audioPreviewVoice_ = 0;
                audioPreviewPath_.clear();
                mediaPanel_.playback().state = ui::MediaPlaybackState::Stopped;
                state.playbackState = "stopped";
            }
        }
    }
    uiModel_.set_media_state(std::move(state));
}

void EditorLayer::poll_asset_preview() {
    if (!assetPreviewFuture_.valid() ||
        assetPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    auto result = assetPreviewFuture_.get();
    if (result.generation != assetPreviewGeneration_ || result.path != selectedAsset_) {
        request_asset_preview();
        return;
    }
    assetPreviewState_.path = result.path;
    assetPreviewState_.kind = "Text";
    assetPreviewState_.title = "Text Resource";
    assetPreviewState_.loading = false;
    assetPreviewState_.textLines.clear();
    assetPreviewStamp_ = result.sourceStamp;
    if (!result.error.empty()) {
        assetPreviewState_.status = "Preview failed: " + result.error;
        assetPreviewState_.truncated = false;
    } else {
        std::size_t cursor = 0;
        while (cursor <= result.content.size() && assetPreviewState_.textLines.size() < 24) {
            const auto end = result.content.find('\n', cursor);
            std::string line = result.content.substr(cursor,
                end == std::string::npos ? result.content.size() - cursor : end - cursor);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            assetPreviewState_.textLines.push_back(std::move(line));
            if (end == std::string::npos) break;
            cursor = end + 1;
        }
        const bool lineLimit = cursor < result.content.size();
        assetPreviewState_.truncated = result.truncated || lineLimit;
        assetPreviewState_.status = assetPreviewState_.truncated ? "Text preview (truncated)" : "Text preview";
    }
    uiModel_.set_asset_preview(assetPreviewState_);
    request_asset_preview();
}

void EditorLayer::poll_image_preview() {
    if (!imagePreviewFuture_.valid() ||
        imagePreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorImagePreviewResult result;
    try { result = imagePreviewFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = imagePreviewGeneration_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = imagePreviewGeneration_;
        result.error = "unknown image preview worker failure";
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (result.generation != imagePreviewGeneration_ || result.path != selectedAsset_ ||
        !indexed || indexed->descriptor.kind != AssetPreviewKind::Image) {
        request_asset_preview();
        return;
    }
    assetPreviewState_.path = result.path;
    assetPreviewState_.kind = "Image";
    assetPreviewState_.title = indexed->descriptor.previewTitle;
    assetPreviewState_.loading = false;
    assetPreviewState_.truncated = false;
    assetPreviewState_.textLines.clear();
    imagePreviewStamp_ = result.sourceStamp;
    assetPreviewState_.imageWidth = result.sourceWidth;
    assetPreviewState_.imageHeight = result.sourceHeight;
    assetPreviewState_.imageSnapshot = std::move(result.snapshot);
    if (!result.error.empty()) {
        assetPreviewState_.status = "Preview unavailable: " + result.error;
        assetPreviewState_.imageWidth = 0;
        assetPreviewState_.imageHeight = 0;
    } else {
        assetPreviewState_.status = "Image preview";
    }
    uiModel_.set_asset_preview(assetPreviewState_);
}

void EditorLayer::request_audio_preview(const EditorAssetIndexEntry& indexed) {
    if (selectedAsset_.empty() || indexed.descriptor.kind != AssetPreviewKind::Audio) return;
    if (audioPreviewSnapshot_ && audioPreviewStamp_ == indexed.writeStamp) {
        audioPreviewStatus_ = "Waveform ready";
        return;
    }
    if (audioPreviewFuture_.valid()) {
        if (audioPreviewFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_audio_preview();
        if (audioPreviewFuture_.valid()) return;
        if (audioPreviewSnapshot_ && audioPreviewStamp_ == indexed.writeStamp) return;
    }
    audioPreviewSnapshot_.reset();
    audioPreviewStamp_ = 0;
    audioPreviewStatus_ = "Loading waveform...";
    mediaPanel_.playback().duration = 0.0;
    mediaPanel_.playback().currentTime = 0.0;
    sync_media_preview_state();
    const auto generation = audioPreviewGeneration_;
    const auto sourceStamp = indexed.writeStamp;
    const auto path = selectedAsset_;
    audioPreviewCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = audioPreviewCancel_;
    auto scanner = fileSystem_;
    audioPreviewFuture_ = std::async(std::launch::async,
        [generation, sourceStamp, path, cancel, scanner = std::move(scanner)]() mutable {
            return load_editor_audio_preview(scanner, path, generation, sourceStamp, 256, cancel.get());
        });
}

void EditorLayer::poll_audio_preview() {
    if (!audioPreviewFuture_.valid() ||
        audioPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorAudioPreviewResult result;
    try { result = audioPreviewFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = audioPreviewGeneration_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = audioPreviewGeneration_;
        result.error = "unknown audio preview worker failure";
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (result.generation != audioPreviewGeneration_ || result.path != selectedAsset_ ||
        !indexed || indexed->descriptor.kind != AssetPreviewKind::Audio) {
        request_asset_preview();
        return;
    }
    audioPreviewStamp_ = result.sourceStamp;
    audioPreviewSnapshot_ = std::move(result.snapshot);
    if (audioPreviewSnapshot_ && audioPreviewSnapshot_->valid()) {
        mediaPanel_.playback().duration = audioPreviewSnapshot_->duration;
        mediaPanel_.playback().currentTime = std::clamp(mediaPanel_.playback().currentTime,
            0.0, audioPreviewSnapshot_->duration);
        audioPreviewStatus_ = "Waveform ready";
    } else {
        mediaPanel_.playback().duration = 0.0;
        mediaPanel_.playback().currentTime = 0.0;
        audioPreviewStatus_ = result.error.empty() ?
            "Preview unavailable: decoder returned no waveform" : "Preview unavailable: " + result.error;
    }
    sync_media_preview_state();
}

void EditorLayer::request_video_preview(const EditorAssetIndexEntry& indexed) {
    if (selectedAsset_.empty() || indexed.descriptor.kind != AssetPreviewKind::Video) return;
    if (videoPreviewSnapshot_ && videoPreviewStamp_ == indexed.writeStamp) {
        videoPreviewStatus_ = "Video preview ready";
        return;
    }
    if (videoPreviewFuture_.valid()) {
        if (videoPreviewFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_video_preview();
        if (videoPreviewFuture_.valid()) return;
        if (videoPreviewSnapshot_ && videoPreviewStamp_ == indexed.writeStamp) return;
    }
    videoPreviewSnapshot_.reset();
    videoPreviewStamp_ = 0;
    videoPreviewStatus_ = "Loading video preview...";
    mediaPanel_.playback().duration = 0.0;
    mediaPanel_.playback().currentTime = 0.0;
    sync_media_preview_state();
    const auto generation = videoPreviewGeneration_;
    const auto sourceStamp = indexed.writeStamp;
    const auto path = selectedAsset_;
    videoPreviewCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = videoPreviewCancel_;
    auto scanner = fileSystem_;
    videoPreviewFuture_ = std::async(std::launch::async,
        [generation, sourceStamp, path, cancel, scanner = std::move(scanner)]() mutable {
            return load_editor_video_preview(scanner, path, generation, sourceStamp, 512, cancel.get());
        });
}

void EditorLayer::poll_video_preview() {
    if (!videoPreviewFuture_.valid() ||
        videoPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorVideoPreviewResult result;
    try { result = videoPreviewFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = videoPreviewGeneration_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = videoPreviewGeneration_;
        result.error = "unknown video preview worker failure";
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (result.generation != videoPreviewGeneration_ || result.path != selectedAsset_ ||
        !indexed || indexed->descriptor.kind != AssetPreviewKind::Video) {
        request_asset_preview();
        return;
    }
    videoPreviewStamp_ = result.sourceStamp;
    videoPreviewSnapshot_ = std::move(result.snapshot);
    if (videoPreviewSnapshot_ && videoPreviewSnapshot_->valid()) {
        mediaPanel_.playback().duration = videoPreviewSnapshot_->duration;
        mediaPanel_.playback().currentTime = std::clamp(mediaPanel_.playback().currentTime,
            0.0, videoPreviewSnapshot_->duration);
        videoPreviewStatus_ = "Video preview ready";
    } else {
        mediaPanel_.playback().duration = 0.0;
        mediaPanel_.playback().currentTime = 0.0;
        videoPreviewStatus_ = result.error.empty() ?
            "Preview unavailable: decoder returned no first frame" : "Preview unavailable: " + result.error;
    }
    sync_media_preview_state();
}

void EditorLayer::request_video_frame(double seconds) {
    if (selectedAsset_.empty() || !videoPreviewSnapshot_ || !videoPreviewSnapshot_->valid() ||
        videoPreviewStamp_ == 0) return;
    const auto clamped = std::clamp(std::isfinite(seconds) ? seconds : 0.0,
                                    0.0, videoPreviewSnapshot_->duration);
    if (videoFrameFuture_.valid()) {
        if (videoFrameFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_video_frame();
        if (videoFrameFuture_.valid()) {
            pendingVideoSeekSeconds_ = clamped;
            if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
            videoPreviewStatus_ = "Seeking video frame...";
            sync_media_preview_state();
            return;
        }
    }
    pendingVideoSeekSeconds_ = -1.0;
    if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
    videoFrameCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = videoFrameCancel_;
    const auto generation = videoPreviewGeneration_;
    const auto sourceStamp = videoPreviewStamp_;
    const auto path = selectedAsset_;
    auto scanner = fileSystem_;
    videoPreviewStatus_ = "Seeking video frame...";
    sync_media_preview_state();
    videoFrameFuture_ = std::async(std::launch::async,
        [generation, sourceStamp, path, clamped, cancel, scanner = std::move(scanner)]() mutable {
            return load_editor_video_frame(scanner, path, generation, sourceStamp, clamped, 512, cancel.get());
        });
}

void EditorLayer::poll_video_frame() {
    if (!videoFrameFuture_.valid() ||
        videoFrameFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorVideoPreviewResult result;
    try { result = videoFrameFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = videoPreviewGeneration_;
        result.sourceStamp = videoPreviewStamp_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = videoPreviewGeneration_;
        result.sourceStamp = videoPreviewStamp_;
        result.error = "unknown video seek worker failure";
    }
    const auto nextSeek = pendingVideoSeekSeconds_;
    pendingVideoSeekSeconds_ = -1.0;
    if (result.generation != videoPreviewGeneration_ || result.path != selectedAsset_ ||
        result.sourceStamp != videoPreviewStamp_) {
        if (nextSeek >= 0.0) request_video_frame(nextSeek);
        return;
    }
    if (result.snapshot && result.snapshot->valid()) {
        videoPreviewSnapshot_ = std::move(result.snapshot);
        videoPreviewStatus_ = "Video frame ready";
        mediaPanel_.playback().currentTime = std::clamp(videoPreviewSnapshot_->frameTime,
            0.0, videoPreviewSnapshot_->duration);
    } else {
        videoPreviewStatus_ = result.error.empty() ?
            "Video seek unavailable" : "Video seek unavailable: " + result.error;
    }
    sync_media_preview_state();
    if (nextSeek >= 0.0) request_video_frame(nextSeek);
}

void EditorLayer::request_model_preview(const EditorAssetIndexEntry& indexed) {
    if (selectedAsset_.empty() || indexed.descriptor.kind != AssetPreviewKind::Model) return;
    if (modelPreviewSnapshot_ && modelPreviewStamp_ == indexed.writeStamp) {
        modelPreviewStatus_ = "Model preview ready";
        return;
    }
    if (modelPreviewFuture_.valid()) {
        if (modelPreviewFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_model_preview();
        if (modelPreviewFuture_.valid()) return;
        if (modelPreviewSnapshot_ && modelPreviewStamp_ == indexed.writeStamp) return;
    }
    modelPreviewSnapshot_.reset();
    modelPreviewScene_.clear();
    modelPreviewStamp_ = 0;
    modelPreviewStatus_ = "Loading model preview...";
    assetPreviewState_.loading = true;
    assetPreviewState_.status = modelPreviewStatus_;
    uiModel_.set_asset_preview(assetPreviewState_);
    const auto generation = modelPreviewGeneration_;
    const auto sourceStamp = indexed.writeStamp;
    const auto path = selectedAsset_;
    modelPreviewCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = modelPreviewCancel_;
    const auto extension = std::filesystem::path(path).extension().string();
    auto normalizedExtension = extension;
    std::transform(normalizedExtension.begin(), normalizedExtension.end(), normalizedExtension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool useAssetSystemModel = (normalizedExtension == ".obj" ||
        normalizedExtension == ".gltf" || normalizedExtension == ".glb") && assetSystem_ &&
        assetSystem_->initialized() && !assetSystemRootNeedsRestart_;
    if (useAssetSystemModel) {
        auto sourceFuture = assetSystem_->request({path, "model"}, {false, false, 6});
        auto scanner = fileSystem_;
        modelPreviewFuture_ = std::async(std::launch::async,
            [generation, sourceStamp, path, cancel, sourceFuture, scanner = std::move(scanner), normalizedExtension]() mutable {
                const auto loaded = sourceFuture.get();
                if (!loaded || !loaded.data || !loaded.data->bytes) {
                    EditorModelPreviewResult result;
                    result.generation = generation;
                    result.sourceStamp = sourceStamp;
                    result.path = path;
                    result.error = loaded.error.empty() ?
                        "AssetSystem did not publish model source bytes" : loaded.error;
                    return result;
                }
                if (normalizedExtension == ".gltf" || normalizedExtension == ".glb")
                    return load_editor_gltf_preview_bytes(scanner, path, *loaded.data->bytes,
                        generation, sourceStamp, cancel.get());
                return load_editor_obj_preview_bytes(path, *loaded.data->bytes,
                    generation, sourceStamp, cancel.get());
            });
    } else {
        auto scanner = fileSystem_;
        modelPreviewFuture_ = std::async(std::launch::async,
            [generation, sourceStamp, path, cancel, scanner = std::move(scanner)]() mutable {
                return load_editor_model_preview(scanner, path, generation, sourceStamp, cancel.get());
            });
    }
}

void EditorLayer::poll_model_preview() {
    if (!modelPreviewFuture_.valid() ||
        modelPreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorModelPreviewResult result;
    try { result = modelPreviewFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = modelPreviewGeneration_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = modelPreviewGeneration_;
        result.error = "unknown model preview worker failure";
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(selectedAsset_) : nullptr;
    if (result.generation != modelPreviewGeneration_ || result.path != selectedAsset_ ||
        !indexed || indexed->descriptor.kind != AssetPreviewKind::Model) {
        request_asset_preview();
        return;
    }
    modelPreviewStamp_ = result.sourceStamp;
    modelPreviewSnapshot_ = std::move(result.snapshot);
    modelPreviewScene_.set_snapshot(modelPreviewSnapshot_);
    assetPreviewState_.loading = false;
    assetPreviewState_.modelPreview = modelPreviewSnapshot_;
    assetPreviewState_.modelPreviewScene = modelPreviewScene_.state();
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    modelTexturePreviewPath_ = selectedAsset_;
    assetPreviewState_.modelTextureWidth = 0;
    assetPreviewState_.modelTextureHeight = 0;
    assetPreviewState_.modelTextureSnapshot.reset();
    assetPreviewState_.modelTextureStatus.clear();
    modelMaterialSelection_ = -1;
    modelTextureSelection_ = -1;
    if (modelPreviewSnapshot_ && modelPreviewSnapshot_->valid()) {
        modelPreviewStatus_ = "Model preview ready";
        assetPreviewState_.status = modelPreviewStatus_;
    } else {
        modelPreviewStatus_ = result.error.empty() ?
            "Preview unavailable: parser returned no geometry" : "Preview unavailable: " + result.error;
        assetPreviewState_.status = modelPreviewStatus_;
        assetPreviewState_.modelPreviewScene.reset();
    }
    uiModel_.set_asset_preview(assetPreviewState_);
    sync_model_preview_selection();
}

void EditorLayer::reset_model_scene_assets() {
    ++modelSceneGeneration_;
    for (auto& [assetId, record] : modelSceneAssets_) {
        (void)assetId;
        record.active = false;
        if (record.cancel) record.cancel->store(true, std::memory_order_relaxed);
        if (record.future.valid()) {
            record.future.wait();
            try { record.future.get(); } catch (...) { }
        }
    }
    modelSceneAssets_.clear();
    modelSceneRenderState_ = {};
}

void EditorLayer::poll_model_scene_assets() {
    for (auto& [assetId, record] : modelSceneAssets_) {
        (void)assetId;
        if (!record.future.valid() ||
            record.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
        EditorModelPreviewResult result;
        try { result = record.future.get(); }
        catch (const std::exception& error) { result.error = error.what(); }
        catch (...) { result.error = "unknown model scene worker failure"; }
        record.cancel.reset();
        if (result.generation != record.generation || result.path != record.path) {
            record.status = "Model scene asset load superseded";
            record.snapshot.reset();
            continue;
        }
        record.snapshot = std::move(result.snapshot);
        record.status = record.snapshot && record.snapshot->valid()
            ? "Model scene asset ready" : (result.error.empty()
                ? "Model scene asset is invalid" : "Model scene asset failed: " + result.error);
    }
}

void EditorLayer::sync_model_scene_assets(const World& world) {
    for (auto& [assetId, record] : modelSceneAssets_) {
        (void)assetId;
        record.active = false;
    }
    if (!assetSystem_ || !assetSystem_->initialized() || assetSystemRootNeedsRestart_ || !assetManifest_) {
        return;
    }

    const auto manifest_for = [this](assets::AssetId id) -> const assets::AssetManifestEntry* {
        if (!assetManifest_ || id == 0) return nullptr;
        for (const auto& entry : *assetManifest_)
            if (entry.id == id && entry.key.type == "model") return &entry;
        return nullptr;
    };
    const auto source_stamp_for = [](const assets::AssetManifestEntry& entry) noexcept {
        if (entry.sourceHash != 0) return entry.sourceHash;
        return entry.sourceTimestamp ^ (static_cast<std::uint64_t>(entry.sourceSize) +
            0x9e3779b97f4a7c15ull + (entry.sourceTimestamp << 6u) + (entry.sourceTimestamp >> 2u));
    };
    const auto normalize_extension = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    };

    world.each_object([&](const GameObject& object) {
        const auto* reference = object.get_component<AssetReferenceComponent>();
        if (!reference || reference->asset_id() == 0 || reference->path().empty()) return;
        const auto* manifest = manifest_for(reference->asset_id());
        if (!manifest) return;
        const auto path = reference->path();
        const auto sourceStamp = source_stamp_for(*manifest);
        auto recordIt = modelSceneAssets_.find(reference->asset_id());
        if (recordIt == modelSceneAssets_.end()) {
            recordIt = modelSceneAssets_.emplace(reference->asset_id(), AsyncModelSceneAsset{}).first;
        }
        auto& record = recordIt->second;
        record.active = true;
        const bool sourceChanged = record.path != path || record.sourceStamp != sourceStamp;
        if (sourceChanged) {
            if (record.future.valid()) {
                if (record.cancel) record.cancel->store(true, std::memory_order_relaxed);
                record.status = "Reloading model scene asset...";
                return;
            }
            ++record.generation;
            record.path = path;
            record.sourceStamp = sourceStamp;
            record.snapshot.reset();
            record.status = "Loading model scene asset...";
        }
        if (record.snapshot && record.sourceStamp == sourceStamp) return;
        if (record.future.valid()) return;

        const auto generation = record.generation;
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        record.cancel = cancel;
        const auto extension = normalize_extension(std::filesystem::path(path).extension().string());
        auto sourceFuture = assetSystem_->request({path, "model"}, {false, false, 5});
        auto scanner = fileSystem_;
        record.future = std::async(std::launch::async,
            [generation, sourceStamp, path, cancel, sourceFuture,
             scanner = std::move(scanner), extension]() mutable {
                const auto loaded = sourceFuture.get();
                if (!loaded || !loaded.data || !loaded.data->bytes) {
                    EditorModelPreviewResult result;
                    result.generation = generation;
                    result.sourceStamp = sourceStamp;
                    result.path = path;
                    result.error = loaded.error.empty() ?
                        "AssetSystem did not publish model source bytes" : loaded.error;
                    return result;
                }
                if (extension == ".gltf" || extension == ".glb")
                    return load_editor_gltf_preview_bytes(scanner, path, *loaded.data->bytes,
                        generation, sourceStamp, cancel.get());
                return load_editor_obj_preview_bytes(path, *loaded.data->bytes,
                    generation, sourceStamp, cancel.get());
            });
    });

    // A completed inactive request can now be reclaimed. Running requests are
    // retained until a later poll so their futures are always drained safely.
    for (auto it = modelSceneAssets_.begin(); it != modelSceneAssets_.end();) {
        if (it->second.active || it->second.future.valid()) { ++it; continue; }
        it = modelSceneAssets_.erase(it);
    }
}

void EditorLayer::sync_model_preview_selection() {
    if (!modelPreviewSnapshot_ || !modelPreviewSnapshot_->valid()) {
        modelMaterialSelection_ = -1;
        modelTextureSelection_ = -1;
        assetPreviewState_.modelMaterialIndex = -1;
        assetPreviewState_.modelTextureIndex = -1;
        assetPreviewState_.modelTextureImageIndex = -1;
        assetPreviewState_.modelMaterialLabel.clear();
        assetPreviewState_.modelTextureLabel.clear();
        assetPreviewState_.modelTextureRole.clear();
        uiModel_.set_asset_preview(assetPreviewState_);
        return;
    }
    const auto& snapshot = *modelPreviewSnapshot_;
    const auto& materials = snapshot.materials;
    const auto& textures = snapshot.textures;
    if (materials && !materials->empty()) {
        modelMaterialSelection_ = std::clamp<std::int32_t>(
            modelMaterialSelection_, 0, static_cast<std::int32_t>(materials->size() - 1u));
        const auto& material = (*materials)[static_cast<std::size_t>(modelMaterialSelection_)];
        assetPreviewState_.modelMaterialIndex = modelMaterialSelection_;
        assetPreviewState_.modelMaterialLabel = "Material " +
            std::to_string(static_cast<std::size_t>(modelMaterialSelection_) + 1u) + "/" +
            std::to_string(materials->size()) + ": " +
            (material.name.empty() ? std::string("Unnamed") : material.name);
    } else {
        modelMaterialSelection_ = -1;
        assetPreviewState_.modelMaterialIndex = -1;
        assetPreviewState_.modelMaterialLabel = "Material: none";
    }

    if (textures && !textures->empty()) {
        if (modelTextureSelection_ < 0 ||
            static_cast<std::size_t>(modelTextureSelection_) >= textures->size()) {
            std::int32_t preferred = -1;
            if (materials && modelMaterialSelection_ >= 0) {
                const auto& material = (*materials)[static_cast<std::size_t>(modelMaterialSelection_)];
                preferred = material.baseColorTexture >= 0 ? material.baseColorTexture :
                    material.normalTexture >= 0 ? material.normalTexture : material.metallicRoughnessTexture;
            }
            modelTextureSelection_ = preferred >= 0 &&
                static_cast<std::size_t>(preferred) < textures->size() ? preferred : 0;
        }
        const auto& texture = (*textures)[static_cast<std::size_t>(modelTextureSelection_)];
        assetPreviewState_.modelTextureIndex = modelTextureSelection_;
        assetPreviewState_.modelTextureLabel = "Texture " +
            std::to_string(static_cast<std::size_t>(modelTextureSelection_) + 1u) + "/" +
            std::to_string(textures->size()) + ": " +
            (texture.name.empty() ? std::string("Unnamed") : texture.name);
        assetPreviewState_.modelTextureImageIndex = texture.source;
        assetPreviewState_.modelTextureRole = "Texture";
        if (materials && modelMaterialSelection_ >= 0) {
            const auto& material = (*materials)[static_cast<std::size_t>(modelMaterialSelection_)];
            if (material.baseColorTexture == modelTextureSelection_) assetPreviewState_.modelTextureRole = "Base Color";
            else if (material.normalTexture == modelTextureSelection_) assetPreviewState_.modelTextureRole = "Normal";
            else if (material.metallicRoughnessTexture == modelTextureSelection_)
                assetPreviewState_.modelTextureRole = "Metallic/Roughness";
        }
    } else {
        modelTextureSelection_ = -1;
        assetPreviewState_.modelTextureIndex = -1;
        assetPreviewState_.modelTextureImageIndex = -1;
        assetPreviewState_.modelTextureLabel = "Texture: none";
        assetPreviewState_.modelTextureRole.clear();
    }
    uiModel_.set_asset_preview(assetPreviewState_);
    request_model_texture_preview();
}

void EditorLayer::select_model_material(std::int32_t delta) {
    if (!modelPreviewSnapshot_ || !modelPreviewSnapshot_->valid() ||
        !modelPreviewSnapshot_->materials || modelPreviewSnapshot_->materials->empty()) return;
    const auto count = static_cast<std::int32_t>(modelPreviewSnapshot_->materials->size());
    if (modelMaterialSelection_ < 0) modelMaterialSelection_ = 0;
    modelMaterialSelection_ = (modelMaterialSelection_ + delta) % count;
    if (modelMaterialSelection_ < 0) modelMaterialSelection_ += count;
    modelTextureSelection_ = -1;
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    assetPreviewState_.modelTextureWidth = 0;
    assetPreviewState_.modelTextureHeight = 0;
    assetPreviewState_.modelTextureSnapshot.reset();
    assetPreviewState_.modelTextureStatus.clear();
    sync_model_preview_selection();
}

void EditorLayer::select_model_texture(std::int32_t delta) {
    if (!modelPreviewSnapshot_ || !modelPreviewSnapshot_->valid() ||
        !modelPreviewSnapshot_->textures || modelPreviewSnapshot_->textures->empty()) return;
    const auto count = static_cast<std::int32_t>(modelPreviewSnapshot_->textures->size());
    if (modelTextureSelection_ < 0) modelTextureSelection_ = 0;
    modelTextureSelection_ = (modelTextureSelection_ + delta) % count;
    if (modelTextureSelection_ < 0) modelTextureSelection_ += count;
    ++modelTexturePreviewGeneration_;
    modelTexturePreviewStamp_ = 0;
    assetPreviewState_.modelTextureWidth = 0;
    assetPreviewState_.modelTextureHeight = 0;
    assetPreviewState_.modelTextureSnapshot.reset();
    assetPreviewState_.modelTextureStatus.clear();
    sync_model_preview_selection();
}

void EditorLayer::request_model_texture_preview() {
    if (selectedAsset_.empty() || !modelPreviewSnapshot_ ||
        !modelPreviewSnapshot_->valid()) return;
    const auto* artifacts = modelPreviewSnapshot_->imageArtifacts.get();
    if (!artifacts || artifacts->empty()) {
        assetPreviewState_.modelTextureStatus = modelPreviewSnapshot_->imageCount == 0
            ? "No glTF image artifacts"
            : "No image payload available";
        uiModel_.set_asset_preview(assetPreviewState_);
        return;
    }
    const EditorModelTextureArtifact* selected = nullptr;
    for (const auto& artifact : *artifacts) {
        if (artifact.valid() && artifact.imageIndex == assetPreviewState_.modelTextureImageIndex) {
            selected = &artifact;
            break;
        }
    }
    if (!selected) {
        assetPreviewState_.modelTextureStatus = assetPreviewState_.modelTextureImageIndex < 0
            ? "Texture has no image source" : "Selected texture image payload unavailable";
        uiModel_.set_asset_preview(assetPreviewState_);
        return;
    }
    if (modelTexturePreviewPath_ == selectedAsset_ &&
        modelTexturePreviewStamp_ == modelPreviewStamp_ &&
        assetPreviewState_.modelTextureSnapshot &&
        !assetPreviewState_.modelTextureSnapshot->bgraPremultiplied.empty()) return;
    if (modelTexturePreviewFuture_.valid()) {
        if (modelTexturePreviewFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            poll_model_texture_preview();
        if (modelTexturePreviewPath_ == selectedAsset_ &&
            modelTexturePreviewStamp_ == modelPreviewStamp_ &&
            assetPreviewState_.modelTextureSnapshot &&
            !assetPreviewState_.modelTextureSnapshot->bgraPremultiplied.empty()) return;
        if (modelTexturePreviewFuture_.valid()) return;
    }
    const auto generation = modelTexturePreviewGeneration_;
    const auto sourceStamp = modelPreviewStamp_;
    const auto path = selectedAsset_;
    const auto bytes = selected->encodedBytes;
    modelTexturePreviewPath_ = path;
    modelTexturePreviewStamp_ = sourceStamp;
    assetPreviewState_.modelTextureWidth = 0;
    assetPreviewState_.modelTextureHeight = 0;
    assetPreviewState_.modelTextureSnapshot.reset();
    assetPreviewState_.modelTextureStatus = "Loading model texture preview...";
    uiModel_.set_asset_preview(assetPreviewState_);
    modelTexturePreviewFuture_ = std::async(std::launch::async,
        [generation, sourceStamp, path, bytes]() {
            if (!bytes) {
                EditorImagePreviewResult result;
                result.generation = generation;
                result.sourceStamp = sourceStamp;
                result.path = path;
                result.error = "texture artifact has no bytes";
                return result;
            }
            return load_editor_image_preview_bytes(path, *bytes, generation, sourceStamp);
        });
}

void EditorLayer::poll_model_texture_preview() {
    if (!modelTexturePreviewFuture_.valid() ||
        modelTexturePreviewFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    EditorImagePreviewResult result;
    try { result = modelTexturePreviewFuture_.get(); }
    catch (const std::exception& error) {
        result.path = selectedAsset_;
        result.generation = modelTexturePreviewGeneration_;
        result.sourceStamp = modelPreviewStamp_;
        result.error = error.what();
    } catch (...) {
        result.path = selectedAsset_;
        result.generation = modelTexturePreviewGeneration_;
        result.sourceStamp = modelPreviewStamp_;
        result.error = "unknown model texture preview worker failure";
    }
    if (result.generation != modelTexturePreviewGeneration_ ||
        result.path != selectedAsset_ || result.sourceStamp != modelPreviewStamp_ ||
        !modelPreviewSnapshot_) {
        if (modelPreviewSnapshot_ && modelPreviewSnapshot_->valid()) request_model_texture_preview();
        return;
    }
    assetPreviewState_.modelTextureWidth = result.sourceWidth;
    assetPreviewState_.modelTextureHeight = result.sourceHeight;
    assetPreviewState_.modelTextureSnapshot = std::move(result.snapshot);
    if (!result.error.empty()) {
        assetPreviewState_.modelTextureWidth = 0;
        assetPreviewState_.modelTextureHeight = 0;
        assetPreviewState_.modelTextureStatus = "Texture preview unavailable: " + result.error;
    } else {
        assetPreviewState_.modelTextureStatus = "Texture preview ready (WIC)";
    }
    uiModel_.set_asset_preview(assetPreviewState_);
}

void EditorLayer::navigate_model_preview(ViewportNavigation action, math::Vec2 delta) {
    if (!modelPreviewSnapshot_ || !modelPreviewSnapshot_->valid()) return;
    if (action == ViewportNavigation::Orbit) modelPreviewScene_.orbit({delta.x, delta.y});
    else if (action == ViewportNavigation::Zoom) modelPreviewScene_.zoom(delta.y);
    else return;
    assetPreviewState_.modelPreviewScene = modelPreviewScene_.state();
    uiModel_.set_asset_preview(assetPreviewState_);
}

void EditorLayer::reset_model_preview() {
    if (!modelPreviewSnapshot_ || !modelPreviewSnapshot_->valid()) return;
    modelPreviewScene_.reset_camera();
    assetPreviewState_.modelPreviewScene = modelPreviewScene_.state();
    uiModel_.set_asset_preview(assetPreviewState_);
}

void EditorLayer::handle_asset_action(EditorAssetAction action, std::string path, std::string value) {
    std::string error;
    switch (action) {
    case EditorAssetAction::Open:
        set_selected_asset(path);
        if (std::filesystem::path(path).extension() == ".scene" && activeWorld_) dispatch_command(EditorCommand::OpenScene, path, *activeWorld_);
        else { lastStatus_ = "Selected " + path + " (no importer registered)"; push_console(lastStatus_); }
        return;
    case EditorAssetAction::Navigate:
        set_selected_asset({});
        set_asset_directory(std::filesystem::path(path));
        return;
    case EditorAssetAction::Refresh:
        reset_asset_manifest("AssetSystem manifest refresh requested");
        ++fileScanGeneration_;
        assetsDirty_ = true;
        lastStatus_ = "Refreshing resources...";
        return;
    case EditorAssetAction::Rename: {
        const std::filesystem::path newName(value);
        if (value.empty() || newName.filename() != newName || value == "." || value == "..") {
            lastStatus_ = "Invalid resource name";
            return;
        }
        const auto target = std::filesystem::path(path).parent_path() / newName;
        if (!fileSystem_.rename(path, target, &error)) {
            lastStatus_ = "Rename failed: " + error;
            push_console(lastStatus_);
            return;
        }
        if (selectedAsset_ == path) set_selected_asset(target.generic_string());
        reset_asset_manifest("AssetSystem manifest refresh requested");
        ++fileScanGeneration_;
        assetsDirty_ = true;
        lastStatus_ = "Renamed resource to " + newName.generic_string();
        break;
    }
    case EditorAssetAction::NewFolder: {
        const std::filesystem::path newName(value);
        if (value.empty() || newName.filename() != newName || value == "." || value == "..") {
            lastStatus_ = "Invalid folder name";
            return;
        }
        const auto target = std::filesystem::path(path) / newName;
        if (!fileSystem_.ensure_directory(target, &error)) {
            lastStatus_ = "Create folder failed: " + error;
            push_console(lastStatus_);
            return;
        }
        reset_asset_manifest("AssetSystem manifest refresh requested");
        ++fileScanGeneration_;
        assetsDirty_ = true;
        lastStatus_ = "Created folder " + newName.generic_string();
        break;
    }
    case EditorAssetAction::Delete:
        if (!fileSystem_.remove(path, &error)) {
            lastStatus_ = "Delete failed: " + error;
            push_console(lastStatus_);
            return;
        }
        if (selectedAsset_ == path) set_selected_asset({});
        reset_asset_manifest("AssetSystem manifest refresh requested");
        ++fileScanGeneration_;
        assetsDirty_ = true;
        lastStatus_ = "Deleted resource " + std::filesystem::path(path).filename().string();
        break;
    }
    push_console(lastStatus_);
}

bool EditorLayer::drop_asset_to_viewport(std::string path, math::Vec2 point) {
    if (!activeWorld_) {
        lastStatus_ = "Cannot drop a resource without an active scene";
        push_console(lastStatus_);
        return false;
    }
    const auto normalized = std::filesystem::u8path(path).lexically_normal();
    const auto normalizedText = normalized.generic_u8string();
    bool directory = false;
    if (normalized.empty() || normalized.is_absolute() || normalized == ".." ||
        normalizedText.rfind("../", 0) == 0 || !fileSystem_.exists(normalized, &directory)) {
        lastStatus_ = "Drop rejected: resource is outside the project or unavailable";
        push_console(lastStatus_);
        return false;
    }
    if (directory || !fileSystem_.exists(normalized, &directory)) {
        lastStatus_ = "Drop rejected: folders cannot be instantiated in the viewport";
        push_console(lastStatus_);
        return false;
    }
    const auto* indexed = assetIndex_ ? assetIndex_->find(normalized.generic_string()) : nullptr;
    const auto kind = indexed ? indexed->descriptor.kind : AssetPreviewCatalog::classify(normalized);
    const bool supported = kind == AssetPreviewKind::Model || kind == AssetPreviewKind::Image ||
        kind == AssetPreviewKind::Audio || kind == AssetPreviewKind::Video || kind == AssetPreviewKind::Material;
    if (!supported) {
        lastStatus_ = "Drop rejected: " + std::string(AssetPreviewCatalog::kind_name(kind)) +
            " resources are not instantiable yet";
        push_console(lastStatus_);
        return false;
    }
    assets::AssetId assetId = 0;
    if (assetSystem_) {
        if (assetSystemRootNeedsRestart_ || !assetSystem_->initialized()) {
            lastStatus_ = "Drop rejected: AssetSystem manifest is unavailable";
            push_console(lastStatus_);
            return false;
        }
        if (!assetManifest_) {
            lastStatus_ = "Drop rejected: AssetSystem manifest is still scanning";
            push_console(lastStatus_);
            return false;
        }
        const auto sourcePath = fileSystem_.resolve_existing(normalized);
        const auto expectedType = [&]() -> std::string_view {
            switch (kind) {
            case AssetPreviewKind::Image: return "texture";
            case AssetPreviewKind::Audio: return "audio";
            case AssetPreviewKind::Video: return "video";
            case AssetPreviewKind::Model: return "model";
            case AssetPreviewKind::Material: return "material";
            default: return {};
            }
        }();
        if (!sourcePath.empty()) {
            for (const auto& entry : *assetManifest_) {
                if (entry.id == 0 || entry.key.type != expectedType) continue;
                const auto manifestPath = entry.sourcePath.lexically_normal();
                if (manifestPath == sourcePath.lexically_normal()) {
                    assetId = entry.id;
                    break;
                }
                std::error_code equivalentError;
                if (std::filesystem::equivalent(manifestPath, sourcePath, equivalentError) && !equivalentError) {
                    assetId = entry.id;
                    break;
                }
            }
        }
        if (assetId == 0) {
            lastStatus_ = "Drop rejected: resource is not present in the current AssetSystem manifest";
            push_console(lastStatus_);
            return false;
        }
    }
    const auto viewport = editorUi_.viewport_rect();
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || viewport.width <= 0.0f ||
        viewport.height <= 0.0f || !viewport.contains(point)) {
        lastStatus_ = "Drop rejected: viewport position is invalid";
        push_console(lastStatus_);
        return false;
    }
    activeWorld_->register_component_type<AssetReferenceComponent>("AssetReference");
    if (!checkpoint(*activeWorld_)) return false;

    auto name = normalized.stem().string();
    if (name.empty()) name = normalized.filename().string();
    auto& object = activeWorld_->create_object(name.empty() ? "Asset" : name);
    auto* reference = object.add_component<AssetReferenceComponent>(normalized.generic_string(), assetId);
    auto* transform = object.get_component<components::TransformComponent>();
    if (!reference || !transform) {
        undo_.pop_back();
        object.destroy();
        lastStatus_ = "Drop failed: could not create the asset reference object";
        push_console(lastStatus_);
        return false;
    }
    const float normalizedX = (point.x - viewport.x) / viewport.width * 2.0f - 1.0f;
    const float normalizedY = 1.0f - (point.y - viewport.y) / viewport.height * 2.0f;
    transform->set_position({normalizedX * 5.0f, normalizedY * 5.0f, 0.0f});
    layout_.selectedObject = object.id();
    document_changed();
    lastStatus_ = "Created asset reference " + std::string(object.name()) + " (" +
        std::string(AssetPreviewCatalog::kind_name(kind)) +
        (assetId == 0 ? std::string{} : ", id=" + std::to_string(assetId)) + ")";
    push_console(lastStatus_);
    return true;
}

bool EditorLayer::create_asset_reference_from_window_drop(World& world, std::string nativePath,
                                                           WindowClientPx clientPoint) {
    activeWorld_ = &world;
    const auto native = std::filesystem::u8path(nativePath);
    const auto relative = fileSystem_.project_relative_existing(native);
    if (relative.empty()) {
        lastStatus_ = "Drop rejected: native path is outside the project or unavailable";
        push_console(lastStatus_);
        return false;
    }
    const auto logical = window_client_to_ui_logical_px(
        clientPoint, effective_editor_ui_scale(displayDpiScale_, layout_.uiScale));
    if (!logical) {
        lastStatus_ = "Drop rejected: native drop coordinates are invalid";
        push_console(lastStatus_);
        return false;
    }
    return create_asset_reference_at_viewport(world, relative.generic_u8string(),
                                              {logical->x, logical->y});
}

void EditorLayer::consume_file_scan() {
    if (!fileScanFuture_.valid() ||
        fileScanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    auto result = fileScanFuture_.get();
    if (result.generation != fileScanGeneration_) {
        assetsDirty_ = true;
        return;
    }

    fileSystem_ = std::move(result.service);
    projectFiles_ = std::move(result.entries);
    assetIndex_ = std::move(result.index);
    if (!result.changes.empty()) ++projectFilesRevision_;
    assetEntries_.clear();
    for (const auto& entry : projectFiles_) {
        if (assetEntries_.size() >= 256) break;
        assetEntries_.push_back(entry.relativePath.generic_string() + (entry.directory ? "/" : ""));
    }
    assetsDirty_ = false;
    request_asset_preview();

    bool styleChanged = false;
    for (const auto& change : result.changes) {
        if (change.relativePath == std::filesystem::path(layout_.styleFile)) styleChanged = true;
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

void EditorLayer::refresh_ide_tools() {
    ideTools_ = EditorToolIntegration::discover_ides(buildSystem_.project_root());
    buildUiDirty_ = true;
}

void EditorLayer::refresh_project_integration() {
    projectDiscovery_ = EditorProjectIntegration::discover(buildSystem_.project_root(), &buildProfile_);
    if (!projectDiscovery_.valid) {
        projectIntegrationStatus_ = "Project discovery failed: " + projectDiscovery_.error;
        clangdConfigStatus_ = "Clangd config unavailable";
        buildUiDirty_ = true;
        return;
    }
    projectIntegrationStatus_ = "Discovered " + std::to_string(projectDiscovery_.files.size()) + " project files";
    if (!projectDiscovery_.error.empty()) projectIntegrationStatus_ += " (bounded scan)";
    if (!projectDiscovery_.recommendedProjectFile.empty()) {
        projectIntegrationStatus_ += "; suggested " + projectDiscovery_.recommendedProjectFile.generic_string();
    }
    if (!projectDiscovery_.compileCommandsFile.empty()) {
        clangdConfigStatus_ = "compile_commands ready: " + projectDiscovery_.compileCommandsFile.generic_string();
    } else {
        clangdConfigStatus_ = "No compile_commands.json discovered";
    }
    buildUiDirty_ = true;
}

bool EditorLayer::associate_project_file(std::string_view target) {
    const auto selected = target.empty() ? projectDiscovery_.recommendedProjectFile :
        std::filesystem::path(std::string(target));
    if (selected.empty()) {
        projectIntegrationStatus_ = "Project association unavailable: no project file discovered";
        lastStatus_ = projectIntegrationStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    const auto found = std::find_if(projectDiscovery_.files.begin(), projectDiscovery_.files.end(),
                                    [&](const auto& file) {
        return file.relativePath.lexically_normal() == selected.lexically_normal();
    });
    if (found == projectDiscovery_.files.end() || found->kind == EditorProjectFileKind::CompileCommands ||
        found->kind == EditorProjectFileKind::ClangdConfig) {
        projectIntegrationStatus_ = "Project association rejected: file is not a supported project";
        lastStatus_ = projectIntegrationStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    buildProfile_.projectFile = found->relativePath;
    const auto current = std::find_if(buildProfiles_.begin(), buildProfiles_.end(),
                                      [&](const auto& profile) { return profile.id == selectedBuildProfileId_; });
    if (current != buildProfiles_.end()) *current = buildProfile_;
    projectIntegrationStatus_ = "Associated " + found->relativePath.generic_string() + " (unsaved profile)";
    lastStatus_ = "Associated project file: " + found->relativePath.generic_string();
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::generate_clangd_config() {
    const auto plan = EditorProjectIntegration::plan_clangd_config(buildSystem_.project_root(), &buildProfile_);
    if (!plan.valid) {
        clangdConfigStatus_ = "Generate failed: " + plan.error;
        lastStatus_ = clangdConfigStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    std::string error;
    if (!EditorProjectIntegration::write_clangd_config(buildSystem_.project_root(), plan, &error)) {
        clangdConfigStatus_ = "Generate failed: " + error;
        lastStatus_ = clangdConfigStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    refresh_project_integration();
    clangdConfigStatus_ = "Generated .clangd from " + plan.compileCommandsFile.generic_string();
    lastStatus_ = clangdConfigStatus_;
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::save_build_profile() {
    if (buildProfiles_.empty()) buildProfiles_.push_back(buildProfile_);
    auto current = std::find_if(buildProfiles_.begin(), buildProfiles_.end(),
                                [&](const auto& profile) { return profile.id == selectedBuildProfileId_; });
    if (current == buildProfiles_.end()) {
        buildProfile_.id = selectedBuildProfileId_.empty() ? buildProfile_.id : selectedBuildProfileId_;
        buildProfiles_.push_back(buildProfile_);
    } else {
        *current = buildProfile_;
    }
    EditorBuildProfileSet set;
    set.profiles = buildProfiles_;
    set.selectedId = selectedBuildProfileId_.empty() ? buildProfile_.id : selectedBuildProfileId_;
    std::string json;
    std::string error;
    if (!EditorBuildProfileStore::serialize_set(set, json, &error) ||
        !fileSystem_.write_text_atomic(".shinkou/build-profile.json", json, &error)) {
        buildProfileStatus_ = "Save failed: " + error;
        lastStatus_ = "Build profile save failed: " + error;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    buildProfileStatus_ = "Saved .shinkou/build-profile.json";
    lastStatus_ = "Build profile saved";
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::load_build_profile() {
    std::string json;
    std::string error;
    if (!fileSystem_.read_text(".shinkou/build-profile.json", json, &error)) {
        // A first-run project is allowed to use the in-memory defaults. Only
        // report errors for a file that actually exists, so a missing config
        // is not mistaken for a broken toolchain.
        bool directory = false;
        if (fileSystem_.exists(".shinkou/build-profile.json", &directory) && !directory) {
            buildProfileStatus_ = "Load failed: " + error;
            lastStatus_ = "Build profile load failed: " + error;
            push_console(lastStatus_);
        } else {
            buildProfiles_.clear();
            buildProfiles_.push_back(buildProfile_);
            selectedBuildProfileId_ = buildProfile_.id;
            buildProfileStatus_ = "Build profile defaults (not saved)";
        }
        buildUiDirty_ = true;
        return false;
    }
    const auto setParsed = EditorBuildProfileStore::deserialize_set(json);
    if (setParsed.valid) {
        buildProfiles_ = setParsed.set.profiles;
        selectedBuildProfileId_ = setParsed.set.selectedId;
        const auto selected = std::find_if(buildProfiles_.begin(), buildProfiles_.end(),
                                           [&](const auto& profile) { return profile.id == selectedBuildProfileId_; });
        if (selected == buildProfiles_.end()) {
            buildProfileStatus_ = "Load failed: selected profile is missing";
            lastStatus_ = "Build profile load failed: selected profile is missing";
            push_console(lastStatus_);
            buildUiDirty_ = true;
            return false;
        }
        buildProfile_ = *selected;
        buildProfileStatus_ = "Loaded .shinkou/build-profile.json (" + std::to_string(buildProfiles_.size()) + " profiles)";
        push_console(buildProfileStatus_);
        buildUiDirty_ = true;
        return true;
    }
    const auto parsed = EditorBuildProfileStore::deserialize(json);
    if (!parsed.valid) {
        buildProfileStatus_ = "Load failed: " + setParsed.error;
        lastStatus_ = "Build profile load failed: " + setParsed.error;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    buildProfile_ = parsed.profile;
    buildProfiles_.clear();
    buildProfiles_.push_back(buildProfile_);
    selectedBuildProfileId_ = buildProfile_.id;
    buildProfileStatus_ = "Loaded .shinkou/build-profile.json";
    push_console(buildProfileStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::select_build_profile(std::string_view id) {
    const auto selected = std::find_if(buildProfiles_.begin(), buildProfiles_.end(),
                                       [id](const auto& profile) { return profile.id == id; });
    if (selected == buildProfiles_.end()) return false;
    if (build_running()) {
        buildProfileStatus_ = "Cannot switch profile while a build is running";
        lastStatus_ = buildProfileStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    buildProfile_ = *selected;
    selectedBuildProfileId_ = buildProfile_.id;
    refresh_project_integration();
    buildProfileStatus_ = "Selected profile: " + buildProfile_.name + " (unsaved selection)";
    lastStatus_ = "Build profile selected: " + buildProfile_.name;
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::edit_build_profile_field(std::string_view id, std::string_view value) {
    if (id.rfind("build-profile.", 0) != 0 || value.size() > 16u * 1024u || value.find('\0') != std::string_view::npos)
        return false;
    EditorBuildProfile candidate = buildProfile_;
    const auto text = std::string(value);
    const auto relative_path = [&](std::string_view field, std::filesystem::path& output, bool allowEmpty) {
        if (id != field) return false;
        if (text.empty()) { if (allowEmpty) output.clear(); else return false; }
        else {
            const std::filesystem::path path(text);
            if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
            const auto normalized = path.lexically_normal();
            const auto generic = normalized.generic_string();
            if (normalized.empty() || generic == ".." || generic.rfind("../", 0) == 0) return false;
            output = normalized;
        }
        return true;
    };
    bool recognized = true;
    if (id == "build-profile.name") candidate.name = text;
    else if (relative_path("build-profile.sourceDirectory", candidate.sourceDirectory, false)) { }
    else if (relative_path("build-profile.buildDirectory", candidate.buildDirectory, false)) { }
    else if (relative_path("build-profile.projectFile", candidate.projectFile, true)) { }
    else if (id == "build-profile.generator") candidate.generator = text;
    else if (id == "build-profile.configuration") candidate.configuration = text;
    else if (id == "build-profile.target") candidate.target = text;
    else recognized = false;
    if (!recognized) return false;
    std::string validation;
    std::string ignored;
    if (!EditorBuildProfileStore::serialize(candidate, ignored, &validation)) return false;
    buildProfile_ = std::move(candidate);
    if (buildProfiles_.empty()) buildProfiles_.push_back(buildProfile_);
    else {
        const auto current = std::find_if(buildProfiles_.begin(), buildProfiles_.end(),
                                          [&](const auto& profile) { return profile.id == selectedBuildProfileId_; });
        if (current != buildProfiles_.end()) *current = buildProfile_;
    }
    buildProfileStatus_ = "Unsaved profile changes";
    lastStatus_ = "Build profile field updated";
    refresh_project_integration();
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::open_project_in_ide(std::string_view target, std::string_view selectedFile,
                                      std::size_t line, std::size_t column) {
    EditorIdeKind kind = EditorIdeKind::Unknown;
    if (target == "visual-studio") kind = EditorIdeKind::VisualStudio;
    else if (target == "rider") kind = EditorIdeKind::Rider;
    else if (target == "vscode") kind = EditorIdeKind::VisualStudioCode;
    else if (target == "clangd") kind = EditorIdeKind::Clangd;
    const auto plan = EditorToolIntegration::plan_ide_launch(
        buildSystem_.project_root(), buildProfile_, kind,
        selectedFile.empty() ? std::string_view(selectedAsset_) : selectedFile, line, column);
    if (!plan.valid) {
        ideStatus_ = "Unavailable: " + plan.error;
        lastStatus_ = "IDE launch unavailable: " + plan.error;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    const auto launched = EditorToolIntegration::launch(plan);
    if (launched.state != EditorExternalLaunchState::Launched) {
        ideStatus_ = launched.error.empty() ? "IDE launch failed" : "Launch failed: " + launched.error;
        lastStatus_ = "IDE launch failed" + (launched.error.empty() ? std::string{} : ": " + launched.error);
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    ideProcessId_ = launched.processId;
    ideProcessState_ = EditorExternalProcessState::Running;
    ideProcessExitCode_ = 0;
    ideProcessName_ = std::string(EditorToolIntegration::ide_name(kind));
    ideProcessPollTime_ = {};
    ideStatus_ = "Launched " + ideProcessName_ + " (PID " + std::to_string(ideProcessId_) + "; tracking)";
    lastStatus_ = ideStatus_;
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::select_build_diagnostic(std::string_view indexText) {
    std::size_t index = 0;
    try {
        index = static_cast<std::size_t>(std::stoull(std::string(indexText)));
    } catch (...) { return false; }
    if (index >= lastBuildResult_.diagnostics.size()) return false;
    selectedDiagnosticIndex_ = index;
    const auto& diagnostic = lastBuildResult_.diagnostics[index];
    if (!diagnostic.file.empty()) set_selected_asset(diagnostic.file.generic_string());
    lastStatus_ = "Selected diagnostic " + std::to_string(index + 1);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::set_diagnostic_filter(std::string_view filter) {
    if (filter != "all" && filter != "errors" && filter != "warnings" && filter != "notes") return false;
    if (diagnosticFilter_ == filter) return true;
    diagnosticFilter_ = std::string(filter);
    buildUiDirty_ = true;
    return true;
}

void EditorLayer::poll_ide_process() {
    if (ideProcessId_ == 0 || ideProcessState_ != EditorExternalProcessState::Running) return;
    const auto now = std::chrono::steady_clock::now();
    if (ideProcessPollTime_ != std::chrono::steady_clock::time_point{} &&
        now - ideProcessPollTime_ < std::chrono::milliseconds(250)) return;
    ideProcessPollTime_ = now;
    const auto status = EditorToolIntegration::query_process(ideProcessId_);
    if (status.state == ideProcessState_ && status.exitCode == ideProcessExitCode_) return;
    ideProcessState_ = status.state;
    ideProcessExitCode_ = status.exitCode;
    switch (status.state) {
    case EditorExternalProcessState::Running:
        ideStatus_ = "Running " + ideProcessName_ + " (PID " + std::to_string(ideProcessId_) + ")";
        break;
    case EditorExternalProcessState::Exited:
        ideStatus_ = "Exited " + ideProcessName_ + " (code " + std::to_string(status.exitCode) + ")";
        break;
    case EditorExternalProcessState::NotFound:
        ideStatus_ = "IDE process ended before it could be queried";
        break;
    case EditorExternalProcessState::QueryFailed:
        ideStatus_ = "IDE process tracking failed: " + status.error;
        break;
    case EditorExternalProcessState::Unsupported:
        ideStatus_ = "IDE launched; process tracking unsupported";
        break;
    default:
        break;
    }
    buildUiDirty_ = true;
}

bool EditorLayer::open_diagnostic_in_ide(std::string_view target) {
    const auto second = target.rfind(':');
    if (second == std::string_view::npos || second == 0) return false;
    const auto first = target.rfind(':', second - 1);
    if (first == std::string_view::npos || first == 0 || second + 1 >= target.size()) return false;
    std::size_t line = 0;
    std::size_t column = 0;
    try {
        line = static_cast<std::size_t>(std::stoull(std::string(target.substr(first + 1, second - first - 1))));
        column = static_cast<std::size_t>(std::stoull(std::string(target.substr(second + 1))));
    } catch (...) { return false; }
    const auto file = target.substr(0, first);
    EditorIdeKind preferred = EditorIdeKind::VisualStudioCode;
    if (std::find_if(ideTools_.begin(), ideTools_.end(), [](const auto& ide) {
            return ide.kind == EditorIdeKind::VisualStudio && ide.available;
        }) != ideTools_.end()) preferred = EditorIdeKind::VisualStudio;
    else if (std::find_if(ideTools_.begin(), ideTools_.end(), [](const auto& ide) {
            return ide.kind == EditorIdeKind::Rider && ide.available;
        }) != ideTools_.end()) preferred = EditorIdeKind::Rider;
    return open_project_in_ide(EditorToolIntegration::ide_name(preferred) == "Visual Studio" ? "visual-studio" :
                               EditorToolIntegration::ide_name(preferred) == "Rider" ? "rider" : "vscode",
                               file, line, column);
}

void EditorLayer::sync_build_ui_state() {
    EditorBuildUiState state;
    state.profileName = buildProfile_.name;
    state.selectedProfileId = selectedBuildProfileId_;
    state.profileStatus = buildProfileStatus_;
    state.status = buildStatus_;
    state.ideStatus = ideStatus_;
    state.running = build_running();
    state.compileCommandCount = compileCommands_.size();
    state.compileCommandsStatus = compileCommandsStatus_;
    state.diagnosticFilter = diagnosticFilter_;
    state.projectStatus = projectIntegrationStatus_;
    state.recommendedProjectFile = projectDiscovery_.recommendedProjectFile.generic_string();
    state.clangdStatus = clangdConfigStatus_;
    for (const auto& profile : buildProfiles_) state.profileIds.push_back(profile.id);
    state.profileFields = {
        {"build-profile.name", "Name", buildProfile_.name, true, false},
        {"build-profile.sourceDirectory", "Source", buildProfile_.sourceDirectory.generic_string(), true, false},
        {"build-profile.buildDirectory", "Build Dir", buildProfile_.buildDirectory.generic_string(), true, false},
        {"build-profile.projectFile", "Project", buildProfile_.projectFile.generic_string(), true, false},
        {"build-profile.generator", "Generator", buildProfile_.generator, true, false},
        {"build-profile.configuration", "Config", buildProfile_.configuration, true, false},
        {"build-profile.target", "Target", buildProfile_.target, true, false},
    };
    for (const auto& tool : buildSystem_.toolchains()) {
        state.tools.push_back({tool.name, tool.executable.generic_string(), tool.available});
    }
    for (const auto& ide : ideTools_) {
        state.ides.push_back({ide.name, ide.executable.generic_string(), ide.available});
    }
    for (const auto& projectFile : projectDiscovery_.files) {
        state.projectFiles.push_back({std::string(EditorProjectIntegration::file_kind_name(projectFile.kind)),
                                      projectFile.relativePath.generic_string(), projectFile.recommended});
    }
    const auto append_diagnostic = [&state](const EditorBuildDiagnostic& diagnostic) {
        ++state.diagnosticTotalCount;
        const char* severity = diagnostic.severity == EditorDiagnosticSeverity::Error ? "error" :
            diagnostic.severity == EditorDiagnosticSeverity::Warning ? "warning" : "note";
        if (diagnostic.severity == EditorDiagnosticSeverity::Error) ++state.diagnosticErrorCount;
        else if (diagnostic.severity == EditorDiagnosticSeverity::Warning) ++state.diagnosticWarningCount;
        else ++state.diagnosticNoteCount;
        if (state.diagnostics.size() < 256) {
            state.diagnostics.push_back({severity, diagnostic.file.generic_string(), diagnostic.line,
                                         diagnostic.column, diagnostic.code, diagnostic.message});
        }
    };
    if (state.running && liveBuildOutput_) {
        std::lock_guard lock(liveBuildOutput_->mutex);
        state.diagnosticTotalCount = liveBuildOutput_->diagnosticTotalCount;
        state.diagnosticErrorCount = liveBuildOutput_->diagnosticErrorCount;
        state.diagnosticWarningCount = liveBuildOutput_->diagnosticWarningCount;
        state.diagnosticNoteCount = liveBuildOutput_->diagnosticNoteCount;
        for (const auto& diagnostic : liveBuildOutput_->diagnostics) {
            if (state.diagnostics.size() >= 256) break;
            const char* severity = diagnostic.severity == EditorDiagnosticSeverity::Error ? "error" :
                diagnostic.severity == EditorDiagnosticSeverity::Warning ? "warning" : "note";
            state.diagnostics.push_back({severity, diagnostic.file.generic_string(), diagnostic.line,
                                         diagnostic.column, diagnostic.code, diagnostic.message});
        }
    } else {
        for (const auto& diagnostic : lastBuildResult_.diagnostics) append_diagnostic(diagnostic);
    }
    state.selectedDiagnostic = selectedDiagnosticIndex_ < lastBuildResult_.diagnostics.size()
        ? selectedDiagnosticIndex_ : static_cast<std::size_t>(-1);

    const auto append_output = [&state](std::string_view value, std::string_view prefix) {
        std::size_t cursor = 0;
        while (cursor <= value.size() && state.outputLines.size() < 32) {
            const auto end = value.find('\n', cursor);
            auto line = value.substr(cursor, end == std::string_view::npos ? value.size() - cursor : end - cursor);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty()) {
                std::string text(prefix);
                text.append(line.substr(0, 240));
                state.outputLines.push_back(std::move(text));
            }
            if (end == std::string_view::npos) break;
            cursor = end + 1;
        }
    };
    if (state.running && liveBuildOutput_) {
        std::lock_guard lock(liveBuildOutput_->mutex);
        state.outputLines = liveBuildOutput_->lines;
        const auto append_pending = [&state](std::string_view value, std::string_view prefix) {
            if (value.empty()) return;
            std::string line(prefix);
            line.append(value.substr(0, 240));
            if (value.size() > 240) line += "...";
            state.outputLines.push_back(std::move(line));
        };
        append_pending(liveBuildOutput_->standardOutputPending, "out  ");
        append_pending(liveBuildOutput_->standardErrorPending, "err  ");
        liveBuildUiRevision_ = liveBuildOutput_->revision;
    } else {
        append_output(lastBuildResult_.standardOutput, "out  ");
        append_output(lastBuildResult_.standardError, "err  ");
        if (state.outputLines.empty() && !lastBuildResult_.error.empty())
            state.outputLines.push_back("error  " + lastBuildResult_.error.substr(0, 240));
    }
    if (state.outputLines.size() > 12)
        state.outputLines.erase(state.outputLines.begin(), state.outputLines.end() - 12);
    uiModel_.set_build_state(std::move(state));
}

bool EditorLayer::live_build_output_changed() const noexcept {
    if (!build_running() || !liveBuildOutput_) return false;
    std::lock_guard lock(liveBuildOutput_->mutex);
    return liveBuildOutput_->revision != liveBuildUiRevision_;
}

bool EditorLayer::import_compile_commands(std::string_view path) {
    const auto target = path.empty() ? std::filesystem::path{"compile_commands.json"} : std::filesystem::path(path);
    std::string json;
    std::string error;
    if (!fileSystem_.read_text(target, json, &error)) {
        compileCommandsStatus_ = "Import failed: " + error;
        lastStatus_ = compileCommandsStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    const auto parsed = EditorCompileCommands::parse(json, fileSystem_.root());
    if (!parsed.valid) {
        compileCommandsStatus_ = "Import failed: " + parsed.error;
        lastStatus_ = compileCommandsStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    compileCommands_ = parsed.commands;
    compileCommandsStatus_ = "Imported " + std::to_string(compileCommands_.size()) + " commands";
    lastStatus_ = compileCommandsStatus_ + " from " + target.generic_string();
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

bool EditorLayer::export_compile_commands(std::string_view path) {
    if (compileCommands_.empty()) {
        compileCommandsStatus_ = "Export skipped: no compile commands loaded";
        lastStatus_ = compileCommandsStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    std::string json;
    std::string error;
    if (!EditorCompileCommands::serialize(compileCommands_, fileSystem_.root(), json, &error)) {
        compileCommandsStatus_ = "Export failed: " + error;
        lastStatus_ = compileCommandsStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    const auto target = path.empty() ? std::filesystem::path{"compile_commands.export.json"} : std::filesystem::path(path);
    if (!fileSystem_.write_text_atomic(target, json, &error)) {
        compileCommandsStatus_ = "Export failed: " + error;
        lastStatus_ = compileCommandsStatus_;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    compileCommandsStatus_ = "Exported " + std::to_string(compileCommands_.size()) + " commands";
    lastStatus_ = compileCommandsStatus_ + " to " + target.generic_string();
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

void EditorLayer::poll_build() {
    if (!buildFuture_.valid() ||
        buildFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    try {
        lastBuildResult_ = buildFuture_.get();
    } catch (const std::exception& error) {
        lastBuildResult_ = {};
        lastBuildResult_.state = EditorBuildProcessState::Failed;
        lastBuildResult_.error = std::string("Build worker failed: ") + error.what();
    } catch (...) {
        lastBuildResult_ = {};
        lastBuildResult_.state = EditorBuildProcessState::Failed;
        lastBuildResult_.error = "Build worker failed with an unknown exception";
    }
    buildCancel_.reset();
    liveBuildOutput_.reset();
    switch (lastBuildResult_.state) {
    case EditorBuildProcessState::Succeeded:
        buildStatus_ = "Succeeded";
        lastStatus_ = "Build succeeded";
        push_console(lastStatus_);
        break;
    case EditorBuildProcessState::Cancelled:
        buildStatus_ = "Cancelled";
        lastStatus_ = "Build cancelled";
        push_console(lastStatus_);
        break;
    case EditorBuildProcessState::TimedOut:
        buildStatus_ = "Timed out";
        lastStatus_ = "Build timed out";
        push_console(lastStatus_ + (lastBuildResult_.error.empty() ? std::string{} : ": " + lastBuildResult_.error));
        break;
    default:
        buildStatus_ = lastBuildResult_.error.empty() ? "Failed" : "Failed: " + lastBuildResult_.error;
        lastStatus_ = lastBuildResult_.error.empty() ? "Build failed" : "Build failed: " + lastBuildResult_.error;
        push_console(lastStatus_);
        break;
    }
    for (const auto& diagnostic : lastBuildResult_.diagnostics) {
        const char* severity = diagnostic.severity == EditorDiagnosticSeverity::Error ? "error" :
            diagnostic.severity == EditorDiagnosticSeverity::Warning ? "warning" : "note";
        std::string entry = std::string(severity) + ": " + diagnostic.file.generic_string();
        if (diagnostic.line != 0) {
            entry += ":" + std::to_string(diagnostic.line);
            if (diagnostic.column != 0) entry += ":" + std::to_string(diagnostic.column);
        }
        if (!diagnostic.code.empty()) entry += " [" + diagnostic.code + "]";
        entry += " " + diagnostic.message;
        push_console(std::move(entry));
    }
    buildUiDirty_ = true;
}

bool EditorLayer::start_build() {
    poll_build();
    if (buildFuture_.valid()) {
        lastStatus_ = "Build is already running";
        buildStatus_ = "Running: " + buildProfile_.name;
        buildUiDirty_ = true;
        return false;
    }
    selectedDiagnosticIndex_ = static_cast<std::size_t>(-1);
    const auto plan = buildSystem_.plan(buildProfile_);
    if (!plan.valid) {
        lastBuildResult_ = {};
        lastBuildResult_.state = EditorBuildProcessState::LaunchFailed;
        lastBuildResult_.error = plan.error;
        buildStatus_ = "Unavailable: " + plan.error;
        lastStatus_ = "Build unavailable: " + plan.error;
        push_console(lastStatus_);
        buildUiDirty_ = true;
        return false;
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto root = buildSystem_.project_root();
    const auto profileName = buildProfile_.name;
    buildCancel_ = cancel;
    auto liveOutput = std::make_shared<LiveBuildOutput>();
    liveBuildOutput_ = liveOutput;
    liveBuildUiRevision_ = 0;
    const auto append_live_output = [liveOutput, root](EditorBuildOutputStream stream,
                                                  std::string_view chunk, bool flush) {
        std::lock_guard lock(liveOutput->mutex);
        auto& pending = stream == EditorBuildOutputStream::StandardOutput
            ? liveOutput->standardOutputPending : liveOutput->standardErrorPending;
        pending.append(chunk.data(), chunk.size());
        if (!chunk.empty()) ++liveOutput->revision;
        const auto prefix = stream == EditorBuildOutputStream::StandardOutput ? "out  " : "err  ";
        const auto publish = [&](std::string_view value) {
            if (value.empty()) return;
            const auto parsedDiagnostics = EditorBuildSystem::parse_diagnostics(value, root);
            for (const auto& diagnostic : parsedDiagnostics) {
                ++liveOutput->diagnosticTotalCount;
                if (diagnostic.severity == EditorDiagnosticSeverity::Error) ++liveOutput->diagnosticErrorCount;
                else if (diagnostic.severity == EditorDiagnosticSeverity::Warning) ++liveOutput->diagnosticWarningCount;
                else ++liveOutput->diagnosticNoteCount;
                if (liveOutput->diagnostics.size() < 256) liveOutput->diagnostics.push_back(diagnostic);
            }
            std::string line(prefix);
            line.append(value.substr(0, 240));
            if (value.size() > 240) line += "...";
            liveOutput->lines.push_back(std::move(line));
            if (liveOutput->lines.size() > 128)
                liveOutput->lines.erase(liveOutput->lines.begin(), liveOutput->lines.begin() + 64);
            ++liveOutput->revision;
        };
        while (true) {
            const auto end = pending.find('\n');
            if (end == std::string::npos) break;
            std::string_view line(pending.data(), end);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            publish(line);
            pending.erase(0, end + 1);
        }
        if (!flush && pending.size() > 240) {
            publish(std::string_view(pending.data(), 240));
            pending.clear();
        } else if (flush && !pending.empty()) {
            std::string_view line(pending);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            publish(line);
            pending.clear();
        }
    };
    buildFuture_ = std::async(std::launch::async,
        [commands = plan.commands, cancel, root, liveOutput, append_live_output]() mutable {
            EditorBuildProcessResult combined;
            combined.state = EditorBuildProcessState::Succeeded;
            EditorBuildProcessOptions options;
            options.diagnosticRoot = root;
            options.outputCallback = [append_live_output](EditorBuildOutputStream stream, std::string_view chunk) {
                append_live_output(stream, chunk, false);
            };
            for (const auto& command : commands) {
                const auto result = EditorBuildProcess::run(command, options, *cancel);
                if (!result.standardOutput.empty()) {
                    if (!combined.standardOutput.empty()) combined.standardOutput.push_back('\n');
                    combined.standardOutput += result.standardOutput;
                }
                if (!result.standardError.empty()) {
                    if (!combined.standardError.empty()) combined.standardError.push_back('\n');
                    combined.standardError += result.standardError;
                }
                combined.diagnostics.insert(combined.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
                combined.exitCode = result.exitCode;
                if (result.state != EditorBuildProcessState::Succeeded) {
                    combined.state = result.state;
                    combined.error = result.error;
                    append_live_output(EditorBuildOutputStream::StandardOutput, {}, true);
                    append_live_output(EditorBuildOutputStream::StandardError, {}, true);
                    return combined;
                }
            }
            append_live_output(EditorBuildOutputStream::StandardOutput, {}, true);
            append_live_output(EditorBuildOutputStream::StandardError, {}, true);
            return combined;
        });
    lastBuildResult_ = {};
    buildStatus_ = "Running: " + profileName;
    lastStatus_ = "Build started: " + profileName;
    push_console(lastStatus_);
    buildUiDirty_ = true;
    return true;
}

void EditorLayer::cancel_build() {
    if (!buildCancel_) {
        lastStatus_ = "No build is running";
        buildStatus_ = lastStatus_;
        buildUiDirty_ = true;
        return;
    }
    buildCancel_->store(true, std::memory_order_relaxed);
    buildStatus_ = "Cancellation requested";
    lastStatus_ = "Build cancellation requested";
    push_console(lastStatus_);
    buildUiDirty_ = true;
}

void EditorLayer::draw(render::Renderer& renderer, World& world, Seconds dt, FrameIndex frame) {
    if (!initialized_) return;
    if (uiModel_.selected_object() != layout_.selectedObject) uiModel_.select_object(layout_.selectedObject);
    { ui::UiTimer timer(ui::UiStage::Model); uiModel_.sync(world); }
    mediaPanel_.update(dt);
    // Asset enumeration is asynchronous and on-demand. Never recursively
    // walk the project root from the render/input thread.
    poll_asset_manifest_scan();
    consume_file_scan();
    poll_asset_system_preview();
    poll_asset_preview();
    poll_image_preview();
    poll_audio_preview();
    poll_video_preview();
    poll_video_frame();
    poll_model_preview();
    poll_model_scene_assets();
    sync_model_scene_assets(world);
    poll_model_texture_preview();
    sync_media_preview_state();
    editorFilePollAccumulator_ += std::max(0.0f, dt);
    if (editorFilePollAccumulator_ >= 1.0f) {
        editorFilePollAccumulator_ = 0.0f;
        poll_editor_files();
    }
    if (assetsDirty_) {
        request_file_scan();
        request_asset_manifest_scan();
    }
    if (buildUiDirty_ || live_build_output_changed()) {
        sync_build_ui_state();
        buildUiDirty_ = false;
    }
    if (modelPreviewSnapshot_ && modelPreviewSnapshot_->valid()) {
        const auto sceneState = modelPreviewScene_.state();
        if (sceneState && sceneState->valid()) {
            const auto textureRole = assetPreviewState_.modelTextureRole == "Normal"
                ? EditorModelPreviewTextureRole::Normal
                : assetPreviewState_.modelTextureRole == "Metallic/Roughness"
                    ? EditorModelPreviewTextureRole::MetallicRoughness
                    : EditorModelPreviewTextureRole::BaseColor;
            const auto renderState = modelPreviewRenderer_.render(
                renderer, *modelPreviewSnapshot_, *sceneState, modelMaterialSelection_,
                assetPreviewState_.modelTextureSnapshot, textureRole);
            assetPreviewState_.modelGpuPreviewReady = renderState.rendererReady;
            assetPreviewState_.modelGpuMaterialApplied = renderState.materialApplied;
            assetPreviewState_.modelGpuMaterialFactorsApplied = renderState.materialFactorsApplied;
            assetPreviewState_.modelGpuTextureRoleApplied = renderState.textureRoleApplied;
            assetPreviewState_.modelGpuBaseColorTextureSampled = renderState.baseColorTextureSampled;
            assetPreviewState_.modelGpuNormalTextureSampled = renderState.normalTextureSampled;
            assetPreviewState_.modelGpuMetallicRoughnessTextureSampled =
                renderState.metallicRoughnessTextureSampled;
            assetPreviewState_.modelGpuTextureSampled = renderState.textureSampled;
            assetPreviewState_.modelGpuOffscreenTargetReady = renderState.offscreenTargetReady;
            assetPreviewState_.modelGpuOffscreenCompositeApplied = renderState.offscreenCompositeApplied;
            assetPreviewState_.modelGpuPreviewStatus = renderState.status;
        } else {
            modelPreviewRenderer_.clear(renderer);
            assetPreviewState_.modelGpuPreviewReady = false;
            assetPreviewState_.modelGpuMaterialApplied = false;
            assetPreviewState_.modelGpuMaterialFactorsApplied = false;
            assetPreviewState_.modelGpuTextureRoleApplied = false;
            assetPreviewState_.modelGpuBaseColorTextureSampled = false;
            assetPreviewState_.modelGpuNormalTextureSampled = false;
            assetPreviewState_.modelGpuMetallicRoughnessTextureSampled = false;
            assetPreviewState_.modelGpuTextureSampled = false;
            assetPreviewState_.modelGpuOffscreenTargetReady = false;
            assetPreviewState_.modelGpuOffscreenCompositeApplied = false;
            assetPreviewState_.modelGpuPreviewStatus = "GPU model preview waiting: model camera state is not ready";
        }
    } else {
        modelPreviewRenderer_.clear(renderer);
        assetPreviewState_.modelGpuPreviewReady = false;
        assetPreviewState_.modelGpuMaterialApplied = false;
        assetPreviewState_.modelGpuMaterialFactorsApplied = false;
        assetPreviewState_.modelGpuTextureRoleApplied = false;
        assetPreviewState_.modelGpuBaseColorTextureSampled = false;
        assetPreviewState_.modelGpuNormalTextureSampled = false;
        assetPreviewState_.modelGpuMetallicRoughnessTextureSampled = false;
        assetPreviewState_.modelGpuTextureSampled = false;
        assetPreviewState_.modelGpuOffscreenTargetReady = false;
        assetPreviewState_.modelGpuOffscreenCompositeApplied = false;
        assetPreviewState_.modelGpuPreviewStatus = selectedAsset_.empty()
            ? std::string{} : "GPU model preview waiting: model geometry is not ready";
    }

    std::vector<EditorModelSceneInstance> modelSceneInstances;
    std::vector<assets::AssetId> activeModelAssets;
    std::vector<std::uint64_t> activeModelObjects;
    world.each_object([&](const GameObject& object) {
        const auto* reference = object.get_component<AssetReferenceComponent>();
        if (!reference || reference->asset_id() == 0 || !object.active_in_hierarchy()) return;
        const auto record = modelSceneAssets_.find(reference->asset_id());
        if (record == modelSceneAssets_.end()) return;
        activeModelAssets.push_back(reference->asset_id());
        activeModelObjects.push_back(object.id());
        if (!record->second.snapshot || !record->second.snapshot->valid()) return;
        const auto* transform = object.get_component<components::TransformComponent>();
        if (!transform) return;
        modelSceneInstances.push_back({reference->asset_id(), object.id(), record->second.snapshot,
                                       transform->world_matrix()});
    });
    render::RenderScene editorScene;
    const auto viewport = renderer.editor_viewport().viewport;
    const float aspect = std::max(1.0e-4f, viewport.width / std::max(1.0f, viewport.height));
    editorScene.extract(world, renderer, aspect);
    modelSceneRenderState_ = modelSceneRenderer_.render(
        renderer, modelSceneInstances, editorScene.view().viewProjection,
        editorScene.view().transform.position);
    modelSceneRenderer_.prune(renderer, activeModelAssets, activeModelObjects);
    uiModel_.set_asset_preview(assetPreviewState_);
    layout_.selectedObject = uiModel_.selected_object();
    frameTimes_.push_back(std::max(dt, 0.0f) * 1000.0f);
    if (frameTimes_.size() > 120) frameTimes_.erase(frameTimes_.begin());
    editorUi_.set_display_size(displayWidth_, displayHeight_, effective_editor_ui_scale(displayDpiScale_, layout_.uiScale));
    editorUi_.set_asset_revision(projectFilesRevision_);
    editorUi_.build(uiModel_, projectFiles_, renderer, mediaPanel_, layout_, consoleEntries_,
                    lastStatus_, dockWorkspace_, dt);
    renderer.set_editor_ui_render_list(&editorUi_.render_list());
    const auto renderCapabilities = renderer.capabilities();
    if (!renderCapabilities.supportsNativeUi && !uiPresentationWarningEmitted_) {
        push_console("Retained editor UI presentation is unavailable for the selected render backend");
        uiPresentationWarningEmitted_ = true;
    }
    (void)frame;
    return;
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
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Project", "Ctrl+B")) dispatch_command(EditorCommand::BuildProject, {}, world);
        if (ImGui::MenuItem("Cancel Build")) dispatch_command(EditorCommand::CancelBuild, {}, world);
        if (ImGui::MenuItem("Refresh Toolchains")) dispatch_command(EditorCommand::RefreshBuildTools, {}, world);
        if (ImGui::MenuItem("Import compile_commands.json")) dispatch_command(EditorCommand::ImportCompileCommands, {}, world);
        if (ImGui::MenuItem("Export compile_commands.json")) dispatch_command(EditorCommand::ExportCompileCommands, {}, world);
        if (ImGui::MenuItem("Open in Visual Studio")) dispatch_command(EditorCommand::OpenProjectInIde, "visual-studio", world);
        if (ImGui::MenuItem("Open in Rider")) dispatch_command(EditorCommand::OpenProjectInIde, "rider", world);
        if (ImGui::MenuItem("Open in VS Code")) dispatch_command(EditorCommand::OpenProjectInIde, "vscode", world);
        if (ImGui::MenuItem("Save Build Profile")) dispatch_command(EditorCommand::SaveBuildProfile, {}, world);
        if (ImGui::MenuItem("Reload Build Profile")) dispatch_command(EditorCommand::ReloadBuildProfile, {}, world);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        const auto menu = [this](const char* label, const char* id) {
            bool visible = panel_visible(id);
            if (ImGui::MenuItem(label, nullptr, &visible)) set_panel_visible(id, visible);
        };
        menu("Scene", "viewport"); menu("Game", "game"); menu("Hierarchy", "hierarchy");
        menu("Inspector", "inspector"); menu("Assets", "assets"); menu("Console", "console");
        menu("Profiler", "profiler"); menu("Render Graph", "render-graph"); menu("Build", "build"); menu("Settings", "settings");
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
        const char* panelNames[] = {"Hierarchy", "Inspector", "Scene", "Game", "Assets", "Console", "Profiler", "Render Graph", "Build"};
        const char* panelIds[] = {"hierarchy", "inspector", "viewport", "game", "assets", "console", "profiler", "render-graph", "build"};
        for (int index = 0; index < 9; ++index) {
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
    if (!imguiEnabled_ || !ImGui::GetCurrentContext()) return;
    sync_layout_to_style();
    ui::ImGuiApplyOptions options;
    options.projectRoot = fileSystem_.root();
    options.applyFont = false;
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
        const auto resolvedFont = ui::ImGuiAdapter::resolve_font_path(styleConfig_, options);
        const auto resolvedFallback = ui::ImGuiAdapter::resolve_fallback_font_path(styleConfig_, options);
        if (!resolvedFont.empty()) fontCandidates.emplace_back(resolvedFont);
        if (!resolvedFallback.empty() && resolvedFallback != resolvedFont)
            fontCandidates.emplace_back(resolvedFallback);
        for (auto fontPath : fontCandidates) {
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

void EditorLayer::shutdown(render::Renderer* renderer) {
    if (const char* path = std::getenv("SHINKOU_UI_METRICS_PATH")) ui::ui_performance().write_csv(path);
    if (!initialized_) return;
    if (fileScanFuture_.valid()) fileScanFuture_.wait();
    if (assetManifestFuture_.valid()) {
        assetManifestFuture_.wait();
        try { assetManifestFuture_.get(); } catch (...) { }
    }
    if (assetSystemPreviewFuture_.valid()) {
        assetSystemPreviewFuture_.wait();
        try { assetSystemPreviewFuture_.get(); } catch (...) { }
    }
    if (assetPreviewFuture_.valid()) {
        assetPreviewFuture_.wait();
        try { assetPreviewFuture_.get(); } catch (...) { }
    }
    if (imagePreviewFuture_.valid()) {
        imagePreviewFuture_.wait();
        try { imagePreviewFuture_.get(); } catch (...) { }
    }
    if (audioPreviewFuture_.valid()) {
        if (audioPreviewCancel_) audioPreviewCancel_->store(true, std::memory_order_relaxed);
        audioPreviewFuture_.wait();
        try { audioPreviewFuture_.get(); } catch (...) { }
    }
    if (videoPreviewFuture_.valid()) {
        if (videoPreviewCancel_) videoPreviewCancel_->store(true, std::memory_order_relaxed);
        videoPreviewFuture_.wait();
        try { videoPreviewFuture_.get(); } catch (...) { }
    }
    if (videoFrameFuture_.valid()) {
        if (videoFrameCancel_) videoFrameCancel_->store(true, std::memory_order_relaxed);
        videoFrameFuture_.wait();
        try { videoFrameFuture_.get(); } catch (...) { }
    }
    if (modelPreviewFuture_.valid()) {
        if (modelPreviewCancel_) modelPreviewCancel_->store(true, std::memory_order_relaxed);
        modelPreviewFuture_.wait();
        try { modelPreviewFuture_.get(); } catch (...) { }
    }
    if (modelTexturePreviewFuture_.valid()) {
        modelTexturePreviewFuture_.wait();
        try { modelTexturePreviewFuture_.get(); } catch (...) { }
    }
    reset_model_scene_assets();
    cancel_build();
    if (buildFuture_.valid()) {
        buildFuture_.wait();
        try { lastBuildResult_ = buildFuture_.get(); } catch (...) { }
    }
    liveBuildOutput_.reset();
    buildCancel_.reset();
    clear_audio_asset_bindings();
    if (renderer) modelSceneRenderer_.clear(*renderer);
    save_layout();
    uninstall_native_menu();
    editorUi_.shutdown();
#if defined(SHINKOU_WITH_IMGUI)
    if (imguiPlatformInitialized_) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        ImGui_ImplWin32_Shutdown();
#endif
        imguiPlatformInitialized_ = false;
    }
    if (uiContextOwned_ && ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
        uiContextOwned_ = false;
    }
    imguiEnabled_ = false;
#endif
    activeWorld_ = nullptr;
    assetSystem_ = nullptr;
    assetSystemRootNeedsRestart_ = false;
    initialized_ = false;
}

std::size_t EditorLayer::model_scene_loaded_asset_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [assetId, record] : modelSceneAssets_) {
        (void)assetId;
        if (record.active && record.snapshot && record.snapshot->valid()) ++count;
    }
    return count;
}

std::size_t EditorLayer::ui_command_count() const noexcept {
    return editorUi_.render_list().commands().size();
}

std::size_t EditorLayer::ui_text_command_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(editorUi_.render_list().commands().begin(),
        editorUi_.render_list().commands().end(), [](const ui::UiDrawCommand& command) {
            return command.type == ui::DrawCommandType::Text;
        }));
}

} // namespace shinkou::editor
