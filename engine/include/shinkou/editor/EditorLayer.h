#pragma once

#include "shinkou/Types.h"
#include "shinkou/editor/DockLayout.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/editor/EditorUiModel.h"
#include "shinkou/editor/EditorUi.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/editor/EditorBuildSystem.h"
#include "shinkou/editor/EditorToolIntegration.h"
#include "shinkou/editor/EditorProjectIntegration.h"
#include "shinkou/editor/EditorCompileCommands.h"
#include "shinkou/editor/EditorModelPreviewScene.h"
#include "shinkou/editor/EditorModelPreviewRenderer.h"
#include "shinkou/editor/EditorModelSceneRenderer.h"
#include "shinkou/editor/CoordinateSpaces.h"
#include "shinkou/editor/EditorAssetIndex.h"
#include "shinkou/editor/EditorAssetReference.h"
#include "shinkou/editor/EditorAudioPreview.h"
#include "shinkou/editor/EditorImagePreview.h"
#include "shinkou/editor/EditorVideoPreview.h"
#include "shinkou/editor/EditorModelPreview.h"
#include "shinkou/assets/AssetSystem.h"
#include "shinkou/audio/AudioSystem.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/ui/InputBridge.h"
#include "shinkou/ui/Components.h"
#include "shinkou/ui/Media.h"
#include "shinkou/ui/Style.h"
#include "shinkou/ui/Theme.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou {
class World;
class GameObject;
namespace input { class InputSystem; }
namespace audio { class AudioSceneSystem; }

namespace editor {

struct EditorLayoutState {
    std::uint32_t layoutVersion{3};
    std::string theme{"dark"};
    std::string workspace{"Default"};
    std::string projectRoot{"."};
    std::string layoutFile{"Saved/Editor/Layouts/Default.json"};
    std::string dockLayoutFile{"Saved/Editor/Layouts/Default.dock.json"};
    std::string styleFile{"Saved/Editor/Styles/Default.json"};
    std::string fontPath{};
    float fontSize{14.0f};
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
};

struct EditorPanelContext {
    render::Renderer& renderer;
    World& world;
    EditorUiModel& ui;
    const EditorLayoutState& layout;
    FrameIndex frame{0};
    Seconds deltaSeconds{0};
};

struct EditorPanel {
    std::string id;
    std::string title;
    bool defaultVisible{true};
    std::function<void(EditorPanelContext&)> draw;
    bool closeable{true};
    float minWidth{160.0f};
    float minHeight{100.0f};
};

class EditorLayer {
    struct AsyncFileScan {
        std::uint64_t generation{0};
        FileSystemService service{};
        std::vector<FileEntry> entries;
        std::vector<FileChange> changes;
        std::shared_ptr<const EditorAssetIndexSnapshot> index;
    };

    struct AsyncAssetPreview {
        std::uint64_t generation{0};
        std::uint64_t sourceStamp{0};
        std::string path;
        std::string content;
        bool truncated{false};
        std::string error;
    };

    struct AsyncAssetManifest {
        std::uint64_t generation{0};
        std::shared_ptr<const std::vector<assets::AssetManifestEntry>> entries{};
        std::string error;
        std::string readbackError;
        bool readbackValidated{false};
    };

    // File operations are external to EditorDocument's world-only history.
    // Keep the destination prefixes until the next manifest snapshot so live
    // scene references can reacquire their manifest-backed identities after a
    // rename without making the render thread scan the filesystem.
    struct PendingAssetReferenceRefresh {
        std::string destinationPrefix;
    };

    struct AssetDocumentMigrationReport {
        std::size_t filesScanned{0};
        std::size_t filesChanged{0};
        std::size_t referencesChanged{0};
        std::size_t filesSkipped{0};
        std::string firstError;
    };

    struct AsyncModelSceneAsset {
        std::uint64_t generation{0};
        std::uint64_t sourceStamp{0};
        std::string path;
        bool active{false};
        std::future<EditorModelPreviewResult> future{};
        std::shared_ptr<std::atomic_bool> cancel{};
        std::shared_ptr<const EditorModelPreviewSnapshot> snapshot{};
        std::string status{"Model scene asset not loaded"};
    };

