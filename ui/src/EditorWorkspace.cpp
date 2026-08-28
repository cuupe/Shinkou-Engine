#include "shinkou/uikit/EditorWorkspace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace shinkou::uikit {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool finite(float value) noexcept { return std::isfinite(static_cast<double>(value)); }

bool valid_color(Color color) noexcept {
    return finite(color.r) && finite(color.g) && finite(color.b) && finite(color.a) &&
           color.r >= 0.0f && color.r <= 1.0f && color.g >= 0.0f && color.g <= 1.0f &&
           color.b >= 0.0f && color.b <= 1.0f && color.a >= 0.0f && color.a <= 1.0f;
}

bool valid_dock_zone(DockZone zone) noexcept {
    return zone >= DockZone::Left && zone <= DockZone::Floating;
}

bool valid_background_mode(BackgroundMode mode) noexcept {
    return mode >= BackgroundMode::Solid && mode <= BackgroundMode::Image;
}

bool parse_float(const std::string& source, float& value) {
    if (source.empty()) return false;
    char* end = nullptr;
    value = std::strtof(source.c_str(), &end);
    return end != source.c_str() && *end == '\0' && finite(value);
}

bool parse_int(const std::string& source, int& value) {
    if (source.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(source.c_str(), &end, 10);
    if (end == source.c_str() || *end != '\0' || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parse_bool(const std::string& source, bool& value) {
    if (source == "true" || source == "1" || source == "yes") { value = true; return true; }
    if (source == "false" || source == "0" || source == "no") { value = false; return true; }
    return false;
}

bool optional_float(const XmlNode& node, const char* name, float& value, std::string& error) {
    const std::string& source = node.attribute(name);
    if (source.empty()) return true;
    if (!parse_float(source, value)) { error = std::string("invalid workspace ") + name; return false; }
    return true;
}

bool optional_int(const XmlNode& node, const char* name, int& value, std::string& error) {
    const std::string& source = node.attribute(name);
    if (source.empty()) return true;
    if (!parse_int(source, value)) { error = std::string("invalid workspace ") + name; return false; }
    return true;
}

bool optional_bool(const XmlNode& node, const char* name, bool& value, std::string& error) {
    const std::string& source = node.attribute(name);
    if (source.empty()) return true;
    if (!parse_bool(source, value)) { error = std::string("invalid workspace ") + name; return false; }
    return true;
}

bool valid_hex(std::string_view source) noexcept {
    if (source.empty() || source.front() != '#' ||
        (source.size() != 4 && source.size() != 5 && source.size() != 7 && source.size() != 9)) return false;
    for (std::size_t index = 1; index < source.size(); ++index) {
        const char value = source[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'))) return false;
    }
    return true;
}

bool optional_color(const XmlNode& node, const char* name, Color& value, std::string& error) {
    const std::string& source = node.attribute(name);
    if (source.empty()) return true;
    if (!valid_hex(source)) { error = std::string("invalid workspace ") + name; return false; }
    value = Color::from_hex(source);
    return valid_color(value);
}

std::string color_hex(Color color) {
    std::ostringstream output;
    output << '#' << std::hex << std::setw(8) << std::setfill('0') << color.to_rgba8();
    return output.str();
}

bool read_panel(const XmlNode& node, PanelDescriptor& panel, std::string& error) {
    panel.id = node.attribute("id");
    panel.title = node.attribute("title", panel.id);
    if (panel.id.empty()) { error = "workspace panel id is missing"; return false; }
    if (!parse_dock_zone(node.attribute("dock", dock_zone_name(panel.dockZone)), panel.dockZone)) { error = "workspace panel dock zone is invalid"; return false; }
    if (!optional_bool(node, "visible", panel.visible, error) || !optional_bool(node, "closable", panel.closable, error) ||
        !optional_bool(node, "locked", panel.locked, error) || !optional_float(node, "ratio", panel.ratio, error) ||
        !optional_int(node, "order", panel.order, error) || !optional_float(node, "minimum-width", panel.minimumSize.width, error) ||
        !optional_float(node, "minimum-height", panel.minimumSize.height, error)) return false;
    return panel.valid(&error);
}

} // namespace

const char* dock_zone_name(DockZone zone) noexcept {
    switch (zone) {
    case DockZone::Left: return "left";
    case DockZone::Right: return "right";
    case DockZone::Top: return "top";
    case DockZone::Bottom: return "bottom";
    case DockZone::Center: return "center";
    case DockZone::Floating: return "floating";
    }
    return "center";
}

bool parse_dock_zone(std::string_view name, DockZone& zone) noexcept {
    if (name == "left") zone = DockZone::Left;
    else if (name == "right") zone = DockZone::Right;
    else if (name == "top") zone = DockZone::Top;
    else if (name == "bottom") zone = DockZone::Bottom;
    else if (name == "center") zone = DockZone::Center;
    else if (name == "floating") zone = DockZone::Floating;
    else return false;
    return true;
}

const char* background_mode_name(BackgroundMode mode) noexcept {
    switch (mode) {
    case BackgroundMode::Solid: return "solid";
    case BackgroundMode::Gradient: return "gradient";
    case BackgroundMode::Image: return "image";
    }
    return "solid";
}

bool parse_background_mode(std::string_view name, BackgroundMode& mode) noexcept {
    if (name == "solid") mode = BackgroundMode::Solid;
    else if (name == "gradient") mode = BackgroundMode::Gradient;
    else if (name == "image") mode = BackgroundMode::Image;
    else return false;
    return true;
}

bool PanelDescriptor::valid(std::string* error) const {
    if (id.empty() || id.size() > 128) { set_error(error, "workspace panel id is invalid"); return false; }
    if (title.size() > 1024) { set_error(error, "workspace panel title is too long"); return false; }
    if (!valid_dock_zone(dockZone) || !finite(ratio) || ratio < 0.0f || ratio > 1.0f || !finite(minimumSize.width) || !finite(minimumSize.height) ||
        minimumSize.width < 0.0f || minimumSize.height < 0.0f) { set_error(error, "workspace panel sizing is invalid"); return false; }
    if (error) error->clear();
    return true;
}

bool operator==(const PanelDescriptor& left, const PanelDescriptor& right) {
    return left.id == right.id && left.title == right.title && left.dockZone == right.dockZone &&
           left.visible == right.visible && left.closable == right.closable && left.locked == right.locked &&
           left.ratio == right.ratio && left.order == right.order && left.minimumSize.width == right.minimumSize.width &&
           left.minimumSize.height == right.minimumSize.height;
}

const PanelDescriptor* EditorWorkspaceState::panel(std::string_view id) const noexcept {
    const auto found = std::find_if(panels.begin(), panels.end(), [&](const PanelDescriptor& item) { return item.id == id; });
    return found == panels.end() ? nullptr : &*found;
}

PanelDescriptor* EditorWorkspaceState::panel(std::string_view id) noexcept {
    const auto found = std::find_if(panels.begin(), panels.end(), [&](const PanelDescriptor& item) { return item.id == id; });
    return found == panels.end() ? nullptr : &*found;
}

bool EditorWorkspaceState::valid(std::string* error) const {
    if (version != kEditorWorkspaceVersion) { set_error(error, "unsupported editor workspace version"); return false; }
    if (!finite(resolution.width) || !finite(resolution.height) || resolution.width <= 0.0f || resolution.height <= 0.0f ||
        resolution.width > 32768.0f || resolution.height > 32768.0f) { set_error(error, "workspace resolution is invalid"); return false; }
    if (!finite(dpiScale) || dpiScale < 0.25f || dpiScale > 8.0f || !finite(uiScale) || uiScale < 0.5f || uiScale > 4.0f) {
        set_error(error, "workspace DPI or UI scale is invalid"); return false;
    }
    if (theme.empty() || theme.size() > 128) { set_error(error, "workspace theme is invalid"); return false; }
    if (font.family.empty() || font.family.size() > 256 || !finite(font.size) || font.size < 6.0f || font.size > 96.0f || font.weight < 1 || font.weight > 1000) {
        set_error(error, "workspace font is invalid"); return false;
    }
    if (!valid_background_mode(background.mode) || !valid_color(background.primary) || !valid_color(background.secondary) || !finite(background.opacity) || background.opacity < 0.0f || background.opacity > 1.0f) {
        set_error(error, "workspace background is invalid"); return false;
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& item : panels) {
        if (!item.valid(error) || !ids.insert(item.id).second) { if (error && error->empty()) *error = "workspace panel ids must be unique"; return false; }
    }
    if (error) error->clear();
    return true;
}

XmlNode workspace_to_xml(const EditorWorkspaceState& state) {
    XmlNode root;
    if (!state.valid()) return root;
    root.name = "workspace";
    root.attributes["schema"] = kEditorWorkspaceSchema;
    root.attributes["version"] = std::to_string(state.version);

    XmlNode display; display.name = "display";
    display.attributes["width"] = std::to_string(state.resolution.width);
    display.attributes["height"] = std::to_string(state.resolution.height);
    display.attributes["dpi-scale"] = std::to_string(state.dpiScale);
    display.attributes["ui-scale"] = std::to_string(state.uiScale);
    root.children.push_back(std::move(display));

    XmlNode document; document.name = "document"; document.attributes["path"] = state.activeDocument;
    root.children.push_back(std::move(document));
    XmlNode theme; theme.name = "theme"; theme.attributes["id"] = state.theme;
    root.children.push_back(std::move(theme));

    XmlNode font; font.name = "font";
    font.attributes["family"] = state.font.family; font.attributes["path"] = state.font.path;
    font.attributes["size"] = std::to_string(state.font.size); font.attributes["weight"] = std::to_string(state.font.weight);
    font.attributes["italic"] = state.font.italic ? "true" : "false";
    root.children.push_back(std::move(font));

    XmlNode background; background.name = "background";
    background.attributes["mode"] = background_mode_name(state.background.mode);
    background.attributes["primary"] = color_hex(state.background.primary);
    background.attributes["secondary"] = color_hex(state.background.secondary);
    background.attributes["image"] = state.background.imagePath;
    background.attributes["opacity"] = std::to_string(state.background.opacity);
    root.children.push_back(std::move(background));

    XmlNode panels; panels.name = "panels";
    for (const auto& descriptor : state.panels) {
        XmlNode panel; panel.name = "panel";
        panel.attributes["id"] = descriptor.id; panel.attributes["title"] = descriptor.title;
        panel.attributes["dock"] = dock_zone_name(descriptor.dockZone);
        panel.attributes["visible"] = descriptor.visible ? "true" : "false";
        panel.attributes["closable"] = descriptor.closable ? "true" : "false";
        panel.attributes["locked"] = descriptor.locked ? "true" : "false";
        panel.attributes["ratio"] = std::to_string(descriptor.ratio);
        panel.attributes["order"] = std::to_string(descriptor.order);
        panel.attributes["minimum-width"] = std::to_string(descriptor.minimumSize.width);
        panel.attributes["minimum-height"] = std::to_string(descriptor.minimumSize.height);
        panels.children.push_back(std::move(panel));
    }
    root.children.push_back(std::move(panels));
    return root;
}

XmlNode EditorWorkspaceState::to_xml() const { return workspace_to_xml(*this); }

std::string serialize_xml(const EditorWorkspaceState& state) {
    XmlDocument document;
    document.set_root(workspace_to_xml(state));
    return document.serialize();
}

std::string EditorWorkspaceState::serialize_xml() const { return shinkou::uikit::serialize_xml(*this); }

bool deserialize_xml(const XmlNode& node, EditorWorkspaceState& state, std::string* error) {
    if (node.name != "workspace" || node.attribute("schema") != kEditorWorkspaceSchema) {
        set_error(error, "unsupported editor workspace XML schema"); return false;
    }
    EditorWorkspaceState parsed = state;
    int version = kEditorWorkspaceVersion;
    if (!node.attribute("version").empty() && !parse_int(node.attribute("version"), version)) { set_error(error, "invalid editor workspace version"); return false; }
    parsed.version = version;
    std::string parseError;
    if (const XmlNode* display = node.child("display")) {
        if (!optional_float(*display, "width", parsed.resolution.width, parseError) || !optional_float(*display, "height", parsed.resolution.height, parseError) ||
            !optional_float(*display, "dpi-scale", parsed.dpiScale, parseError) || !optional_float(*display, "ui-scale", parsed.uiScale, parseError)) { set_error(error, std::move(parseError)); return false; }
    }
    if (const XmlNode* document = node.child("document")) parsed.activeDocument = document->attribute("path", parsed.activeDocument);
    if (const XmlNode* theme = node.child("theme")) parsed.theme = theme->attribute("id", parsed.theme);
    if (const XmlNode* font = node.child("font")) {
        parsed.font.family = font->attribute("family", parsed.font.family);
        parsed.font.path = font->attribute("path", parsed.font.path);
        if (!optional_float(*font, "size", parsed.font.size, parseError) || !optional_int(*font, "weight", parsed.font.weight, parseError) || !optional_bool(*font, "italic", parsed.font.italic, parseError)) { set_error(error, std::move(parseError)); return false; }
    }
    if (const XmlNode* background = node.child("background")) {
        if (!background->attribute("mode").empty() && !parse_background_mode(background->attribute("mode"), parsed.background.mode)) { set_error(error, "invalid workspace background mode"); return false; }
        if (!optional_color(*background, "primary", parsed.background.primary, parseError) || !optional_color(*background, "secondary", parsed.background.secondary, parseError) ||
            !optional_float(*background, "opacity", parsed.background.opacity, parseError)) { set_error(error, std::move(parseError)); return false; }
        parsed.background.imagePath = background->attribute("image", parsed.background.imagePath);
    }
    if (const XmlNode* panels = node.child("panels")) {
        parsed.panels.clear();
        for (const XmlNode* panelNode : panels->children_named("panel")) {
            PanelDescriptor descriptor;
            if (!read_panel(*panelNode, descriptor, parseError)) { set_error(error, std::move(parseError)); return false; }
            parsed.panels.push_back(std::move(descriptor));
        }
    }
    if (!parsed.valid(&parseError)) { set_error(error, std::move(parseError)); return false; }
    state = std::move(parsed);
    if (error) error->clear();
    return true;
}

bool deserialize_xml(const std::string& source, EditorWorkspaceState& state, std::string* error) {
    XmlDocument document;
    if (!document.parse(source, error) || !document.root()) return false;
    return deserialize_xml(*document.root(), state, error);
}

const char* workspace_command_name(WorkspaceCommandKind kind) noexcept {
    switch (kind) {
    case WorkspaceCommandKind::SetPanelVisibility: return "set-panel-visibility";
    case WorkspaceCommandKind::SetPanelDockZone: return "set-panel-dock-zone";
    case WorkspaceCommandKind::AddPanel: return "add-panel";
    case WorkspaceCommandKind::RemovePanel: return "remove-panel";
    case WorkspaceCommandKind::SetActiveDocument: return "set-active-document";
    case WorkspaceCommandKind::SetResolution: return "set-resolution";
    case WorkspaceCommandKind::SetDpiScale: return "set-dpi-scale";
    case WorkspaceCommandKind::SetUiScale: return "set-ui-scale";
    case WorkspaceCommandKind::SetTheme: return "set-theme";
    case WorkspaceCommandKind::SetFont: return "set-font";
    case WorkspaceCommandKind::SetBackground: return "set-background";
    }
    return "unknown";
}

WorkspaceCommand WorkspaceCommand::set_panel_visibility(std::string panelId, bool visible) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetPanelVisibility; command.panelId = std::move(panelId); command.boolValue = visible; return command;
}

WorkspaceCommand WorkspaceCommand::set_panel_dock_zone(std::string panelId, DockZone zone) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetPanelDockZone; command.panelId = std::move(panelId); command.dockZone = zone; return command;
}

WorkspaceCommand WorkspaceCommand::add_panel(PanelDescriptor panel) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::AddPanel; command.panelId = panel.id; command.panelValue = std::move(panel); return command;
}

