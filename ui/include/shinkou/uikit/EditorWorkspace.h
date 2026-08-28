#pragma once

#include "Style.h"
#include "Xml.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::uikit {

inline constexpr int kEditorWorkspaceVersion = 1;
inline constexpr const char* kEditorWorkspaceSchema = "shinkou-editor-workspace";

enum class DockZone : std::uint8_t { Left, Right, Top, Bottom, Center, Floating };
const char* dock_zone_name(DockZone zone) noexcept;
bool parse_dock_zone(std::string_view name, DockZone& zone) noexcept;

enum class BackgroundMode : std::uint8_t { Solid, Gradient, Image };
const char* background_mode_name(BackgroundMode mode) noexcept;
bool parse_background_mode(std::string_view name, BackgroundMode& mode) noexcept;

struct WorkspaceBackground {
    BackgroundMode mode = BackgroundMode::Solid;
    Color primary = Color::from_hex("#202020");
    Color secondary = Color::from_hex("#2b2b2b");
    std::string imagePath;
    float opacity = 1.0f;
};

struct PanelDescriptor {
    std::string id;
    std::string title;
    DockZone dockZone = DockZone::Center;
    bool visible = true;
    bool closable = true;
    bool locked = false;
    float ratio = 0.25f;
    int order = 0;
    Size minimumSize{160.0f, 80.0f};

    bool valid(std::string* error = nullptr) const;
    friend bool operator==(const PanelDescriptor& left, const PanelDescriptor& right);
};

struct EditorWorkspaceState {
    int version = kEditorWorkspaceVersion;
    std::vector<PanelDescriptor> panels;
    std::string activeDocument;
    Size resolution{1280.0f, 720.0f};
    float dpiScale = 1.0f;
    float uiScale = 1.0f;
    std::string theme = "windows11";
    FontSpec font{};
    WorkspaceBackground background{};

    const PanelDescriptor* panel(std::string_view id) const noexcept;
    PanelDescriptor* panel(std::string_view id) noexcept;
    bool valid(std::string* error = nullptr) const;
    XmlNode to_xml() const;
    std::string serialize_xml() const;
};

enum class WorkspaceCommandKind : std::uint8_t {
    SetPanelVisibility,
    SetPanelDockZone,
    AddPanel,
    RemovePanel,
    SetActiveDocument,
    SetResolution,
    SetDpiScale,
    SetUiScale,
    SetTheme,
    SetFont,
    SetBackground
};

const char* workspace_command_name(WorkspaceCommandKind kind) noexcept;

// A serializable command payload. Only the field associated with `kind` is used.
// Keeping commands as data makes them suitable for WPF, scripting, or a future
// remote editor protocol without capturing C++ lambdas.
struct WorkspaceCommand {
    WorkspaceCommandKind kind = WorkspaceCommandKind::SetActiveDocument;
    std::string panelId;
    bool boolValue = false;
    DockZone dockZone = DockZone::Center;
    std::string stringValue;
    Size sizeValue{};
    float floatValue = 1.0f;
    FontSpec fontValue{};
    WorkspaceBackground backgroundValue{};
    PanelDescriptor panelValue{};

    static WorkspaceCommand set_panel_visibility(std::string panelId, bool visible);
    static WorkspaceCommand set_panel_visible(std::string panelId, bool visible) {
        return set_panel_visibility(std::move(panelId), visible);
    }
    static WorkspaceCommand set_panel_dock_zone(std::string panelId, DockZone zone);
    static WorkspaceCommand add_panel(PanelDescriptor panel);
    static WorkspaceCommand remove_panel(std::string panelId);
    static WorkspaceCommand set_active_document(std::string document);
    static WorkspaceCommand set_resolution(Size resolution);
    static WorkspaceCommand set_dpi_scale(float scale);
    static WorkspaceCommand set_ui_scale(float scale);
    static WorkspaceCommand set_theme(std::string theme);
    static WorkspaceCommand set_font(FontSpec font);
    static WorkspaceCommand set_background(WorkspaceBackground background);
};

class EditorWorkspace final {
public:
    explicit EditorWorkspace(EditorWorkspaceState state = {});

    const EditorWorkspaceState& state() const noexcept { return state_; }
    EditorWorkspaceState& state() noexcept { return state_; }

    bool execute(const WorkspaceCommand& command);
    bool undo();
    bool redo();
    bool can_undo() const noexcept { return !undo_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }
    std::size_t undo_count() const noexcept { return undo_.size(); }
    std::size_t redo_count() const noexcept { return redo_.size(); }
    void clear_history() noexcept;

    bool set_panel_visible(std::string_view panelId, bool visible);
    bool set_panel_dock_zone(std::string_view panelId, DockZone zone);
    bool add_panel(PanelDescriptor panel);
    bool remove_panel(std::string_view panelId);
    bool set_active_document(std::string document);
    bool set_resolution(Size resolution);
    bool set_dpi_scale(float scale);
    bool set_ui_scale(float scale);
    bool set_theme(std::string theme);
    bool set_font(FontSpec font);
    bool set_background(WorkspaceBackground background);

    XmlNode to_xml() const { return state_.to_xml(); }
    std::string serialize_xml() const { return state_.serialize_xml(); }
    bool load_xml(const std::string& source, std::string* error = nullptr);

private:
    struct HistoryEntry {
        WorkspaceCommand command;
        WorkspaceCommand inverse;
    };

    EditorWorkspaceState state_;
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;

    bool apply(const WorkspaceCommand& command, WorkspaceCommand* inverse);
};

XmlNode workspace_to_xml(const EditorWorkspaceState& state);
std::string serialize_xml(const EditorWorkspaceState& state);
bool deserialize_xml(const XmlNode& node, EditorWorkspaceState& state, std::string* error = nullptr);
bool deserialize_xml(const std::string& source, EditorWorkspaceState& state, std::string* error = nullptr);

} // namespace shinkou::uikit
