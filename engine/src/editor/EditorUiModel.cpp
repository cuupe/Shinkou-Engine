#include "shinkou/editor/EditorUiModel.h"

#include "shinkou/GameObject.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorDocument.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace shinkou::editor {
namespace {

bool matches_filter(std::string_view name, std::string_view filter) {
    if (filter.empty()) return true;
    if (name.size() < filter.size()) return false;
    for (std::size_t offset = 0; offset + filter.size() <= name.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < filter.size(); ++index) {
            const auto lhs = static_cast<unsigned char>(name[offset + index]);
            const auto rhs = static_cast<unsigned char>(filter[index]);
            if (std::tolower(lhs) != std::tolower(rhs)) { matches = false; break; }
        }
        if (matches) return true;
    }
    return false;
}

EditorMenuItemModel item(std::string id, std::string label, EditorCommand command,
                         std::string shortcut = {}, std::string target = {}) {
    return {std::move(id), std::move(label), std::move(shortcut), std::move(target), command, false, command != EditorCommand::None};
}

EditorMenuItemModel separator() {
    EditorMenuItemModel result;
    result.separator = true;
    return result;
}

std::shared_ptr<const std::vector<EditorInspectorChoice>> audio_bus_choices() {
    static const auto choices = std::make_shared<const std::vector<EditorInspectorChoice>>(
        std::vector<EditorInspectorChoice>{
            {"0", "Master", 0},
            {"1", "Music", 0},
            {"2", "SFX", 0},
            {"3", "Voice", 0},
            {"4", "Ambient", 0},
            {"5", "UI", 0},
        });
    return choices;
}

std::string audio_source_status(const components::AudioSourceComponent& source,
                                const std::shared_ptr<const std::vector<EditorInspectorChoice>>& choices,
                                std::string_view fallback) {
    if (source.assetId == 0) {
        return source.clipPath.empty() ? "No audio clip selected" : "Path-only audio · identity not assigned";
    }
    if (!choices || choices->empty()) return std::string(fallback);
    const auto found = std::find_if(choices->begin(), choices->end(), [&](const auto& choice) {
        return choice.assetId == source.assetId;
    });
    if (found == choices->end()) return "AssetId unavailable in current manifest";
    if (found->value != source.clipPath) return "Path/AssetId mismatch · " + found->value;
    return "Ready · " + found->value;
}

bool same_build_state(const EditorBuildUiState& left, const EditorBuildUiState& right) {
    if (left.profileName != right.profileName || left.selectedProfileId != right.selectedProfileId ||
        left.profileStatus != right.profileStatus ||
        left.status != right.status || left.ideStatus != right.ideStatus || left.running != right.running ||
        left.compileCommandCount != right.compileCommandCount || left.compileCommandsStatus != right.compileCommandsStatus ||
        left.diagnosticFilter != right.diagnosticFilter ||
        left.projectStatus != right.projectStatus || left.recommendedProjectFile != right.recommendedProjectFile ||
        left.clangdStatus != right.clangdStatus ||
        left.diagnosticTotalCount != right.diagnosticTotalCount ||
        left.diagnosticErrorCount != right.diagnosticErrorCount ||
        left.diagnosticWarningCount != right.diagnosticWarningCount ||
        left.diagnosticNoteCount != right.diagnosticNoteCount ||
        left.selectedDiagnostic != right.selectedDiagnostic ||
        left.profileIds != right.profileIds || left.profileFields.size() != right.profileFields.size() ||
        left.tools.size() != right.tools.size() || left.ides.size() != right.ides.size() ||
        left.projectFiles.size() != right.projectFiles.size() ||
        left.diagnostics.size() != right.diagnostics.size() ||
        left.outputLines != right.outputLines) return false;
    for (std::size_t index = 0; index < left.profileFields.size(); ++index) {
        const auto& a = left.profileFields[index];
        const auto& b = right.profileFields[index];
        if (a.id != b.id || a.label != b.label || a.value != b.value ||
            a.editable != b.editable || a.boolean != b.boolean) return false;
    }
    for (std::size_t index = 0; index < left.tools.size(); ++index) {
        const auto& a = left.tools[index];
        const auto& b = right.tools[index];
        if (a.name != b.name || a.executable != b.executable || a.available != b.available) return false;
    }
    for (std::size_t index = 0; index < left.ides.size(); ++index) {
        const auto& a = left.ides[index];
        const auto& b = right.ides[index];
        if (a.name != b.name || a.executable != b.executable || a.available != b.available) return false;
    }
    for (std::size_t index = 0; index < left.projectFiles.size(); ++index) {
        const auto& a = left.projectFiles[index];
        const auto& b = right.projectFiles[index];
        if (a.kind != b.kind || a.path != b.path || a.recommended != b.recommended) return false;
    }
    for (std::size_t index = 0; index < left.diagnostics.size(); ++index) {
        const auto& a = left.diagnostics[index];
        const auto& b = right.diagnostics[index];
        if (a.severity != b.severity || a.file != b.file || a.line != b.line || a.column != b.column ||
            a.code != b.code || a.message != b.message) return false;
    }
    return true;
}