    using AsyncImagePreview = EditorImagePreviewResult;

    struct LiveBuildOutput {
        mutable std::mutex mutex;
        std::vector<std::string> lines;
        std::vector<EditorBuildDiagnostic> diagnostics;
        std::size_t diagnosticTotalCount{0};
        std::size_t diagnosticErrorCount{0};
        std::size_t diagnosticWarningCount{0};
        std::size_t diagnosticNoteCount{0};
        std::string standardOutputPending;
        std::string standardErrorPending;
        std::uint64_t revision{0};
    };

    EditorUiModel uiModel_;
    EditorLayoutState layout_{};
    std::vector<EditorPanel> panels_;
    std::unordered_map<std::string, bool> panelVisibility_;
    std::vector<std::string> assetEntries_;
    std::vector<std::string> consoleEntries_;
    std::vector<float> frameTimes_;
    std::string lastStatus_;
    std::string assetFilter_;
    std::string selectedAsset_;
    std::vector<FileEntry> projectFiles_;
    std::shared_ptr<const EditorAssetIndexSnapshot> assetIndex_{};
    std::filesystem::path assetDirectory_{};
    std::string projectRootOverride_{};
    std::uint64_t projectFilesRevision_{0};
    bool initialized_{false};
    bool uiContextOwned_{false};
    bool imguiEnabled_{false};
    bool imguiPlatformInitialized_{false};
    bool uiPresentationWarningEmitted_{false};
    void* nativeWindow_{nullptr};
    void* nativeMenu_{nullptr};
    bool assetsDirty_{true};
    float displayWidth_{1280.0f};
    float displayHeight_{720.0f};
    float displayDpiScale_{1.0f};
    float appliedUiScale_{1.0f};
    std::string renderViewMode_{"Perspective"};
    std::string appliedFontPath_;
    float appliedFontSize_{0.0f};
    float editorFilePollAccumulator_{0.0f};
    std::uint64_t fileScanGeneration_{0};
    std::uint64_t assetManifestGeneration_{0};
    bool assetManifestDirty_{true};
    std::future<AsyncFileScan> fileScanFuture_{};
    std::future<AsyncAssetPreview> assetPreviewFuture_{};
    std::future<AsyncAssetManifest> assetManifestFuture_{};
    assets::AssetFuture assetSystemPreviewFuture_{};
    std::future<AsyncImagePreview> imagePreviewFuture_{};
    std::future<EditorAudioPreviewResult> audioPreviewFuture_{};
    std::uint64_t assetPreviewGeneration_{0};
    std::uint64_t assetPreviewStamp_{0};
    std::uint64_t assetSystemPreviewGeneration_{0};
    std::uint64_t assetSystemPreviewStamp_{0};
    std::string assetSystemPreviewPath_{};
    std::string assetSystemPreviewStatus_{"AssetSystem not connected"};
    std::string assetSystemPreviewFormat_{};
    std::string assetSystemPreviewMetadataFormat_{};
    std::size_t assetSystemPreviewMetadataBytes_{0};
    std::uint64_t assetSystemPreviewSourceHash_{0};
    bool assetSystemPreviewLoading_{false};
    bool assetSystemPreviewReady_{false};
    std::shared_ptr<const std::vector<assets::AssetManifestEntry>> assetManifest_{};
    std::vector<PendingAssetReferenceRefresh> pendingAssetReferenceRefreshes_{};
    std::shared_ptr<const std::vector<EditorInspectorChoice>> audioInspectorChoices_{};
    std::string assetManifestStatus_{"AssetSystem manifest not connected"};
    EditorAssetPreviewUiState assetPreviewState_{};
    std::uint64_t imagePreviewGeneration_{0};
    std::uint64_t imagePreviewStamp_{0};
    std::uint64_t audioPreviewGeneration_{0};
    std::uint64_t audioPreviewStamp_{0};
    std::shared_ptr<std::atomic_bool> audioPreviewCancel_{};
    std::shared_ptr<const EditorAudioPreviewSnapshot> audioPreviewSnapshot_{};
    std::string audioPreviewSourcePath_{};
    std::string audioPreviewStatus_{"Audio preview not loaded"};
    std::future<EditorVideoPreviewResult> videoPreviewFuture_{};
    std::future<EditorVideoPreviewResult> videoFrameFuture_{};
    std::uint64_t videoPreviewGeneration_{0};
    std::uint64_t videoPreviewStamp_{0};
    std::shared_ptr<std::atomic_bool> videoPreviewCancel_{};
    std::shared_ptr<std::atomic_bool> videoFrameCancel_{};
    double pendingVideoSeekSeconds_{-1.0};
    std::shared_ptr<const EditorVideoPreviewSnapshot> videoPreviewSnapshot_{};
    std::string videoPreviewStatus_{"Video preview not loaded"};
    std::future<EditorModelPreviewResult> modelPreviewFuture_{};
    std::uint64_t modelPreviewGeneration_{0};
    std::uint64_t modelPreviewStamp_{0};
      std::shared_ptr<std::atomic_bool> modelPreviewCancel_{};
      std::shared_ptr<const EditorModelPreviewSnapshot> modelPreviewSnapshot_{};
      EditorModelPreviewScene modelPreviewScene_{};
      EditorModelPreviewRenderer modelPreviewRenderer_{};
      std::string modelPreviewStatus_{"Model preview not loaded"};
      std::future<EditorImagePreviewResult> modelTexturePreviewFuture_{};
      std::uint64_t modelTexturePreviewGeneration_{0};
      std::uint64_t modelTexturePreviewStamp_{0};
      std::string modelTexturePreviewPath_{};
      std::int32_t modelMaterialSelection_{-1};
      std::int32_t modelTextureSelection_{-1};
    std::unordered_map<assets::AssetId, AsyncModelSceneAsset> modelSceneAssets_{};
    std::uint64_t modelSceneGeneration_{0};
    EditorModelSceneRenderer modelSceneRenderer_{};
    EditorModelSceneRenderState modelSceneRenderState_{};
    EditorBuildProfile buildProfile_{};
    std::vector<EditorBuildProfile> buildProfiles_{};
    std::string selectedBuildProfileId_{"default"};
    std::vector<EditorIdeDescriptor> ideTools_{};
    EditorProjectDiscoveryResult projectDiscovery_{};
    std::string projectIntegrationStatus_{"Project discovery not run"};
    std::string clangdConfigStatus_{"Clangd config not generated"};
    std::string buildProfileStatus_{"Build profile defaults"};
    std::string ideStatus_{"IDE launcher not used"};
    std::string diagnosticFilter_{"all"};
    std::size_t selectedDiagnosticIndex_{static_cast<std::size_t>(-1)};
    std::uint32_t ideProcessId_{0};
    EditorExternalProcessState ideProcessState_{EditorExternalProcessState::NotStarted};
    std::uint32_t ideProcessExitCode_{0};
    std::string ideProcessName_{};
    std::chrono::steady_clock::time_point ideProcessPollTime_{};
    std::vector<EditorCompileCommand> compileCommands_{};
    std::string compileCommandsStatus_{"Not loaded"};
    std::future<EditorBuildProcessResult> buildFuture_{};
    std::shared_ptr<std::atomic_bool> buildCancel_{};
    std::shared_ptr<LiveBuildOutput> liveBuildOutput_{};
    std::uint64_t liveBuildUiRevision_{0};
    audio::AudioSystem* audioSystem_{nullptr};
    audio::AudioSceneSystem* audioSceneSystem_{nullptr};
    assets::AssetSystem* assetSystem_{nullptr};
    bool assetSystemRootNeedsRestart_{false};
    audio::AudioVoiceId audioPreviewVoice_{0};
    audio::AudioAssetId audioPreviewAsset_{0};
    assets::AssetId audioPreviewAssetId_{0};
    std::unordered_map<assets::AssetId, audio::AudioAssetId> audioAssetBindings_{};
    std::string audioPreviewPath_{};
    EditorBuildProcessResult lastBuildResult_{};
    std::string buildStatus_{"Ready"};
    bool buildUiDirty_{true};
    DockWorkspace dockWorkspace_{};
    ui::ThemeRegistry themeRegistry_{};
    ui::UiStyleConfig styleConfig_{};
    ui::MediaPanel mediaPanel_{{"editor-media", "Media", ui::MediaKind::Video, {"asset://preview", "video/*", "Preview"}, true, true, true, true, 16.0f}};
    ui::ComponentDocument uiComponents_{};
    ui::UiRuntime uiRuntime_{};
    ui::InputBridgeStats uiInputStats_{};
    FileSystemService fileSystem_{};
    EditorBuildSystem buildSystem_{};
    EditorUi editorUi_{};
    World* activeWorld_{nullptr};
    float orbitDistance_{5.0f};
    void navigate_viewport(ViewportNavigation action, math::Vec2 delta);
    std::vector<EditorDocument> undo_, redo_;
    bool sceneDirty_{false};
    bool stepPending_{false};
    bool quitRequested_{false};
    std::string scenePath_{"assets/Scenes/Untitled.scene"};
    bool checkpoint(World& world);
    bool edit_field(std::string_view id, std::string_view value);
    bool dispatch_audio_source_transport(EditorCommand command, std::string_view target, World& world);
    void document_changed(bool preserveRedo = false);