WorkspaceCommand WorkspaceCommand::remove_panel(std::string panelId) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::RemovePanel; command.panelId = std::move(panelId); return command;
}

WorkspaceCommand WorkspaceCommand::set_active_document(std::string document) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetActiveDocument; command.stringValue = std::move(document); return command;
}

WorkspaceCommand WorkspaceCommand::set_resolution(Size resolution) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetResolution; command.sizeValue = resolution; return command;
}

WorkspaceCommand WorkspaceCommand::set_dpi_scale(float scale) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetDpiScale; command.floatValue = scale; return command;
}

WorkspaceCommand WorkspaceCommand::set_ui_scale(float scale) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetUiScale; command.floatValue = scale; return command;
}

WorkspaceCommand WorkspaceCommand::set_theme(std::string theme) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetTheme; command.stringValue = std::move(theme); return command;
}

WorkspaceCommand WorkspaceCommand::set_font(FontSpec font) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetFont; command.fontValue = std::move(font); return command;
}

WorkspaceCommand WorkspaceCommand::set_background(WorkspaceBackground background) {
    WorkspaceCommand command; command.kind = WorkspaceCommandKind::SetBackground; command.backgroundValue = std::move(background); return command;
}

EditorWorkspace::EditorWorkspace(EditorWorkspaceState state) : state_(std::move(state)) {
    if (!state_.valid()) state_ = EditorWorkspaceState{};
}