bool same_asset_preview(const EditorAssetPreviewUiState& left, const EditorAssetPreviewUiState& right) {
    return left.path == right.path && left.kind == right.kind && left.title == right.title &&
        left.status == right.status && left.loading == right.loading && left.truncated == right.truncated &&
        left.textLines == right.textLines && left.imageWidth == right.imageWidth &&
        left.imageHeight == right.imageHeight && left.imageSnapshot == right.imageSnapshot &&
        left.modelPreview == right.modelPreview && left.modelPreviewScene == right.modelPreviewScene &&
        left.modelTextureWidth == right.modelTextureWidth &&
        left.modelTextureHeight == right.modelTextureHeight &&
        left.modelTextureSnapshot == right.modelTextureSnapshot &&
        left.modelTextureStatus == right.modelTextureStatus &&
        left.modelMaterialIndex == right.modelMaterialIndex &&
        left.modelTextureIndex == right.modelTextureIndex &&
        left.modelTextureImageIndex == right.modelTextureImageIndex &&
        left.modelMaterialLabel == right.modelMaterialLabel &&
        left.modelTextureLabel == right.modelTextureLabel &&
        left.modelTextureRole == right.modelTextureRole &&
        left.modelGpuPreviewReady == right.modelGpuPreviewReady &&
        left.modelGpuMaterialApplied == right.modelGpuMaterialApplied &&
        left.modelGpuMaterialFactorsApplied == right.modelGpuMaterialFactorsApplied &&
        left.modelGpuTextureRoleApplied == right.modelGpuTextureRoleApplied &&
        left.modelGpuBaseColorTextureSampled == right.modelGpuBaseColorTextureSampled &&
        left.modelGpuNormalTextureSampled == right.modelGpuNormalTextureSampled &&
        left.modelGpuMetallicRoughnessTextureSampled == right.modelGpuMetallicRoughnessTextureSampled &&
        left.modelGpuTextureSampled == right.modelGpuTextureSampled &&
        left.modelGpuOffscreenTargetReady == right.modelGpuOffscreenTargetReady &&
        left.modelGpuOffscreenCompositeApplied == right.modelGpuOffscreenCompositeApplied &&
        left.modelGpuPreviewStatus == right.modelGpuPreviewStatus &&
        left.assetSystemStatus == right.assetSystemStatus &&
        left.assetSystemFormat == right.assetSystemFormat &&
        left.assetSystemMetadataFormat == right.assetSystemMetadataFormat &&
        left.assetSystemMetadataBytes == right.assetSystemMetadataBytes &&
        left.assetSystemSourceHash == right.assetSystemSourceHash &&
        left.assetSystemLoading == right.assetSystemLoading &&
        left.assetSystemReady == right.assetSystemReady;
}