    void register_builtin_panels();
    void build_default_workspace();
    void sync_workspace_visibility() noexcept;
    void sync_page_visibility() noexcept;
    void refresh_asset_cache();
    void poll_editor_files();
    void request_file_scan();
    void consume_file_scan();
    void reset_asset_manifest(std::string status);
    void request_asset_manifest_scan();
    void poll_asset_manifest_scan();
    std::size_t remap_live_asset_references(std::string_view from, std::string_view to);
    std::size_t invalidate_live_asset_references(std::string_view path);
    AssetDocumentMigrationReport migrate_asset_documents(std::string_view from,
                                                         std::string_view to,
                                                         bool invalidateIdentity);
    std::size_t rebind_asset_references_from_manifest();
    void apply_pending_asset_reference_refreshes();
    void set_selected_asset(std::string path);
    void request_asset_preview();
    void poll_asset_preview();
    void reset_asset_system_preview(std::string status);
    void publish_asset_system_preview_state();
    void request_asset_system_preview();
    void poll_asset_system_preview();
    void request_image_preview(const EditorAssetIndexEntry& indexed);
    void poll_image_preview();
    void request_audio_preview(const EditorAssetIndexEntry& indexed);
    void request_audio_preview_for_path(std::string path, const EditorAssetIndexEntry& indexed);
    void poll_audio_preview();
    void request_video_preview(const EditorAssetIndexEntry& indexed);
    void poll_video_preview();
    void request_video_frame(double seconds);
    void poll_video_frame();
    void request_model_preview(const EditorAssetIndexEntry& indexed);
    void poll_model_preview();
    void sync_model_scene_assets(const World& world);
    void poll_model_scene_assets();
    void reset_model_scene_assets();
    void sync_model_preview_selection();
    void select_model_material(std::int32_t delta);
    void select_model_texture(std::int32_t delta);
    void request_model_texture_preview();
    void poll_model_texture_preview();
    void navigate_model_preview(ViewportNavigation action, math::Vec2 delta);
    void reset_model_preview();
    void sync_media_preview_state();
    void stop_audio_preview();
    void clear_audio_asset_bindings();
    void clear_media_preview();
    void start_audio_preview();
    void poll_build();
    void sync_build_ui_state();
    void refresh_ide_tools();
    void refresh_project_integration();
    bool associate_project_file(std::string_view path);
    bool generate_clangd_config();
    bool save_build_profile();
    bool load_build_profile();
    bool select_build_profile(std::string_view id);
    bool edit_build_profile_field(std::string_view id, std::string_view value);
    bool open_project_in_ide(std::string_view target, std::string_view selectedFile = {},
                             std::size_t line = 0, std::size_t column = 0);
    bool open_diagnostic_in_ide(std::string_view target);
    bool select_build_diagnostic(std::string_view index);
    bool set_diagnostic_filter(std::string_view filter);
    void poll_ide_process();
    bool live_build_output_changed() const noexcept;
    bool import_compile_commands(std::string_view path = {});
    bool export_compile_commands(std::string_view path = {});
    bool start_build();
    void cancel_build();
    void set_asset_directory(std::filesystem::path directory);
    void handle_asset_action(EditorAssetAction action, std::string path, std::string value);
    bool drop_asset_to_viewport(std::string path, math::Vec2 point);
    void draw_toolbar(render::Renderer& renderer, World& world);
    void draw_main_menu(render::Renderer& renderer, World& world);
    void draw_hierarchy(World& world);
    void draw_hierarchy_object(const EditorObjectTreeNode& object);
    void draw_inspector(World& world);
    void draw_viewport(render::Renderer& renderer);
    void draw_game_view(render::Renderer& renderer);
    void draw_assets();
    void draw_console(const render::Renderer& renderer);
    void draw_profiler(const render::Renderer& renderer);
    void draw_render_graph(render::Renderer& renderer);
    void draw_settings();
    void draw_media();
    void draw_status_bar(const render::Renderer& renderer);
    void draw_registered_panels(render::Renderer& renderer, World& world, FrameIndex frame, Seconds dt);
    void dispatch_command(EditorCommand command, std::string_view target, World& world);
    void install_native_menu();
    void uninstall_native_menu();
    bool save_layout_file();
    bool load_layout_file();
    bool save_workspace_file();
    bool load_workspace_file();
    bool save_style_file();
    bool load_style_file();
    void sync_style_to_layout() noexcept;
    void sync_layout_to_style() noexcept;
    void apply_theme();
    void apply_render_view_mode(World& world, std::string_view mode);

public:
    void execute_command(EditorCommand command, std::string_view target, World& world) { activeWorld_ = &world; dispatch_command(command, target, world); }
    // Shared ingress for native OS drag/drop adapters and the retained Project
    // browser. It preserves the same validation and undo transaction for both
    // paths, so external adapters cannot bypass editor safety checks.
    bool create_asset_reference_at_viewport(World& world, std::string path, math::Vec2 point) {
        activeWorld_ = &world;
        return drop_asset_to_viewport(std::move(path), point);
    }
    // Native Window adapters supply an absolute UTF-8 path and client-area
    // physical pixels. Normalize and convert them here, then reuse the same
    // validated viewport ingress and undo transaction as retained UI drops.
    bool create_asset_reference_from_window_drop(World& world, std::string nativePath,
                                                 WindowClientPx clientPoint);
    bool consume_simulation_step() noexcept;
    bool quit_requested() const noexcept { return quitRequested_; }
    const EditorUi& editor_ui() const noexcept { return editorUi_; }
    void set_native_window(void* nativeWindow) noexcept { nativeWindow_ = nativeWindow; }
    void handle_native_menu_command(std::uint32_t command, World& world);
    bool initialize(bool enableImGui = true);
    void process_input(const input::InputSystem& input, World& world);
    // Resolve the UE/Slate-style editor layout before scene passes are built.
    // The renderer uses this seam to constrain world rendering to the viewport.
    void prepare_frame(render::Renderer& renderer, World& world);
    void draw(render::Renderer& renderer, World& world, Seconds dt, FrameIndex frame);
    void shutdown(render::Renderer* renderer = nullptr);
    std::size_t ui_command_count() const noexcept;
    std::size_t ui_text_command_count() const noexcept;
    std::size_t ui_asset_file_count() const noexcept { return projectFiles_.size(); }
    std::size_t ui_visible_asset_file_count() const noexcept { return editorUi_.visible_asset_count(); }
    std::string ui_asset_directory() const { return assetDirectory_.generic_string(); }
    const EditorMediaUiState& media_preview() const noexcept { return uiModel_.media_state(); }
    std::string ui_first_asset_path() const {
        return projectFiles_.empty() ? std::string{} : projectFiles_.front().relativePath.generic_string();
    }
    DockRect ui_viewport_rect() const noexcept { return editorUi_.viewport_rect(); }
    float ui_dpi_scale() const noexcept { return editorUi_.dpi_scale(); }
    std::size_t model_scene_instance_count() const noexcept { return modelSceneRenderState_.submittedInstances; }
    std::size_t model_scene_cached_asset_count() const noexcept { return modelSceneRenderer_.cached_asset_count(); }
    std::size_t model_scene_loaded_asset_count() const noexcept;
    const std::string& model_scene_status() const noexcept { return modelSceneRenderState_.status; }

