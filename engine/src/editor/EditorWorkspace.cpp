#include "EditorImGuiView.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorLocalization.h"
#include "shinkou/reflection/Serialization.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#endif
namespace shinkou::editor {
namespace {
void register_layout_types(reflection::TypeRegistry &registry) {
    std::string ignored;
    registry.register_type(
        reflection::TypeBuilder<EditorLayoutState>("shinkou.editor.EditorLayoutState")
            .field("layoutVersion", &EditorLayoutState::layoutVersion)
            .field("cameraStored", &EditorLayoutState::cameraStored)
            .field("editorCamera", &EditorLayoutState::editorCamera)
            .field("cameraFocusDistance", &EditorLayoutState::cameraFocusDistance)
            .field("cameraOrthographicSize", &EditorLayoutState::cameraOrthographicSize)
            .field("cameraViewMode", &EditorLayoutState::cameraViewMode)
            .field("transformTool", &EditorLayoutState::transformTool)
            .field("lastSceneFile", &EditorLayoutState::lastSceneFile)
            .field("theme", &EditorLayoutState::theme)
            .field("language", &EditorLayoutState::language)
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
            .field("showRecovery", &EditorLayoutState::showRecovery)
            .take(),
        &ignored);
}

ui::ThemeColor parse_hex_color(std::string_view value, ui::ThemeColor fallback) noexcept {
    if (value.size() != 7 || value.front() != '#')
        return fallback;
    unsigned result = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char character = value[index];
        const unsigned digit =
            character >= '0' && character <= '9'   ? static_cast<unsigned>(character - '0')
            : character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10)
            : character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10)
                                                   : 16u;
        if (digit > 15u)
            return fallback;
        result = (result << 4u) | digit;
    }
    return {((result >> 16u) & 0xffu) / 255.0f, ((result >> 8u) & 0xffu) / 255.0f,
            (result & 0xffu) / 255.0f, 1.0f};
}

std::string hex_color(ui::ThemeColor color) {
    const auto byte = [](float value) {
        return static_cast<unsigned>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    std::ostringstream output;
    output << '#' << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
           << byte(color.r) << std::setw(2) << byte(color.g) << std::setw(2) << byte(color.b);
    return output.str();
}

} // namespace
bool EditorLayer::save_layout_file() {
    if (viewportController_.initialized()) {
        layout_.cameraStored = true;
        layout_.editorCamera = viewportController_.pose();
        layout_.cameraFocusDistance = viewportController_.focus_distance();
        layout_.cameraOrthographicSize = viewportController_.camera().orthographicSize;
        layout_.cameraViewMode = renderViewMode_;
        layout_.transformTool = static_cast<std::int32_t>(viewportController_.tool());
    }
    if (sceneNamed_ || sceneDirty_)
        layout_.lastSceneFile = sceneNamed_ ? scenePath_ : std::string{};

    const std::filesystem::path path(layout_.layoutFile);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::string output;
    reflection::TypeRegistry registry;
    register_layout_types(registry);
    const auto result = reflection::serialize_json(
        registry, reflection::fnv1a("shinkou.editor.EditorLayoutState"), &layout_, output,
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
    if (!file)
        return false;
    std::stringstream input;
    input << file.rdbuf();
    reflection::TypeRegistry registry;
    register_layout_types(registry);
    EditorLayoutState loaded = layout_;
    const auto result = reflection::deserialize_json(
        registry, reflection::fnv1a("shinkou.editor.EditorLayoutState"), input.str(), &loaded,
        reflection::JsonOptions{false, false, false, 32});
    if (!result) {
        lastStatus_ = "Layout load failed: " + result.message;
        return false;
    }
    if (loaded.layoutVersion > 4) {
        lastStatus_ = "Layout load failed: unsupported layout version";
        return false;
    }
    if (loaded.layoutVersion == 0)
        loaded.layoutVersion = 1;
    loaded.layoutVersion = 4;
    if (!EditorLocalization::supported(loaded.language))
        loaded.language = "zh-CN";
    if (!std::isfinite(loaded.uiScale))
        loaded.uiScale = 1.0f;
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
    set_panel_visible("recovery", layout_.showRecovery);
    lastStatus_ = "Layout loaded: " + layout_.layoutFile;
    return true;
}

bool EditorLayer::save_workspace_file() {
    const std::filesystem::path path(layout_.dockLayoutFile);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    const auto temporaryPath = path.string() + ".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file << dockWorkspace_.to_json(true);
    file.flush();
    file.close();
    if (!file)
        return false;
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
    if (!file)
        return false;
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
    styleConfig_.uiScale =
        std::clamp(std::isfinite(layout_.uiScale) ? layout_.uiScale : 1.0f, 0.5f, 3.0f);
    styleConfig_.fontPath = layout_.fontPath;
    styleConfig_.fontSize =
        std::clamp(std::isfinite(layout_.fontSize) ? layout_.fontSize : 14.0f, 6.0f, 96.0f);
    styleConfig_.background.primary =
        parse_hex_color(layout_.backgroundColor, styleConfig_.background.primary);
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
    if (imguiView_ && uiContext_) {
        auto *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(uiContext_));
        ImGui::SaveIniSettingsToDisk(imguiPath.c_str());
        ImGui::SetCurrentContext(previous);
    }
#endif
    const bool savedLayout = save_layout_file();
    const bool savedDock = save_workspace_file();
    const bool savedStyle = save_style_file();
    if (!savedDock && savedLayout)
        lastStatus_ = "Layout saved, dock layout could not be committed";
    return savedLayout && savedDock && savedStyle;
}

bool EditorLayer::load_layout() {
    const auto project = layout_.projectRoot;
    const bool loaded = load_layout_file();
    layout_.projectRoot = project;
    load_style_file();
    sync_style_to_layout();
    const bool dockLoaded = load_workspace_file();
#if defined(SHINKOU_WITH_IMGUI)
    if (loaded && imguiView_ && uiContext_) {
        auto *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(uiContext_));
        ImGui::LoadIniSettingsFromDisk((layout_.layoutFile + ".imgui.ini").c_str());
        ImGui::SetCurrentContext(previous);
    }
#endif
    viewportController_ = EditorViewportController{};
    if (activeWorld_)
        ensure_viewport_camera(*activeWorld_);
    sync_workspace_visibility();
    return loaded && dockLoaded;
}

void EditorLayer::reset_layout() {
    const auto layoutPath = layout_.layoutFile, dockPath = layout_.dockLayoutFile,
               stylePath = layout_.styleFile, project = layout_.projectRoot;
    const auto selection = layout_.selectedObject;
    layout_ = EditorLayoutState{};
    layout_.layoutFile = layoutPath;
    layout_.dockLayoutFile = dockPath;
    layout_.styleFile = stylePath;
    layout_.projectRoot = project;
    layout_.selectedObject = selection;
    styleConfig_ = ui::UiStyleConfig{};
    sync_style_to_layout();
    themeRegistry_.switch_theme(layout_.theme);
    for (const auto &panel : panels_)
        panelVisibility_[panel.id] = panel.defaultVisible;
    build_default_workspace();
    sync_page_visibility();
    sync_workspace_visibility();
    if (imguiView_)
        imguiView_->reset_docking();
    lastStatus_ = "Layout reset";
}

} // namespace shinkou::editor