bool same_media_state(const EditorMediaUiState& left, const EditorMediaUiState& right) {
    return left.path == right.path && left.kind == right.kind && left.status == right.status &&
        left.playbackState == right.playbackState && left.currentTime == right.currentTime &&
        left.duration == right.duration && left.volume == right.volume &&
        left.available == right.available && left.loop == right.loop &&
        left.previewLoading == right.previewLoading && left.previewStatus == right.previewStatus &&
        left.audioPreview == right.audioPreview && left.videoPreview == right.videoPreview;
}

} // namespace

EditorUiModel::EditorUiModel() {
    build_default_pages();
    build_default_menus();
}

void EditorUiModel::build_default_pages() {
    pages_ = {
        {"scene", "Scene", "", true, true},
        {"game", "Game", "Ctrl+G", false, true},
        {"hierarchy", "Hierarchy", "", true, false},
        {"inspector", "Inspector", "", true, false},
        {"project", "Project", "", true, false},
        {"console", "Console", "Ctrl+Shift+C", true, false},
        {"profiler", "Profiler", "", false, false},
        {"render-graph", "Render Graph", "", false, false},
        {"build", "Build", "Ctrl+B", false, false},
        {"media", "Media Preview", "", false, false},
        {"settings", "Project Settings", "", false, false},
    };
}

void EditorUiModel::build_default_menus() {
    menus_ = {
        {"file", "File", {
            item("new-scene", "New Scene", EditorCommand::NewScene, "Ctrl+N"),
            item("open-scene", "Open Scene...", EditorCommand::OpenScene, "Ctrl+O"),
            item("save-scene", "Save Scene", EditorCommand::SaveScene, "Ctrl+S"),
            item("save-scene-as", "Save Scene As...", EditorCommand::SaveSceneAs, "Ctrl+Shift+S"),
            separator(),
            item("save-layout", "Save Layout", EditorCommand::SaveLayout),
            item("reload-layout", "Reload Layout", EditorCommand::ReloadLayout),
            separator(),
            item("quit", "Exit", EditorCommand::Quit),
        }},
        {"edit", "Edit", {
            item("undo", "Undo", EditorCommand::Undo, "Ctrl+Z"),
            item("redo", "Redo", EditorCommand::Redo, "Ctrl+Y"),
            item("delete-object", "Delete Object", EditorCommand::DeleteObject, "Delete"),
            separator(),
            item("frame-selection", "Frame Selected", EditorCommand::FrameSelection, "F"),
            item("project-settings", "Project Settings", EditorCommand::ProjectSettings),
        }},
        {"assets", "Assets", {
            item("create-asset", "Create Asset", EditorCommand::None),
            item("import-asset", "Import New Asset...", EditorCommand::None),
            item("refresh-assets", "Refresh", EditorCommand::RefreshAssets),
        }},
        {"build", "Build", {
            item("build-project", "Build Project", EditorCommand::BuildProject, "Ctrl+B"),
            item("cancel-build", "Cancel Build", EditorCommand::CancelBuild),
            item("refresh-build-tools", "Refresh Toolchains", EditorCommand::RefreshBuildTools),
            item("refresh-project-files", "Discover Project Files", EditorCommand::RefreshProjectFiles),
            item("generate-clangd-config", "Generate .clangd", EditorCommand::GenerateClangdConfig),
            item("import-compile-commands", "Import compile_commands.json", EditorCommand::ImportCompileCommands),
            item("export-compile-commands", "Export compile_commands.json", EditorCommand::ExportCompileCommands),
            separator(),
            item("open-visual-studio", "Open in Visual Studio", EditorCommand::OpenProjectInIde, {}, "visual-studio"),
            item("open-rider", "Open in Rider", EditorCommand::OpenProjectInIde, {}, "rider"),
            item("open-vscode", "Open in VS Code", EditorCommand::OpenProjectInIde, {}, "vscode"),
            item("save-build-profile", "Save Build Profile", EditorCommand::SaveBuildProfile),
            item("reload-build-profile", "Reload Build Profile", EditorCommand::ReloadBuildProfile),
        }},
        {"game-object", "GameObject", {
            item("create-empty", "Create Empty", EditorCommand::CreateEmpty, "Ctrl+Shift+N"),
            item("create-child", "Create Child", EditorCommand::CreateChild),
            separator(),
            item("create-3d", "3D Object (factory unavailable)", EditorCommand::None),
            item("create-2d", "2D Object (factory unavailable)", EditorCommand::None),
            item("create-ui", "UI Object (factory unavailable)", EditorCommand::None),
        }},
        {"component", "Component", {
            item("add-component", "Add Component...", EditorCommand::AddComponent),
        }},
        {"window", "Window", {
            item("scene", "Scene", EditorCommand::TogglePage, {}, "scene"),
            item("game", "Game", EditorCommand::TogglePage, {}, "game"),
            item("hierarchy", "Hierarchy", EditorCommand::TogglePage, {}, "hierarchy"),
            item("inspector", "Inspector", EditorCommand::TogglePage, {}, "inspector"),
            item("project", "Project", EditorCommand::TogglePage, {}, "project"),
            item("console", "Console", EditorCommand::TogglePage, {}, "console"),
            item("profiler", "Profiler", EditorCommand::TogglePage, {}, "profiler"),
            item("render-graph", "Render Graph", EditorCommand::TogglePage, {}, "render-graph"),
            item("build", "Build", EditorCommand::TogglePage, {}, "build"),
            item("media", "Media Preview", EditorCommand::TogglePage, {}, "media"),
        }},
        {"theme", "Theme", {
            item("dark", "Dark", EditorCommand::SetDarkTheme),
            item("light", "Light", EditorCommand::SetLightTheme),
            item("high-contrast", "High Contrast", EditorCommand::SetHighContrastTheme),
        }},
        {"help", "Help", {
            item("documentation", "Documentation", EditorCommand::None),
            item("about", "About ShinkouEngine", EditorCommand::None),
        }},
    };
}