bool EditorWorkspace::apply(const WorkspaceCommand& command, WorkspaceCommand* inverse) {
    EditorWorkspaceState candidate = state_;
    WorkspaceCommand undoCommand;
    bool validCommand = true;
    switch (command.kind) {
    case WorkspaceCommandKind::SetPanelVisibility: {
        auto* panel = candidate.panel(command.panelId);
        const auto* current = state_.panel(command.panelId);
        if (!panel || !current) { validCommand = false; break; }
        undoCommand = WorkspaceCommand::set_panel_visibility(command.panelId, current->visible);
        panel->visible = command.boolValue;
        break;
    }
    case WorkspaceCommandKind::SetPanelDockZone: {
        auto* panel = candidate.panel(command.panelId);
        const auto* current = state_.panel(command.panelId);
        if (!panel || !current || !valid_dock_zone(command.dockZone)) { validCommand = false; break; }
        undoCommand = WorkspaceCommand::set_panel_dock_zone(command.panelId, current->dockZone);
        panel->dockZone = command.dockZone;
        break;
    }
    case WorkspaceCommandKind::AddPanel:
        if (!command.panelValue.valid() || state_.panel(command.panelValue.id)) { validCommand = false; break; }
        undoCommand = WorkspaceCommand::remove_panel(command.panelValue.id);
        candidate.panels.push_back(command.panelValue);
        break;
    case WorkspaceCommandKind::RemovePanel: {
        const auto* current = state_.panel(command.panelId);
        if (!current) { validCommand = false; break; }
        undoCommand = WorkspaceCommand::add_panel(*current);
        candidate.panels.erase(std::remove_if(candidate.panels.begin(), candidate.panels.end(), [&](const PanelDescriptor& item) { return item.id == command.panelId; }), candidate.panels.end());
        break;
    }
    case WorkspaceCommandKind::SetActiveDocument:
        undoCommand = WorkspaceCommand::set_active_document(state_.activeDocument);
        candidate.activeDocument = command.stringValue;
        break;
    case WorkspaceCommandKind::SetResolution:
        undoCommand = WorkspaceCommand::set_resolution(state_.resolution);
        candidate.resolution = command.sizeValue;
        break;
    case WorkspaceCommandKind::SetDpiScale:
        undoCommand = WorkspaceCommand::set_dpi_scale(state_.dpiScale);
        candidate.dpiScale = command.floatValue;
        break;
    case WorkspaceCommandKind::SetUiScale:
        undoCommand = WorkspaceCommand::set_ui_scale(state_.uiScale);
        candidate.uiScale = command.floatValue;
        break;
    case WorkspaceCommandKind::SetTheme:
        undoCommand = WorkspaceCommand::set_theme(state_.theme);
        candidate.theme = command.stringValue;
        break;
    case WorkspaceCommandKind::SetFont:
        undoCommand = WorkspaceCommand::set_font(state_.font);
        candidate.font = command.fontValue;
        break;
    case WorkspaceCommandKind::SetBackground:
        undoCommand = WorkspaceCommand::set_background(state_.background);
        candidate.background = command.backgroundValue;
        break;
    }
    if (!validCommand || !candidate.valid()) return false;
    state_ = std::move(candidate);
    if (inverse) *inverse = std::move(undoCommand);
    return true;
}

