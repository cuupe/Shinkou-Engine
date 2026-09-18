#include "shinkou/World.h"
#include "shinkou/editor/EditorUiModel.h"
#include <algorithm>
#include <iostream>

namespace {
bool has_selected(const shinkou::editor::EditorObjectTreeNode& node, shinkou::ObjectId id) {
    if (node.id == id && node.selected) return true;
    for (const auto& child : node.children) if (has_selected(child, id)) return true;
    return false;
}
}

int main() {
    shinkou::World world;
    auto& player = world.create_object("Player");
    const auto& camera = player.create_child("Camera");

    shinkou::editor::EditorUiModel model;
    model.sync(world);
    if (model.menus().size() < 6 || model.pages().size() < 8 || model.object_roots().size() != 1) return 1;
    const auto buildMenu = std::find_if(model.menus().begin(), model.menus().end(), [](const auto& menu) {
        return menu.id == "build";
    });
    if (buildMenu == model.menus().end() ||
        std::find_if(buildMenu->items.begin(), buildMenu->items.end(), [](const auto& item) {
            return item.command == shinkou::editor::EditorCommand::BuildProject;
        }) == buildMenu->items.end()) return 7;
    const auto stableRevision = model.revision();
    model.sync(world);
    if (model.revision() != stableRevision) return 5;
    shinkou::editor::EditorBuildUiState buildState;
    buildState.profileName = "CMake Debug";
    buildState.status = "Running: CMake Debug";
    buildState.running = true;
    buildState.compileCommandCount = 2;
    buildState.compileCommandsStatus = "Imported 2 commands";
    buildState.tools.push_back({"CMake", "cmake.exe", true});
    model.set_build_state(buildState);
    const auto buildRevision = model.revision();
    model.set_build_state(buildState);
    if (model.revision() != buildRevision || !model.build_state().running || model.build_state().tools.size() != 1 ||
        model.build_state().compileCommandCount != 2) return 8;
    shinkou::editor::EditorAssetPreviewUiState preview;
    preview.path = "assets/main.cpp";
    preview.kind = "Text";
    preview.title = "Text Resource";
    preview.status = "Text preview";
    preview.textLines = {"int main() {}"};
    model.set_asset_preview(preview);
    const auto previewRevision = model.revision();
    model.set_asset_preview(preview);
    if (model.revision() != previewRevision || model.asset_preview().textLines.size() != 1) return 9;
    shinkou::editor::EditorMediaUiState media;
    media.path = "assets/music.wav";
    media.kind = "Audio";
    media.status = "AudioSystem connected";
    media.playbackState = "playing";
    media.available = true;
    media.loop = true;
    model.set_media_state(media);
    const auto mediaRevision = model.revision();
    model.set_media_state(media);
    if (model.revision() != mediaRevision || model.media_state().path != "assets/music.wav" ||
        model.media_state().playbackState != "playing") return 10;
    shinkou::editor::EditorFileRecoveryUiState recovery;
    recovery.externalChangeCount = 1;
    recovery.latestExternalBatchId = 7;
    recovery.externalBatchStatus = "Batch #7: 1 external path(s) require review";
    recovery.externalChanges.push_back({"assets/external-editor.txt", "Modified", 7});
    model.set_file_recovery_state(recovery);
    const auto recoveryRevision = model.revision();
    model.set_file_recovery_state(recovery);
    if (model.revision() != recoveryRevision || model.file_recovery_state().latestExternalBatchId != 7 ||
        model.file_recovery_state().externalChanges.front().batchId != 7) return 11;
    auto boundedRecovery = recovery;
    boundedRecovery.externalChanges.clear();
    for (std::size_t index = 0; index < 65; ++index)
        boundedRecovery.externalChanges.push_back({"assets/external-" + std::to_string(index) + ".txt", "Added", 8});
    boundedRecovery.externalChangeCount = boundedRecovery.externalChanges.size();
    boundedRecovery.latestExternalBatchId = 8;
    model.set_file_recovery_state(std::move(boundedRecovery));
    if (model.file_recovery_state().externalChanges.size() != 64) return 12;
    player.set_name("Renamed externally"); player.set_active(false); model.sync(world);
    if (model.object_roots().front().name != "Renamed externally" || model.object_roots().front().active) return 6;

    model.select_object(camera.id());
    model.sync(world);
    if (!has_selected(model.object_roots().front(), camera.id())) return 2;

    model.set_object_filter("camera");
    model.sync(world);
    if (model.object_roots().size() != 1 || model.object_roots().front().children.empty()) return 3;

    model.execute(shinkou::editor::EditorCommand::Play);
    if (!model.playing() || model.paused()) return 4;
    std::cout << "EditorUiModel object tree and command model passed\n";
    return 0;
}