EditorObjectTreeNode EditorUiModel::make_tree_node(const GameObject& object, std::string_view filter,
                                                   ObjectId selected) {
    EditorObjectTreeNode node{object.id(), std::string(object.name()), object.active_in_hierarchy(), object.id() == selected, {}};
    for (const auto& child : object.children()) {
        if (!child) continue;
        auto childNode = make_tree_node(*child, filter, selected);
        if (filter.empty() || childNode.name != "" || !childNode.children.empty()) node.children.push_back(std::move(childNode));
    }
    const bool selfMatches = matches_filter(node.name, filter);
    if (!filter.empty() && !selfMatches && node.children.empty()) node.name.clear();
    return node;
}

bool EditorUiModel::contains_object(const EditorObjectTreeNode& node, ObjectId id) noexcept {
    if (node.id == id) return true;
    return std::any_of(node.children.begin(), node.children.end(), [id](const auto& child) {
        return contains_object(child, id);
    });
}

void EditorUiModel::sync(World& world) {
    // Names, active flags and parentage may change outside editor commands.
    // Hash only hierarchy data; stable trees avoid allocating/rebuilding rows.
    std::uint64_t signatureTree = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t value) { signatureTree ^= value; signatureTree *= 1099511628211ull; };
    const auto visit = [&](const auto& self, const GameObject& object) -> void {
        mix(object.id()); mix(object.parent() ? object.parent()->id() : 0); mix(object.active_self());
        for (const unsigned char c : object.name()) mix(c);
        mix(0xff);
        for (const auto& child : object.children()) if (child) self(self,*child);
    };
    world.each_object([&](const GameObject& object) { visit(visit,object); });
    if (signatureTree != treeSignature_) { treeSignature_ = signatureTree; treeDirty_ = true; }
    if (!treeDirty_ && (syncedWorld_ != &world || syncedObjectCount_ != world.object_count())) treeDirty_ = true;
    std::vector<EditorInspectorField> fields;
    componentTypes_ = world.component_types();
    if (auto* object = world.find_object(selectedObject_)) {
        fields.push_back({"name", "Name", std::string(object->name()), true, false});
        fields.push_back({"active", "Active", object->active_self() ? "true" : "false", true, true});
        object->each_component([&](Component& component) {
            fields.push_back({"enabled:" + std::to_string(component.id()), std::string(component.registered_type_name()) + " enabled", component.enabled() ? "true" : "false", true, true});
            auto* audioSource = dynamic_cast<components::AudioSourceComponent*>(&component);
            if (audioSource) fields.push_back({
                std::to_string(component.id()) + ":audioStatus", "Resource", audio_source_status(
                    *audioSource, audioAssetChoices_, audioAssetStatus_), false, false});
            if (audioSource) fields.push_back({
                std::to_string(component.id()) + ":audioTransport", "Playback", audioTransportComponent_ == component.id()
                    ? audioTransportState_ : "Stopped", false, false});
            for (auto& p : component.properties()) {
                if (!p.get || has_flag(p.flags, PropertyFlags::Hidden)) continue;
                if (audioSource && p.name == "assetId") continue;
                EditorInspectorField field{
                    std::to_string(component.id()) + ":" + p.name, p.displayName, property_text(p.get()),
                    p.editable(), p.type == PropertyType::Bool};
                if (audioSource && p.name == "clipPath") field.choices = audioAssetChoices_;
                if (audioSource && p.name == "bus") field.choices = audio_bus_choices();
                fields.push_back(std::move(field));
            }
            if (audioSource) {
                std::string cursor = "Unavailable";
                if (audioTransportComponent_ == component.id() && audioTransportCursorSupported_) {
                    std::ostringstream out;
                    out << std::fixed << std::setprecision(2) << audioTransportCursor_;
                    if (audioTransportDurationKnown_ && audioTransportDuration_ > 0.0)
                        out << " / " << audioTransportDuration_;
                    out << " s";
                    cursor = out.str();
                }
                fields.push_back({std::to_string(component.id()) + ":audioTimeline", "Timeline", std::move(cursor), false, false});
            }
        });
    }
    std::string signature;
    for (const auto& f : fields) signature += f.id + "\n" + f.label + "\n" + f.value + "\n";
    if (signature != inspectorSignature_) {
        inspectorSignature_ = std::move(signature);
        inspectorFields_ = std::move(fields);
        ++revision_;
    }
    if (!treeDirty_) return;
    objectRoots_.clear();
    world.each_object([this](const GameObject& object) {
        auto node = make_tree_node(object, objectFilter_, selectedObject_);
        if (objectFilter_.empty() || !node.name.empty() || !node.children.empty()) objectRoots_.push_back(std::move(node));
    });
    if (selectedObject_ != 0 && !world.find_object(selectedObject_)) selectedObject_ = 0;
    treeDirty_ = false;
    syncedWorld_ = &world;
    syncedObjectCount_ = world.object_count();
    ++revision_;
}