    bool register_panel(EditorPanel panel);
    bool unregister_panel(std::string_view id);
    bool set_panel_visible(std::string_view id, bool visible);
    bool panel_visible(std::string_view id) const noexcept;

    void set_project_root(std::string path);
    void set_audio_system(audio::AudioSystem* audioSystem) noexcept;
    void set_audio_scene_system(audio::AudioSceneSystem* audioSceneSystem) noexcept {
        audioSceneSystem_ = audioSceneSystem;
    }
    void set_asset_system(assets::AssetSystem* assetSystem);
    void set_layout_path(std::string path);
    void set_display_size(float width, float height, float dpiScale = 1.0f) noexcept;
    void set_asset_view(EditorAssetView view) noexcept { editorUi_.set_asset_view(view); }
    EditorAssetView asset_view() const noexcept { return editorUi_.asset_view(); }
    bool save_layout();
    bool load_layout();
    void reset_layout();
    void set_theme(std::string theme);
    void set_style_file(std::string path);
    ui::UiStyleConfig& ui_style() noexcept { return styleConfig_; }
    const ui::UiStyleConfig& ui_style() const noexcept { return styleConfig_; }
    FileSystemService& file_system() noexcept { return fileSystem_; }
    const FileSystemService& file_system() const noexcept { return fileSystem_; }
    EditorBuildSystem& build_system() noexcept { return buildSystem_; }
    const EditorBuildSystem& build_system() const noexcept { return buildSystem_; }
    void refresh_build_tools() { buildSystem_.refresh_toolchains(); refresh_ide_tools(); refresh_project_integration(); }
    ui::ThemeRegistry& theme_registry() noexcept { return themeRegistry_; }
    const ui::ThemeRegistry& theme_registry() const noexcept { return themeRegistry_; }
    DockWorkspace& dock_workspace() noexcept { return dockWorkspace_; }
    const DockWorkspace& dock_workspace() const noexcept { return dockWorkspace_; }
    ui::UiRuntime& ui_runtime() noexcept { return uiRuntime_; }
    const ui::UiRuntime& ui_runtime() const noexcept { return uiRuntime_; }
    const ui::InputBridgeStats& ui_input_stats() const noexcept { return uiInputStats_; }
    const EditorLayoutState& layout() const noexcept { return layout_; }
    const std::string& last_status() const noexcept { return lastStatus_; }
    void push_console(std::string message);
    bool build_running() const noexcept {
        return buildFuture_.valid() &&
            buildFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    }
    const EditorBuildProfile& build_profile() const noexcept { return buildProfile_; }
    const std::vector<EditorBuildProfile>& build_profiles() const noexcept { return buildProfiles_; }
    const std::string& selected_build_profile_id() const noexcept { return selectedBuildProfileId_; }
    const std::string& diagnostic_filter() const noexcept { return diagnosticFilter_; }
    std::size_t selected_diagnostic_index() const noexcept { return selectedDiagnosticIndex_; }
    std::uint32_t ide_process_id() const noexcept { return ideProcessId_; }
    const std::vector<EditorIdeDescriptor>& ide_tools() const noexcept { return ideTools_; }
    const EditorProjectDiscoveryResult& project_discovery() const noexcept { return projectDiscovery_; }
    const std::string& project_integration_status() const noexcept { return projectIntegrationStatus_; }
    const std::string& clangd_config_status() const noexcept { return clangdConfigStatus_; }
    const std::string& build_profile_status() const noexcept { return buildProfileStatus_; }
    const std::string& ide_status() const noexcept { return ideStatus_; }
    const EditorBuildProcessResult& last_build_result() const noexcept { return lastBuildResult_; }
    std::size_t compile_command_count() const noexcept { return compileCommands_.size(); }
    const std::string& compile_commands_status() const noexcept { return compileCommandsStatus_; }
    const EditorAssetPreviewUiState& asset_preview() const noexcept { return assetPreviewState_; }
    std::size_t asset_system_manifest_count() const noexcept {
        return assetManifest_ ? assetManifest_->size() : 0;
    }
    const std::string& asset_system_manifest_status() const noexcept { return assetManifestStatus_; }
};

} // namespace editor
} // namespace shinkou
