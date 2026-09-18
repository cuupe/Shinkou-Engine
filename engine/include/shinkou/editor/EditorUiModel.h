#pragma once

#include "shinkou/Types.h"
#include "shinkou/ui/Render.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou {
class GameObject;
class World;

namespace editor {

struct EditorAudioPreviewSnapshot;
struct EditorVideoPreviewSnapshot;
struct EditorModelPreviewSnapshot;
struct EditorModelPreviewSceneState;

enum class EditorCommand : std::uint8_t {
    None,
    NewScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    SaveLayout,
    ReloadLayout,
    ResetLayout,
    Undo,
    Redo,
    Play,
    Pause,
    Step,
    FrameSelection,
    CreateEmpty,
    CreateChild,
    Create3DObject,
    Create2DObject,
    CreateUiObject,
    AddComponent,
    RefreshAssets,
    OpenAsset,
    BuildProject,
    CancelBuild,
    RefreshBuildTools,
    RefreshProjectFiles,
    AssociateProjectFile,
    GenerateClangdConfig,
    ImportCompileCommands,
    ExportCompileCommands,
    OpenProjectInIde,
    SaveBuildProfile,
    ReloadBuildProfile,
    SelectBuildProfile,
    SetDiagnosticFilter,
    SelectBuildDiagnostic,
    OpenDiagnosticInIde,
    PruneFileRecovery,
    ClearFileConflicts,
    ReviewFileConflict,
    MediaPlay,
    MediaPause,
    MediaStop,
    MediaToggleLoop,
    MediaSeek,
    MediaSetVolume,
    TogglePage,
    SetDarkTheme,
    SetLightTheme,
    SetHighContrastTheme,
    ProjectSettings,
    Quit,
    DeleteObject,
    ClearConsole,
    SetUiScale,
    ToggleCompact,
    ToggleDocking,
};

struct EditorMenuItemModel {
    std::string id;
    std::string label;
    std::string shortcut;
    std::string target;
    EditorCommand command{EditorCommand::None};
    bool separator{false};
    bool enabled{true};
};

struct EditorMenuModel {
    std::string id;
    std::string label;
    std::vector<EditorMenuItemModel> items;
};

struct EditorPageModel {
    std::string id;
    std::string title;
    std::string shortcut;
    bool visible{true};
    bool primary{false};
};

struct EditorObjectTreeNode {
    ObjectId id{0};
    std::string name;
    bool active{true};
    bool selected{false};
    std::vector<EditorObjectTreeNode> children;
};

struct EditorInspectorChoice {
    std::string value;
    std::string label;
    std::uint64_t assetId{0};
};

struct EditorInspectorField {
    std::string id;
    std::string label;
    std::string value;
    bool editable{false};
    bool boolean{false};
    std::shared_ptr<const std::vector<EditorInspectorChoice>> choices{};
};

struct EditorBuildToolModel {
    std::string name;
    std::string executable;
    bool available{false};
};

struct EditorProjectFileModel {
    std::string kind;
    std::string path;
    bool recommended{false};
};

struct EditorBuildDiagnosticModel {
    std::string severity;
    std::string file;
    std::size_t line{0};
    std::size_t column{0};
    std::string code;
    std::string message;
};

struct EditorAssetPreviewUiState {
    std::string path;
    std::string kind;
    std::string title;
    std::string status;
    bool loading{false};
    bool truncated{false};
    std::vector<std::string> textLines;
    std::uint32_t imageWidth{0};
      std::uint32_t imageHeight{0};
      std::shared_ptr<const ui::UiImageSnapshot> imageSnapshot{};
      std::shared_ptr<const EditorModelPreviewSnapshot> modelPreview{};
      std::shared_ptr<const EditorModelPreviewSceneState> modelPreviewScene{};
      std::uint32_t modelTextureWidth{0};
      std::uint32_t modelTextureHeight{0};
      std::shared_ptr<const ui::UiImageSnapshot> modelTextureSnapshot{};
      std::string modelTextureStatus{};
      std::int32_t modelMaterialIndex{-1};
      std::int32_t modelTextureIndex{-1};
      std::int32_t modelTextureImageIndex{-1};
      std::string modelMaterialLabel{};
      std::string modelTextureLabel{};
      std::string modelTextureRole{};
      bool modelGpuPreviewReady{false};
      bool modelGpuMaterialApplied{false};
      bool modelGpuMaterialFactorsApplied{false};
      bool modelGpuTextureRoleApplied{false};
      bool modelGpuBaseColorTextureSampled{false};
      bool modelGpuNormalTextureSampled{false};
      bool modelGpuMetallicRoughnessTextureSampled{false};
      bool modelGpuTextureSampled{false};
      bool modelGpuOffscreenTargetReady{false};
      bool modelGpuOffscreenCompositeApplied{false};
      std::string modelGpuPreviewStatus{};
    // The AssetSystem bridge is intentionally presentation-only. Providers
    // remain responsible for decoded previews, while this state exposes the
    // canonical resource load/cache result to the editor inspector.
    std::string assetSystemStatus{"AssetSystem not connected"};
    std::string assetSystemFormat{};
    std::string assetSystemMetadataFormat{};
    std::size_t assetSystemMetadataBytes{0};
    std::uint64_t assetSystemSourceHash{0};
    bool assetSystemLoading{false};
    bool assetSystemReady{false};
};

// Presentation-only snapshot for the media dock. Decoders and transport stay
// in EditorLayer; retained paint consumes this bounded state without touching
// the filesystem or audio backend.
struct EditorMediaUiState {
    std::string path;
    std::string kind;
    std::string status{"Select a media resource"};
    std::string playbackState{"Stopped"};
    double currentTime{0.0};
    double duration{0.0};
    double volume{1.0};
    bool available{false};
    bool loop{false};
    bool previewLoading{false};
    std::string previewStatus{"Media preview not loaded"};
    std::shared_ptr<const EditorAudioPreviewSnapshot> audioPreview{};
    std::shared_ptr<const EditorVideoPreviewSnapshot> videoPreview{};
};

// Retained build state is intentionally presentation-only. The process runner
// and build system remain independent of the editor renderer and can be
// audited/tested without constructing a UI frame.
struct EditorBuildUiState {
    std::string profileName{"CMake Debug"};
    std::string selectedProfileId{"default"};
    std::string profileStatus{"Build profile defaults"};
    std::string status{"Ready"};
    std::string ideStatus{"IDE launcher not used"};
    bool running{false};
    std::size_t compileCommandCount{0};
    std::string compileCommandsStatus{"Not loaded"};
    std::string diagnosticFilter{"all"};
    std::string projectStatus{"Project discovery not run"};
    std::string recommendedProjectFile{};
    std::string clangdStatus{"Clangd config not generated"};
    std::size_t diagnosticTotalCount{0};
    std::size_t diagnosticErrorCount{0};
    std::size_t diagnosticWarningCount{0};
    std::size_t diagnosticNoteCount{0};
    std::size_t selectedDiagnostic{static_cast<std::size_t>(-1)};
    std::vector<std::string> profileIds;
    std::vector<EditorInspectorField> profileFields;
    std::vector<EditorBuildToolModel> tools;
    std::vector<EditorBuildToolModel> ides;
    std::vector<EditorProjectFileModel> projectFiles;
    std::vector<EditorBuildDiagnosticModel> diagnostics;
    std::vector<std::string> outputLines;
};

struct EditorFileRecoveryEntryModel {
    std::string kind;
    std::string sourcePath;
    std::string destinationPath;
    std::string recyclePath;
    std::uintmax_t bytes{0};
    bool recoverable{false};
    bool orphan{false};
};

struct EditorFileConflictEntryModel {
    std::string path;
    std::string kind;
    std::uint64_t batchId{0};
};

struct EditorFileRecoveryUiState {
    std::string status{"Recovery area is clean"};
    std::size_t undoCount{0};
    std::size_t redoCount{0};
    std::size_t orphanCount{0};
    std::uintmax_t totalBytes{0};
    std::vector<EditorFileRecoveryEntryModel> entries;
    std::size_t externalChangeCount{0};
    std::uint64_t latestExternalBatchId{0};
    std::string externalBatchStatus{"No external change batch"};
    std::vector<EditorFileConflictEntryModel> externalChanges;
};

class EditorUiModel {
    std::vector<EditorMenuModel> menus_;
    std::vector<EditorPageModel> pages_;
    std::vector<EditorObjectTreeNode> objectRoots_;
    ObjectId selectedObject_{0};
    std::string objectFilter_;
    std::string activePage_{"scene"};
    EditorCommand lastCommand_{EditorCommand::None};
    bool playing_{false};
    bool paused_{false};
    bool treeDirty_{true};
    std::uint64_t revision_{0};
    const World* syncedWorld_{nullptr};
    std::size_t syncedObjectCount_{0};
    std::uint64_t treeSignature_{0};
    std::vector<EditorInspectorField> inspectorFields_;
    std::vector<std::string> componentTypes_;
    std::string inspectorSignature_;
    std::shared_ptr<const std::vector<EditorInspectorChoice>> audioAssetChoices_{};
    std::string audioAssetStatus_{"Audio manifest unavailable"};
    ComponentId audioTransportComponent_{0};
    std::string audioTransportState_{"Unavailable"};
    double audioTransportCursor_{0.0};
    bool audioTransportCursorSupported_{false};
    double audioTransportDuration_{0.0};
    bool audioTransportDurationKnown_{false};
    std::shared_ptr<const EditorAudioPreviewSnapshot> audioTransportPreview_{};
    EditorBuildUiState buildState_{};
    EditorFileRecoveryUiState fileRecoveryState_{};
    EditorAssetPreviewUiState assetPreview_{};
    EditorMediaUiState mediaState_{};