void EditorUiModel::set_active_page(std::string pageId) {
    const auto it = std::find_if(pages_.begin(), pages_.end(), [&pageId](const auto& page) { return page.id == pageId; });
    if (it == pages_.end()) return;
    activePage_ = std::move(pageId);
    it->visible = true;
}

void EditorUiModel::set_page_visible(std::string_view pageId, bool visible) noexcept {
    for (auto& page : pages_) if (page.id == pageId) { page.visible = visible; return; }
}

void EditorUiModel::set_build_state(EditorBuildUiState state) {
    if (same_build_state(buildState_, state)) return;
    buildState_ = std::move(state);
    ++revision_;
}

void EditorUiModel::set_asset_preview(EditorAssetPreviewUiState state) {
    if (same_asset_preview(assetPreview_, state)) return;
    assetPreview_ = std::move(state);
    ++revision_;
}

void EditorUiModel::set_media_state(EditorMediaUiState state) {
    if (same_media_state(mediaState_, state)) return;
    mediaState_ = std::move(state);
    ++revision_;
}

void EditorUiModel::set_audio_asset_choices(
    std::shared_ptr<const std::vector<EditorInspectorChoice>> choices) {
    if (!choices) choices = std::make_shared<const std::vector<EditorInspectorChoice>>();
    if (audioAssetChoices_ && audioAssetChoices_->size() == choices->size() &&
        std::equal(audioAssetChoices_->begin(), audioAssetChoices_->end(), choices->begin(),
                   [](const auto& left, const auto& right) {
                       return left.value == right.value && left.label == right.label && left.assetId == right.assetId;
                   })) return;
    audioAssetChoices_ = std::move(choices);
    ++revision_;
}

