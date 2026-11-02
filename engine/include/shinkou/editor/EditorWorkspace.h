#pragma once
#include "shinkou/Math.h"
#include "shinkou/Types.h"
#include <string>

namespace shinkou::editor {
struct EditorLayoutState {
    std::uint32_t layoutVersion{4};
    bool cameraStored{false};
    math::Transform editorCamera{};
    float cameraFocusDistance{5}, cameraOrthographicSize{10};
    std::int32_t transformTool{0};
    std::string cameraViewMode{"Perspective"}, lastSceneFile;
    std::string theme{"dark"};
    std::string language{"zh-CN"};
    std::string workspace{"Default"};
    std::string projectRoot{"."};
    std::string layoutFile{"Saved/Editor/Layouts/Default.json"};
    std::string dockLayoutFile{"Saved/Editor/Layouts/Default.dock.json"};
    std::string styleFile{"Saved/Editor/Styles/Default.json"};
    std::string fontPath{};
    float fontSize{15.0f};
    std::string backgroundColor{"#0e1117"};
    std::string backgroundImage{};
    bool compactControls{false};
    bool reduceMotion{false};
    ObjectId selectedObject{0};
    float uiScale{1.0f};
    bool allowDocking{true};
    bool showToolbar{true};
    bool showStatusBar{true};
    bool showHierarchy{true};
    bool showInspector{true};
    bool showViewport{true};
    bool showGame{false};
    bool showAssets{true};
    bool showConsole{true};
    bool showProfiler{false};
    bool showRenderGraph{false};
    bool showSettings{false};
    bool showMedia{false};
    bool showBuild{false};
    bool showRecovery{false};
};
} // namespace shinkou::editor