    static EditorObjectTreeNode make_tree_node(const GameObject& object, std::string_view filter,
                                               ObjectId selected);
    static bool contains_object(const EditorObjectTreeNode& node, ObjectId id) noexcept;
    void build_default_menus();
    void build_default_pages();

public:
    EditorUiModel();

    void sync(World& world);
    void invalidate() noexcept { treeDirty_ = true; }
    void select_object(ObjectId id) noexcept {
        if (selectedObject_ == id) return;
        selectedObject_ = id;
        treeDirty_ = true;
    }
    void set_object_filter(std::string filter) {
        if (objectFilter_ == filter) return;
        objectFilter_ = std::move(filter);
        treeDirty_ = true;
    }
    void set_active_page(std::string pageId);
    void set_page_visible(std::string_view pageId, bool visible) noexcept;
    void set_build_state(EditorBuildUiState state);
    void set_file_recovery_state(EditorFileRecoveryUiState state);
    void set_asset_preview(EditorAssetPreviewUiState state);
    void set_media_state(EditorMediaUiState state);
    void set_audio_asset_choices(std::shared_ptr<const std::vector<EditorInspectorChoice>> choices);
    void set_audio_asset_choices(std::vector<EditorInspectorChoice> choices);
    void set_audio_asset_status(std::string status);
    void set_audio_transport_state(ComponentId component, std::string state);
    void set_audio_transport_cursor(ComponentId component, double seconds, bool supported,
                                    double durationSeconds = 0.0, bool durationKnown = false);
    void set_audio_transport_preview(std::shared_ptr<const EditorAudioPreviewSnapshot> preview);
    void execute(EditorCommand command) noexcept;
    const std::vector<EditorInspectorField>& inspector_fields() const noexcept { return inspectorFields_; }
    const std::vector<std::string>& component_types() const noexcept { return componentTypes_; }