bool EditorWorkspace::execute(const WorkspaceCommand& command) {
    WorkspaceCommand inverse;
    if (!apply(command, &inverse)) return false;
    undo_.push_back({command, std::move(inverse)});
    redo_.clear();
    return true;
}

bool EditorWorkspace::undo() {
    if (undo_.empty()) return false;
    HistoryEntry entry = std::move(undo_.back());
    if (!apply(entry.inverse, nullptr)) return false;
    undo_.pop_back();
    redo_.push_back(std::move(entry));
    return true;
}

bool EditorWorkspace::redo() {
    if (redo_.empty()) return false;
    HistoryEntry entry = std::move(redo_.back());
    if (!apply(entry.command, nullptr)) return false;
    redo_.pop_back();
    undo_.push_back(std::move(entry));
    return true;
}

void EditorWorkspace::clear_history() noexcept {
    undo_.clear();
    redo_.clear();
}

bool EditorWorkspace::set_panel_visible(std::string_view panelId, bool visible) { return execute(WorkspaceCommand::set_panel_visibility(std::string(panelId), visible)); }
bool EditorWorkspace::set_panel_dock_zone(std::string_view panelId, DockZone zone) { return execute(WorkspaceCommand::set_panel_dock_zone(std::string(panelId), zone)); }
bool EditorWorkspace::add_panel(PanelDescriptor panel) { return execute(WorkspaceCommand::add_panel(std::move(panel))); }
bool EditorWorkspace::remove_panel(std::string_view panelId) { return execute(WorkspaceCommand::remove_panel(std::string(panelId))); }
bool EditorWorkspace::set_active_document(std::string document) { return execute(WorkspaceCommand::set_active_document(std::move(document))); }
bool EditorWorkspace::set_resolution(Size resolution) { return execute(WorkspaceCommand::set_resolution(resolution)); }
bool EditorWorkspace::set_dpi_scale(float scale) { return execute(WorkspaceCommand::set_dpi_scale(scale)); }
bool EditorWorkspace::set_ui_scale(float scale) { return execute(WorkspaceCommand::set_ui_scale(scale)); }
bool EditorWorkspace::set_theme(std::string theme) { return execute(WorkspaceCommand::set_theme(std::move(theme))); }
bool EditorWorkspace::set_font(FontSpec font) { return execute(WorkspaceCommand::set_font(std::move(font))); }
bool EditorWorkspace::set_background(WorkspaceBackground background) { return execute(WorkspaceCommand::set_background(std::move(background))); }

bool EditorWorkspace::load_xml(const std::string& source, std::string* error) {
    EditorWorkspaceState parsed = state_;
    if (!deserialize_xml(source, parsed, error)) return false;
    state_ = std::move(parsed);
    clear_history();
    return true;
}

} // namespace shinkou::uikit
