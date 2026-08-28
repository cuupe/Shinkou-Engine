#include "shinkou/uikit/EditorWorkspace.h"

#include <cassert>
#include <string>

using namespace shinkou::uikit;

namespace {

PanelDescriptor panel(std::string id, DockZone zone) {
    PanelDescriptor result;
    result.id = std::move(id);
    result.title = result.id + " panel";
    result.dockZone = zone;
    result.ratio = 0.3f;
    result.minimumSize = {220.0f, 120.0f};
    return result;
}

void test_state_xml_round_trip() {
    EditorWorkspaceState source;
    source.panels = {panel("hierarchy", DockZone::Left), panel("inspector", DockZone::Right), panel("viewport", DockZone::Center)};
    source.panels[1].visible = false;
    source.panels[2].locked = true;
    source.activeDocument = "assets/ui/main.ui.xml";
    source.resolution = {2560.0f, 1440.0f};
    source.dpiScale = 1.5f;
    source.uiScale = 1.15f;
    source.theme = "windows11-dark";
    source.font.family = "Microsoft YaHei";
    source.font.path = "C:/Windows/Fonts/msyh.ttc";
    source.font.size = 15.0f;
    source.font.weight = 500;
    source.font.italic = true;
    source.background.mode = BackgroundMode::Gradient;
    source.background.primary = Color::from_hex("#182233");
    source.background.secondary = Color::from_hex("#0b1020");
    source.background.imagePath = "assets/editor/background.png";
    source.background.opacity = 0.85f;
    assert(source.valid());

    const std::string xml = source.serialize_xml();
    assert(xml.find("shinkou-editor-workspace") != std::string::npos);
    assert(xml.find("Microsoft YaHei") != std::string::npos);
    assert(xml.find("dock=\"left\"") != std::string::npos);

    EditorWorkspaceState restored;
    std::string error;
    assert(deserialize_xml(xml, restored, &error));
    assert(restored.activeDocument == source.activeDocument);
    assert(restored.resolution.width == 2560.0f && restored.resolution.height == 1440.0f);
    assert(restored.dpiScale == 1.5f && restored.uiScale == 1.15f);
    assert(restored.theme == "windows11-dark" && restored.font.path == source.font.path);
    assert(restored.font.italic && restored.background.mode == BackgroundMode::Gradient);
    assert(restored.panels.size() == 3 && restored.panel("inspector")->visible == false);
    assert(restored.panel("viewport")->locked);

    const EditorWorkspaceState before = restored;
    assert(!deserialize_xml("<workspace schema=\"bad\" version=\"1\"/>", restored, &error));
    assert(restored.activeDocument == before.activeDocument && restored.panels.size() == before.panels.size());
    assert(!deserialize_xml("<workspace schema=\"shinkou-editor-workspace\" version=\"1\"><display width=\"nan\"/></workspace>", restored, &error));
    assert(restored.resolution.width == before.resolution.width);
}

void test_commands_and_history() {
    EditorWorkspace workspace;
    assert(workspace.add_panel(panel("hierarchy", DockZone::Left)));
    assert(workspace.add_panel(panel("inspector", DockZone::Right)));
    assert(workspace.add_panel(panel("viewport", DockZone::Center)));
    workspace.clear_history();
    assert(workspace.set_panel_visible("inspector", false));
    assert(!workspace.state().panel("inspector")->visible && workspace.can_undo());
    assert(workspace.undo());
    assert(workspace.state().panel("inspector")->visible && workspace.can_redo());
    assert(workspace.redo());
    assert(!workspace.state().panel("inspector")->visible);

    assert(workspace.set_panel_dock_zone("hierarchy", DockZone::Floating));
    assert(workspace.set_active_document("scene/main.scene"));
    assert(workspace.set_resolution({1920.0f, 1080.0f}));
    assert(workspace.set_dpi_scale(1.25f));
    assert(workspace.set_ui_scale(1.1f));
    assert(workspace.set_theme("windows11-dark"));
    FontSpec font; font.family = "Microsoft YaHei"; font.size = 16.0f; font.weight = 600;
    assert(workspace.set_font(font));
    WorkspaceBackground background; background.mode = BackgroundMode::Image; background.imagePath = "editor.png";
    assert(workspace.set_background(background));
    assert(workspace.state().panel("hierarchy")->dockZone == DockZone::Floating);
    assert(workspace.state().activeDocument == "scene/main.scene");
    assert(workspace.state().background.mode == BackgroundMode::Image);

    const std::size_t history = workspace.undo_count();
    assert(history >= 8);
    assert(workspace.execute(WorkspaceCommand::remove_panel("viewport")));
    assert(workspace.state().panel("viewport") == nullptr);
    assert(workspace.undo());
    assert(workspace.state().panel("viewport") != nullptr);
    assert(workspace.execute(WorkspaceCommand::set_panel_visibility("hierarchy", false)));
    assert(!workspace.can_redo());
    assert(!workspace.execute(WorkspaceCommand::set_panel_visibility("missing", false)));
    assert(!workspace.execute(WorkspaceCommand::set_dpi_scale(9.0f)));
}

void test_command_protocol_and_xml_load() {
    assert(std::string(workspace_command_name(WorkspaceCommandKind::SetFont)) == "set-font");
    assert(std::string(dock_zone_name(DockZone::Floating)) == "floating");
    assert(std::string(background_mode_name(BackgroundMode::Gradient)) == "gradient");
    DockZone zone = DockZone::Center;
    BackgroundMode mode = BackgroundMode::Solid;
    assert(parse_dock_zone("right", zone) && zone == DockZone::Right);
    assert(parse_background_mode("image", mode) && mode == BackgroundMode::Image);

    EditorWorkspace source;
    assert(source.add_panel(panel("assets", DockZone::Bottom)));
    assert(source.set_active_document("project.ui"));
    const std::string xml = source.serialize_xml();
    EditorWorkspace restored;
    assert(restored.load_xml(xml));
    assert(restored.state().panel("assets") != nullptr);
    assert(restored.state().activeDocument == "project.ui");
    assert(!restored.can_undo());
}

} // namespace

int main() {
    test_state_xml_round_trip();
    test_commands_and_history();
    test_command_protocol_and_xml_load();
    return 0;
}