    const std::vector<EditorMenuModel>& menus() const noexcept { return menus_; }
    const std::vector<EditorPageModel>& pages() const noexcept { return pages_; }
    const std::vector<EditorObjectTreeNode>& object_roots() const noexcept { return objectRoots_; }
    ObjectId selected_object() const noexcept { return selectedObject_; }
    const std::string& object_filter() const noexcept { return objectFilter_; }
    const std::string& active_page() const noexcept { return activePage_; }
    EditorCommand last_command() const noexcept { return lastCommand_; }
    bool playing() const noexcept { return playing_; }
    bool paused() const noexcept { return paused_; }
    const EditorBuildUiState& build_state() const noexcept { return buildState_; }
    const EditorFileRecoveryUiState& file_recovery_state() const noexcept { return fileRecoveryState_; }
    const EditorAssetPreviewUiState& asset_preview() const noexcept { return assetPreview_; }
    const EditorMediaUiState& media_state() const noexcept { return mediaState_; }
    double audio_transport_cursor() const noexcept { return audioTransportCursor_; }
    double audio_transport_duration() const noexcept { return audioTransportDuration_; }
    bool audio_transport_cursor_supported() const noexcept { return audioTransportCursorSupported_; }
    bool audio_transport_duration_known() const noexcept { return audioTransportDurationKnown_; }
    const std::shared_ptr<const EditorAudioPreviewSnapshot>& audio_transport_preview() const noexcept {
        return audioTransportPreview_;
    }
    std::uint64_t revision() const noexcept { return revision_; }
};

} // namespace editor
} // namespace shinkou