void EditorUiModel::set_audio_asset_choices(std::vector<EditorInspectorChoice> choices) {
    constexpr std::size_t maxChoices = 256;
    if (choices.size() > maxChoices) choices.resize(maxChoices);
    set_audio_asset_choices(std::make_shared<const std::vector<EditorInspectorChoice>>(std::move(choices)));
}

void EditorUiModel::set_audio_asset_status(std::string status) {
    if (status.size() > 160) status.resize(160);
    if (audioAssetStatus_ == status) return;
    audioAssetStatus_ = std::move(status);
    ++revision_;
}

void EditorUiModel::set_audio_transport_state(ComponentId component, std::string state) {
    if (state.size() > 32) state.resize(32);
    if (audioTransportComponent_ == component && audioTransportState_ == state) return;
    audioTransportComponent_ = component;
    audioTransportState_ = std::move(state);
    ++revision_;
}

void EditorUiModel::set_audio_transport_cursor(ComponentId component, double seconds, bool supported,
                                               double durationSeconds, bool durationKnown) {
    const auto boundedDuration = durationKnown && std::isfinite(durationSeconds) && durationSeconds > 0.0
        ? durationSeconds : 0.0;
    const auto bounded = std::isfinite(seconds)
        ? std::clamp(seconds, 0.0, boundedDuration > 0.0 ? boundedDuration : 7.0 * 24.0 * 60.0 * 60.0)
        : 0.0;
    const auto normalizedDurationKnown = boundedDuration > 0.0;
    if (audioTransportComponent_ == component && audioTransportCursor_ == bounded &&
        audioTransportCursorSupported_ == supported &&
        audioTransportDuration_ == boundedDuration &&
        audioTransportDurationKnown_ == normalizedDurationKnown) return;
    audioTransportComponent_ = component;
    audioTransportCursor_ = bounded;
    audioTransportCursorSupported_ = supported;
    audioTransportDuration_ = boundedDuration;
    audioTransportDurationKnown_ = normalizedDurationKnown;
    ++revision_;
}

void EditorUiModel::set_audio_transport_preview(
    std::shared_ptr<const EditorAudioPreviewSnapshot> preview) {
    if (audioTransportPreview_ == preview) return;
    audioTransportPreview_ = std::move(preview);
    ++revision_;
}

void EditorUiModel::execute(EditorCommand command) noexcept {
    lastCommand_ = command;
    if (command == EditorCommand::Play) { playing_ = !playing_; paused_ = false; }
    else if (command == EditorCommand::Pause) { if (playing_) paused_ = !paused_; }
    else if (command == EditorCommand::Step) { playing_ = true; paused_ = true; }
}

} // namespace shinkou::editor
