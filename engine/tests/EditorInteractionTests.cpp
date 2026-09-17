#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/audio/AudioSceneSystem.h"
#include "shinkou/World.h"
#include "shinkou/render/RenderScene.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace shinkou;
using namespace shinkou::editor;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void append_wav_u16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xffu));
}

void append_wav_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
}

std::vector<unsigned char> make_audio_fixture_wav() {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t sampleCount = 256;
    constexpr std::uint32_t dataBytes = sampleCount * channels * bits / 8u;
    std::vector<unsigned char> bytes;
    bytes.reserve(44u + dataBytes);
    const auto chunk = [&bytes](const char* text) {
        for (int index = 0; index < 4; ++index)
            bytes.push_back(static_cast<unsigned char>(text[index]));
    };
    chunk("RIFF");
    append_wav_u32(bytes, 36u + dataBytes);
    chunk("WAVE");
    chunk("fmt ");
    append_wav_u32(bytes, 16u);
    append_wav_u16(bytes, 1u);
    append_wav_u16(bytes, channels);
    append_wav_u32(bytes, sampleRate);
    append_wav_u32(bytes, sampleRate * channels * bits / 8u);
    append_wav_u16(bytes, channels * bits / 8u);
    append_wav_u16(bytes, bits);
    chunk("data");
    append_wav_u32(bytes, dataBytes);
    for (std::uint32_t index = 0; index < sampleCount; ++index) {
        const auto sample = (index / 32u) % 2u == 0u ? 26000 : -26000;
        append_wav_u16(bytes, static_cast<std::uint16_t>(sample));
    }
    return bytes;
}

class TestInput final : public input::IInputBackend {
public:
    std::vector<input::InputEvent> queue, frame;
    input::MouseState mouse{};
    std::vector<input::GamepadState> pads;
    bool initialize() override { return true; }
    void poll() override { frame = std::move(queue); queue.clear(); }
    float control_value(std::string_view) const override { return 0; }
    const input::MouseState& mouse_state() const override { return mouse; }
    const std::vector<input::GamepadState>& gamepads() const override { return pads; }
    const std::vector<input::InputEvent>& events() const override { return frame; }
};

class TestAudioBackend final : public audio::IAudioBackend {
    struct Voice {
        audio::AudioVoiceId id{0};
        audio::AudioBus bus{audio::AudioBus::Master};
        audio::AudioVoiceState state{audio::AudioVoiceState::Invalid};
    };
    std::vector<Voice> voices_;
    std::unordered_map<audio::AudioVoiceId, double> cursors_;

    Voice* find(audio::AudioVoiceId id) noexcept {
        for (auto& voice : voices_) if (voice.id == id) return &voice;
        return nullptr;
    }
    const Voice* find(audio::AudioVoiceId id) const noexcept {
        for (const auto& voice : voices_) if (voice.id == id) return &voice;
        return nullptr;
    }

public:
    bool initialize(const audio::AudioConfig&) override { return true; }
    void shutdown() override { voices_.clear(); cursors_.clear(); }
    void update(Seconds) override {}
    audio::AudioAssetInfo inspect_asset(const audio::AudioAssetDesc& asset) const override {
        return {asset.streaming, true, 30.0, true};
    }
    audio::AudioVoiceId play(const audio::AudioAssetDesc&, const audio::AudioPlayParams& params) override {
        const auto id = audio::make_audio_handle(static_cast<std::uint32_t>(voices_.size()), 1);
        voices_.push_back({id, params.bus, params.startPaused ? audio::AudioVoiceState::Paused : audio::AudioVoiceState::Playing});
        cursors_[id] = 0.0;
        return id;
    }
    void stop(audio::AudioVoiceId id, Seconds) override { if (auto* voice = find(id)) voice->state = audio::AudioVoiceState::Stopped; }
    void pause(audio::AudioVoiceId id) override { if (auto* voice = find(id); voice && voice->state == audio::AudioVoiceState::Playing) voice->state = audio::AudioVoiceState::Paused; }
    void resume(audio::AudioVoiceId id) override { if (auto* voice = find(id); voice && voice->state == audio::AudioVoiceState::Paused) voice->state = audio::AudioVoiceState::Playing; }
    void seek(audio::AudioVoiceId id, double seconds) override { if (find(id)) cursors_[id] = std::max(0.0, seconds); }
    double cursor_seconds(audio::AudioVoiceId id) const override {
        const auto found = cursors_.find(id);
        return found == cursors_.end() ? 0.0 : found->second;
    }
    bool supports_cursor() const noexcept override { return true; }
    void set_volume(audio::AudioVoiceId, float) override {}
    void set_pitch(audio::AudioVoiceId, float) override {}
    void set_pan(audio::AudioVoiceId, float) override {}
    void set_spatial(audio::AudioVoiceId, const audio::AudioPlayParams&) override {}
    void set_listener(const audio::AudioListener&) override {}
    void set_bus_volume(audio::AudioBus, float) override {}
    void set_bus_muted(audio::AudioBus, bool) override {}
    audio::AudioTrackId create_track(const audio::AudioTrackDesc&) override { return 0; }
    void destroy_track(audio::AudioTrackId) override {}
    void set_track_volume(audio::AudioTrackId, float) override {}
    void set_track_muted(audio::AudioTrackId, bool) override {}
    audio::AudioTrackSnapshot track_snapshot(audio::AudioTrackId) const override { return {}; }
    audio::AudioVoiceState state(audio::AudioVoiceId id) const override {
        const auto* voice = find(id);
        return voice ? voice->state : audio::AudioVoiceState::Invalid;
    }
    std::uint32_t collect_finished(audio::AudioVoiceId*, std::uint32_t) override { return 0; }
    void stop_all(audio::AudioBus bus, Seconds) override {
        for (auto& voice : voices_) if (bus == audio::AudioBus::Master || voice.bus == bus)
            voice.state = audio::AudioVoiceState::Stopped;
    }
    std::string last_error() const override { return {}; }
    audio::AudioDiagnostics diagnostics() const override {
        audio::AudioDiagnostics result;
        for (const auto& voice : voices_)
            if (voice.state == audio::AudioVoiceState::Playing || voice.state == audio::AudioVoiceState::Paused)
                ++result.activeVoices;
        return result;
    }
};

class ModelArtifactProcessor final : public assets::IAssetProcessor {
public:
    bool process(const assets::AssetProcessContext& context, assets::AssetArtifact& output,
                 std::string&) const override {
        if (std::filesystem::path(context.key.uri).extension() == ".gltf") {
            output.payload = context.sourceBytes;
            output.format = "model";
            return true;
        }
        static constexpr char imported[] =
            "o AssetSystemImported\n"
            "v -4 -2 0\n"
            "v 4 -2 0\n"
            "v 0 4 0\n"
            "f 1 2 3\n";
        output.payload.assign(imported, imported + sizeof(imported) - 1);
        output.format = "model";
        return true;
    }
};

class TextureArtifactProcessor final : public assets::IAssetProcessor {
public:
    bool process(const assets::AssetProcessContext&, assets::AssetArtifact& output,
                 std::string&) const override {
        static constexpr unsigned char png[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
            0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54,
            0x78, 0x9c, 0x63, 0x64, 0x00, 0x02, 0x00, 0x00,
            0x05, 0x00, 0x01, 0xe9, 0x8d, 0x5d, 0x3c,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
            0xae, 0x42, 0x60, 0x82};
        output.payload.assign(std::begin(png), std::end(png));
        output.format = "texture";
        return true;
    }
};
}

int main() {
    try {
        PropertyValue value;
        require(!parse_property_text(PropertyType::Vec3,"1 2 nan",value),"non-finite vector accepted");
        require(!parse_property_text(PropertyType::Number,"2junk",value),"trailing numeric garbage accepted");
        require(!parse_property_text(PropertyType::UnsignedInteger,"-1",value),"negative unsigned accepted");
        require(parse_property_text(PropertyType::Vec3,"1.5 -2 3",value),"valid vector rejected");
        World world;
        auto& root = world.create_object("Root");
        root.create_child("Child").add_component<components::LifetimeComponent>()->remaining = 9;
        root.add_component<components::LifetimeComponent>(); // -1 means never expire; must round-trip.
        auto* tags = root.add_component<components::TagComponent>(); tags->add("hero");
        EditorDocument document; std::string error, json;
        require(EditorDocument::capture(world,root.id(),document,error),"snapshot failed");
        require(document.to_json(json,error),"serialize failed");
        EditorDocument decoded;
        require(EditorDocument::from_json(json,decoded,error),"deserialize failed");
        ObjectId selected{};
        require(decoded.restore(world,selected,error),"restore failed");
        require(world.object_count()==2 && world.find_object(selected)->name()=="Root","hierarchy round trip failed");
        require(world.find_object(selected)->get_component<components::TagComponent>()->has("hero"),"tags lost");
        auto malformed = decoded; malformed.objects[1].parent=1;
        require(!malformed.restore(world,selected,error) && world.object_count()==2,"malformed scene damaged current world");
        malformed=decoded; malformed.objects[0].components[0].properties[0].value="nan 0 0";
        require(!malformed.restore(world,selected,error) && world.object_count()==2,"invalid setter damaged world");

        const auto project = std::filesystem::temp_directory_path()/ ("shinkou-ui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(project/"assets/Folder/Nested");
        std::filesystem::create_directories(project/"assets/Scenes");
        std::ofstream(project/"assets/Folder/Nested/needle.txt") << "fixture";
        const auto previewWav = make_audio_fixture_wav();
        {
            std::ofstream file(project/"assets/preview.wav", std::ios::binary);
            file.write(reinterpret_cast<const char*>(previewWav.data()),
                       static_cast<std::streamsize>(previewWav.size()));
        }
        std::ofstream(project/"assets/preview.avi", std::ios::binary) << "not a decoded video fixture";
        std::ofstream(project/"assets/preview.obj") <<
            "o Triangle\n"
            "v -1 -1 0\n"
            "v 1 -1 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n";
        std::ofstream(project/"assets/preview.gltf") << R"json({
          "asset":{"version":"2.0"},
          "buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAQABAAIA"}],
          "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
          "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
          "images":[
            {"name":"AlbedoA","uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/AAAADAQEAyf6S7wAAAABJRU5ErkJggg==","mimeType":"image/png"},
            {"name":"AlbedoB","uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/AAAADAQEAyf6S7wAAAABJRU5ErkJggg==","mimeType":"image/png"}
          ],
          "samplers":[{}],
          "textures":[{"name":"TextureA","source":0,"sampler":0},{"name":"TextureB","source":1,"sampler":0}],
          "materials":[
            {"name":"MaterialA","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},
            {"name":"MaterialB","pbrMetallicRoughness":{"baseColorTexture":{"index":1}}}
          ],
          "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}]
        })json";
        std::ofstream(project/"assets/Folder-extra.txt") << "sibling must follow the complete subtree";
        std::ofstream(project/"assets/preview.png", std::ios::binary) << "AssetSystem texture source";
        std::ofstream(project/"CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\n";
        std::ofstream(project/"Sample.sln") << "Microsoft Visual Studio Solution File\n";
        std::ofstream(project/"compile_commands.json") << R"json([{"directory":".","file":"assets/Folder/Nested/needle.txt","arguments":["clang++","-c","assets/Folder/Nested/needle.txt"]}])json";
        for (int i=0;i<100;++i) std::ofstream(project/("assets/item"+std::to_string(i)+".txt")) << i;
        assets::AssetSystemConfig assetConfig;
        assetConfig.projectRoot = project;
        assetConfig.cacheRoot = project / ".shinkou" / "cache";
        assetConfig.workerCount = 2;
        assetConfig.memoryBudgetBytes = 16u * 1024u * 1024u;
        assets::AssetSystem resourceSystem(assetConfig);
        resourceSystem.register_processor("model", std::make_unique<ModelArtifactProcessor>());
        resourceSystem.register_processor("texture", std::make_unique<TextureArtifactProcessor>());
        require(resourceSystem.initialize(), "asset system initialization failed");
        require(resourceSystem.write_manifest(project / ".shinkou" / "manifest.json"),
                "asset system could not write the editor manifest fixture");
        assets::AssetId previewModelAssetId = 0;
        assets::AssetId previewAudioAssetId = 0;
        assets::AssetId needleAssetId = 0;
        assets::AssetId folderExtraAssetId = 0;
        for (const auto& entry : resourceSystem.scan_sources()) {
            if (entry.key.type == "model" && entry.sourcePath.filename() == "preview.obj") {
                previewModelAssetId = entry.id;
            }
            if (entry.key.type == "audio" && entry.sourcePath.filename() == "preview.wav") previewAudioAssetId = entry.id;
            if (std::filesystem::relative(entry.sourcePath, project).generic_string() ==
                "assets/Folder/Nested/needle.txt") needleAssetId = entry.id;
            if (std::filesystem::relative(entry.sourcePath, project).generic_string() ==
                "assets/Folder-extra.txt") folderExtraAssetId = entry.id;
        }
        require(previewModelAssetId != 0 && previewAudioAssetId != 0 && needleAssetId != 0 &&
                folderExtraAssetId != 0,
                "asset system manifest did not index the model/audio fixtures");
        World unloadedDocumentWorld;
        unloadedDocumentWorld.register_component_type<AssetReferenceComponent>("AssetReference");
        auto& unloadedRenameObject = unloadedDocumentWorld.create_object("Unloaded Rename Reference");
        require(unloadedRenameObject.add_component<AssetReferenceComponent>(
                    "assets/Folder/Nested/needle.txt", needleAssetId) != nullptr,
                "could not create unloaded rename document fixture");
        auto& unloadedDeleteObject = unloadedDocumentWorld.create_object("Unloaded Delete Reference");
        require(unloadedDeleteObject.add_component<AssetReferenceComponent>(
                    "assets/Folder-extra.txt", folderExtraAssetId) != nullptr,
                "could not create unloaded delete document fixture");
        EditorDocument unloadedDocument;
        std::string unloadedDocumentJson;
        std::string unloadedDocumentError;
        require(EditorDocument::capture(unloadedDocumentWorld, unloadedRenameObject.id(), unloadedDocument,
                                         unloadedDocumentError) &&
                    unloadedDocument.to_json(unloadedDocumentJson, unloadedDocumentError),
                "could not serialize unloaded scene/prefab fixture");
        {
            std::ofstream file(project / "assets/Scenes/Unloaded.prefab", std::ios::binary);
            file << unloadedDocumentJson;
        }
        EditorBuildProfile alternateProfile;
        alternateProfile.id = "release";
        alternateProfile.name = "CMake Release";
        alternateProfile.configuration = "Release";
        EditorBuildProfileSet profileSet;
        profileSet.profiles = {EditorBuildProfile{}, alternateProfile};
        profileSet.selectedId = "default";
        std::string profileSetJson;
        std::string profileSetError;
        require(EditorBuildProfileStore::serialize_set(profileSet, profileSetJson, &profileSetError),
                "could not create multi-profile fixture");
        std::ofstream(project / ".shinkou" / "build-profile.json") << profileSetJson;
        audio::AudioConfig audioConfig;
        audioConfig.assetRoot = project;
        audioConfig.startDevice = false;
        audio::AudioSystem audioSystem(std::make_unique<TestAudioBackend>(), audioConfig);
        require(audioSystem.initialize(), "editor audio test backend initialization failed");
        audio::AudioSceneSystem audioScene;
        EditorLayer editor; editor.set_project_root(project.generic_string());
        editor.set_layout_path((project/"Saved/layout.json").generic_string());
        require(editor.initialize(false),"editor initialization failed");
        editor.set_audio_system(&audioSystem);
        editor.set_audio_scene_system(&audioScene);
        editor.set_asset_system(&resourceSystem);
        editor.set_display_size(1280,720,1);
        render::Renderer renderer;
        auto backend = std::make_unique<TestInput>(); auto* fake=backend.get();
        input::InputSystem input(std::move(backend)); input.initialize();
        FrameIndex frame=0;
        auto tick = [&] { input.poll(); editor.process_input(input,world); editor.prepare_frame(renderer,world); editor.draw(renderer,world,1.0f/60,frame++); };
        auto require_unloaded_reference = [&](const std::string& expectedPath) {
            FileSystemService files(project);
            std::string documentJson, documentError;
            EditorDocument document;
            require(files.read_text("assets/Scenes/Unloaded.prefab", documentJson, &documentError) &&
                        EditorDocument::from_json(documentJson, document, documentError),
                    "unloaded scene/prefab document could not be read after asset operation");
            bool found = false;
            for (const auto& object : document.objects) {
                for (const auto& component : object.components) {
                    if (component.type != "AssetReference") continue;
                    std::string pathValue;
                    std::string assetIdValue;
                    for (const auto& property : component.properties) {
                        if (property.name == "path") pathValue = property.value;
                        else if (property.name == "assetId") assetIdValue = property.value;
                    }
                    if (pathValue == expectedPath) {
                        found = true;
                        require(assetIdValue == "0", "unloaded reference retained a stale AssetId");
                    }
                }
            }
            require(found, "unloaded scene/prefab reference was not migrated");
        };
        auto region = [&](const std::string& id) {
            const auto& regions=editor.editor_ui().interaction_regions();
            auto it=regions.find(id);
            if (it==regions.end() || it->second.width<=0 || it->second.height<=0) throw std::runtime_error("missing region: "+id);
            return it->second;
        };
        auto click = [&](const std::string& id) {
            const auto r=region(id); fake->mouse.position={r.x+r.width*0.5f,r.y+r.height*0.5f};
            input::InputEvent e; e.type=input::InputEventType::MouseButtonDown; e.control="mouse:left"; e.position=fake->mouse.position;
            fake->queue.push_back(e); e.type=input::InputEventType::MouseButtonUp; fake->queue.push_back(e); tick();
        };
        auto key = [&](const char* name) { input::InputEvent e; e.type=input::InputEventType::KeyDown;e.control=std::string("key:")+name;fake->queue.push_back(e);tick(); };
        auto text = [&](const char* text) { input::InputEvent e;e.type=input::InputEventType::TextInput;e.text=text;fake->queue.push_back(e);tick(); };
        for(int i=0;i<200 && editor.ui_asset_file_count()<100;++i) {tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        require(editor.ui_asset_file_count()>100,"async file scan did not finish");
        auto& referenceObject = world.create_object("Needle Reference");
        auto* needleReference = referenceObject.add_component<AssetReferenceComponent>(
            "assets/Folder/Nested/needle.txt", needleAssetId);
        require(needleReference != nullptr, "could not create asset rename migration fixture");
        auto& deletedReferenceObject = world.create_object("Deleted Asset Reference");
        auto* deletedReference = deletedReferenceObject.add_component<AssetReferenceComponent>(
            "assets/Folder-extra.txt", folderExtraAssetId);
        require(deletedReference != nullptr, "could not create asset delete invalidation fixture");
        tick();
        const auto camera = world.ecs().create();
        world.ecs().emplace<render::CameraComponent>(camera);
        render::TransformComponent cameraTransform; cameraTransform.local.position={0,0,-5};
        world.ecs().emplace<render::TransformComponent>(camera,cameraTransform);
        tick();
        const auto viewport = region("viewport.surface");
        input::InputEvent gesture; gesture.type=input::InputEventType::MouseWheel;
        gesture.position={viewport.x+viewport.width/2,viewport.y+viewport.height/2}; gesture.delta.y=1;
        fake->queue.push_back(gesture);tick();
        auto* cameraPose=world.ecs().try_get<render::TransformComponent>(camera);
        require(cameraPose->local.position.z > -5,"viewport wheel did not move scene camera");
        gesture.type=input::InputEventType::MouseButtonDown;gesture.control="mouse:middle";fake->queue.push_back(gesture);tick();
        gesture.type=input::InputEventType::MouseMove;gesture.position.x+=40;gesture.position.y+=20;fake->queue.push_back(gesture);tick();
        gesture.type=input::InputEventType::MouseButtonUp;fake->queue.push_back(gesture);tick();
        require(std::abs(cameraPose->local.rotation.y)>0.01f,"viewport orbit did not rotate camera");
        editor.execute_command(EditorCommand::ProjectSettings,{},world);tick();
        click("command:theme-light"); require(editor.layout().theme=="light","settings theme command failed");
        const auto settings=region("tools:settings");
        gesture.type=input::InputEventType::MouseWheel;gesture.position={settings.x+10,settings.y+10};gesture.delta.y=-20;
        fake->queue.push_back(gesture);tick();
        click("command:save-layout"); require(std::filesystem::exists(editor.layout().layoutFile),"settings could not save layout");
        editor.set_panel_visible("settings",false);editor.set_theme("dark");tick();
        editor.execute_command(EditorCommand::TogglePage, "build", world); tick();
        require(editor.layout().showBuild && editor.panel_visible("build"), "build panel visibility was not persisted in the editor state");
        require(region("command:build-panel-build").width > 0 && region("command:build-panel-refresh").width > 0,
                "retained build panel controls were not registered");
        require(region("command:build-panel-open-vscode").width > 0 &&
                region("command:build-panel-save-profile").width > 0,
                "retained IDE/profile controls were not registered");
        require(region("command:build-panel-profile:default").width > 0 &&
                region("command:build-panel-profile:release").width > 0 &&
                region("command:build-diagnostic-filter:all").width > 0 &&
                region("command:build-diagnostic-filter:errors").width > 0 &&
                region("command:build-panel-refresh-project-files").width > 0 &&
                region("command:build-panel-associate-project-file").width > 0 &&
                region("command:build-panel-generate-clangd").width > 0 &&
                region("field:build-profile.name").width > 0 &&
                region("field:build-profile.buildDirectory").width > 0,
                "retained build profile editor controls were not registered");
        click("command:build-diagnostic-filter:errors");
        require(editor.diagnostic_filter() == "errors", "diagnostic filter command did not update editor state");
        click("command:build-diagnostic-filter:all");
        click("command:build-panel-refresh-project-files");
        require(editor.project_integration_status().find("Discovered") != std::string::npos &&
                editor.project_discovery().recommendedProjectFile == std::filesystem::path("Sample.sln"),
                "project discovery command did not expose the recommended solution");
        click("command:build-panel-associate-project-file");
        require(editor.build_profile().projectFile == std::filesystem::path("Sample.sln") &&
                editor.project_integration_status().find("Associated") != std::string::npos,
                "project association command did not update the selected profile");
        click("command:build-panel-generate-clangd");
        require(std::filesystem::exists(project / ".clangd") &&
                editor.clangd_config_status().find("Generated") != std::string::npos,
                "clangd config command did not write a project-root config");
        click("command:build-panel-profile:release");
        require(editor.selected_build_profile_id() == "release" && editor.build_profile().name == "CMake Release",
                "retained build profile selector did not switch the active profile");
        click("command:build-panel-profile:default");
        click("field:build-profile.name"); text("QA Profile"); key("Return");
        require(editor.build_profile().name == "QA Profile" &&
                editor.build_profile_status().find("Unsaved") != std::string::npos,
                "build profile field edit did not commit through the retained input path");
        editor.execute_command(EditorCommand::SaveBuildProfile, {}, world); tick();
        require(std::filesystem::exists(project / ".shinkou" / "build-profile.json"),
                "build profile command did not persist project configuration");
        require(editor.build_profile_status().find("Saved") != std::string::npos,
                "build profile save status was not published");
        editor.execute_command(EditorCommand::ReloadBuildProfile, {}, world); tick();
        require(editor.build_profile_status().find("Loaded") != std::string::npos,
                "build profile reload command did not publish status");
        require(editor.build_profile().name == "QA Profile",
                "build profile field edit was not persisted by the profile set format");
        editor.execute_command(EditorCommand::ImportCompileCommands, {}, world); tick();
        require(editor.compile_command_count() == 1 && editor.compile_commands_status().find("Imported 1") != std::string::npos,
                "compile_commands import did not update the build model");
        editor.execute_command(EditorCommand::ExportCompileCommands, "Saved/exported-compile_commands.json", world);
        require(std::filesystem::exists(project/"Saved/exported-compile_commands.json"),
                "compile_commands export did not write through the project filesystem");
        editor.execute_command(EditorCommand::TogglePage, "build", world); tick();
        require(!editor.layout().showBuild && !editor.panel_visible("build"), "build panel toggle command failed");
        tick();click("asset-view:large"); require(editor.asset_view()==EditorAssetView::LargeIcons,"large view click failed");
        click("asset-view:tree");
        require(region("asset:assets/Folder/Nested/needle.txt").y < region("asset:assets/Folder-extra.txt").y,"tree sibling interrupted a directory subtree");
        click("asset-toggle:assets/Folder");
        click("assets.filter"); text("needle");
        require(editor.ui_visible_asset_file_count()==3,"search did not include collapsed ancestors");
        key("Return"); key("End"); tick();
        require(editor.editor_ui().selected_asset()=="assets/Folder/Nested/needle.txt","End navigation failed");
        for (int i = 0; i < 100 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().path == "assets/Folder/Nested/needle.txt" &&
                editor.asset_preview().kind == "Text" && !editor.asset_preview().textLines.empty() &&
                editor.asset_preview().textLines.front().find("fixture") != std::string::npos,
                "selected text resource did not produce an async preview");
        for (int i = 0; i < 100 && !editor.asset_preview().assetSystemReady; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().assetSystemReady &&
                editor.asset_preview().assetSystemStatus == "AssetSystem ready" &&
                editor.asset_preview().assetSystemFormat == "text" &&
                editor.asset_preview().assetSystemMetadataFormat == "shinkou.asset.text.v1" &&
                editor.asset_preview().assetSystemMetadataBytes > 0 &&
                editor.asset_preview().assetSystemSourceHash != 0,
                "selected resource did not publish the AssetSystem cache contract");
        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (editor.asset_system_manifest_count() == 0 ||
            editor.asset_system_manifest_status().find("AssetSystem manifest ready") == std::string::npos) {
            throw std::runtime_error("editor did not publish the AssetSystem manifest snapshot: count=" +
                std::to_string(editor.asset_system_manifest_count()) + " status=" +
                editor.asset_system_manifest_status());
        }
        require(editor.asset_system_manifest_status().find("validated cache") != std::string::npos,
                "editor did not validate and seed the persisted AssetSystem manifest");
        for (int i = 0; i < 8; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "ready AssetSystem manifest was restarted while the file tree was still settling");
        bool bridgeStatusVisible = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("AssetSystem ready") != std::string::npos) {
                bridgeStatusVisible = true;
                break;
            }
        }
        require(bridgeStatusVisible, "Inspector did not retain-paint the AssetSystem status");
        editor.execute_command(EditorCommand::OpenAsset, "assets/Folder/Nested/needle.txt", world); tick();
        require(editor.layout().showInspector && editor.asset_preview().path == "assets/Folder/Nested/needle.txt",
                "open asset command did not route to the inspector preview");
        editor.execute_command(EditorCommand::OpenAsset, "../outside.txt", world);
        require(editor.last_status().find("outside") != std::string::npos,
                "open asset command accepted a path outside the project root");
        key("F2"); text("renamed.txt"); key("Return");
        require(std::filesystem::exists(project/"assets/Folder/Nested/renamed.txt"),"real rename failed");
        require(needleReference->path() == "assets/Folder/Nested/renamed.txt",
                "asset rename did not migrate the live reference path");
        assets::AssetId renamedNeedleAssetId = 0;
        for (int i = 0; i < 200 && needleReference->asset_id() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (const auto& entry : resourceSystem.scan_sources()) {
            if (std::filesystem::relative(entry.sourcePath, project).generic_string() ==
                "assets/Folder/Nested/renamed.txt") renamedNeedleAssetId = entry.id;
        }
        require(renamedNeedleAssetId != 0 && renamedNeedleAssetId != needleAssetId &&
                needleReference->asset_id() == renamedNeedleAssetId,
                "asset rename did not rebind the live reference to the new manifest identity");
        require_unloaded_reference("assets/Folder/Nested/renamed.txt");
        needleReference->set_asset_id(0);
        editor.execute_command(EditorCommand::RefreshAssets, {}, world);
        for (int i = 0; i < 120 && needleReference->asset_id() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(needleReference->asset_id() == renamedNeedleAssetId,
                "manifest-ready rebind did not restore a path-only asset reference");
        click("assets.filter"); key("Left Ctrl"); key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace"); key("Return"); key("End");tick();
        require(editor.editor_ui().asset_scroll_offset()>0,"keyboard failed to reveal last item");
        const auto rail=region("assets.scrollbar");
        input::InputEvent e;e.type=input::InputEventType::MouseButtonDown;e.control="mouse:left";e.position={rail.x+6,rail.y+rail.height-3};fake->queue.push_back(e);tick();
        e.type=input::InputEventType::MouseMove;e.position={rail.x+6,rail.y};fake->queue.push_back(e);tick();
        e.type=input::InputEventType::MouseButtonUp;fake->queue.push_back(e);tick();
        require(editor.editor_ui().asset_scroll_offset()<30,"scrollbar drag failed");

        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "asset manifest did not settle before the AudioSource inspector interaction");

        editor.execute_command(EditorCommand::CreateEmpty,{},world); tick();
        const auto created=editor.layout().selectedObject;
        auto* audioSource = world.find_object(created)->add_component<components::AudioSourceComponent>();
        require(audioSource != nullptr, "could not create AudioSource inspector fixture");
        tick();
        const auto audioClipField = "field:" + std::to_string(audioSource->id()) + ":clipPath";
        const auto audioBusField = "field:" + std::to_string(audioSource->id()) + ":bus";
        const auto audioMinDistanceField = "field:" + std::to_string(audioSource->id()) + ":minDistance";
        const auto audioMaxDistanceField = "field:" + std::to_string(audioSource->id()) + ":maxDistance";
        const auto audioRolloffField = "field:" + std::to_string(audioSource->id()) + ":rolloff";
        require(region(audioClipField).width > 0 && region(audioBusField).width > 0 &&
                editor.editor_ui().interaction_regions().find("field:" + std::to_string(audioSource->id()) + ":assetId") ==
                    editor.editor_ui().interaction_regions().end(),
                "AudioSource inspector did not expose clip/bus/spatial fields");
        click(audioClipField);
        require(region("inspector-choice-popup").width > 0, "AudioSource clip picker did not open");
        std::string previewAudioChoice;
        for (const auto& entry : editor.editor_ui().interaction_regions()) {
            if (entry.first.rfind("inspector-choice:", 0) == 0) { previewAudioChoice = entry.first; break; }
        }
        require(!previewAudioChoice.empty(), "AudioSource clip picker did not expose manifest choices");
        click(previewAudioChoice);
        require(audioSource->clipPath == "assets/preview.wav" && audioSource->assetId == previewAudioAssetId,
                "AudioSource clip picker did not link path and AssetId");
        click(audioBusField);
        require(region("inspector-choice-popup").width > 0 &&
                editor.editor_ui().interaction_regions().find("inspector-choice:0") !=
                    editor.editor_ui().interaction_regions().end(),
                "AudioSource bus choice control did not open");
        click("inspector-choice:0");
        require(audioSource->bus == 0, "AudioSource bus choice did not commit");
        const auto inspectorSurface = region("inspector.background");
        gesture.type = input::InputEventType::MouseWheel;
        gesture.position = {inspectorSurface.x + inspectorSurface.width * 0.5f,
                            inspectorSurface.y + inspectorSurface.height * 0.5f};
        gesture.delta.y = -20.0f;
        fake->queue.push_back(gesture);
        tick();
        require(region(audioMinDistanceField).width > 0 && region(audioMaxDistanceField).width > 0 &&
                region(audioRolloffField).width > 0,
                "AudioSource inspector did not expose spatial fields after scrolling");
        click(audioMinDistanceField); text("12.5"); key("Return");
        click(audioMaxDistanceField); text("40"); key("Return");
        click(audioRolloffField); text("0.75"); key("Return");
        require(std::abs(audioSource->minDistance - 12.5f) < 0.001f &&
                std::abs(audioSource->maxDistance - 40.0f) < 0.001f &&
                std::abs(audioSource->rolloff - 0.75f) < 0.001f,
                "AudioSource spatial inspector fields did not commit");
        gesture.delta.y = 20.0f;
        fake->queue.push_back(gesture);
        tick();
        bool audioStatusVisible = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("Ready · assets/preview.wav") != std::string::npos) {
                audioStatusVisible = true;
                break;
            }
        }
        require(audioStatusVisible, "AudioSource inspector did not show linked resource status");
        const auto audioTarget = "audio-source:" + std::to_string(created);
        const auto audioPlayControl = "command:media-play:" + audioTarget;
        const auto audioPauseControl = "command:media-pause:" + audioTarget;
        const auto audioStopControl = "command:media-stop:" + audioTarget;
        const auto audioSeekBackControl = "command:media-seek:" + audioTarget + ":relative:-5";
        const auto audioSeekForwardControl = "command:media-seek:" + audioTarget + ":relative:5";
        require(region(audioPlayControl).width > 0 && region(audioPauseControl).width > 0 &&
                region(audioStopControl).width > 0,
                "AudioSource playback transport controls were not registered");
        click(audioPlayControl);
        require(audioScene.transport_state(created, audioSystem) == "Playing",
                "AudioSource Play did not start the scene voice");
        gesture.delta.y = -30.0f;
        fake->queue.push_back(gesture);
        tick();
        require(region(audioSeekBackControl).width > 0 && region(audioSeekForwardControl).width > 0,
                "AudioSource timeline seek controls were not registered");
        const auto audioTimelineRegion = "audio-timeline:" + std::to_string(created);
        require(region(audioTimelineRegion).width > 0, "AudioSource continuous timeline was not registered");
        bool audioWaveformRendered = false;
        for (int index = 0; index < 160 && !audioWaveformRendered; ++index) {
            tick();
            const auto timeline = region(audioTimelineRegion);
            std::size_t waveformColumns = 0;
            for (const auto& command : editor.editor_ui().render_list().commands()) {
                if (command.type != ui::DrawCommandType::Line ||
                    command.from.x <= timeline.x || command.from.x >= timeline.x + timeline.width ||
                    command.to.x != command.from.x ||
                    command.from.y < timeline.y - 2.0f ||
                    command.to.y > timeline.y + timeline.height + 2.0f) continue;
                ++waveformColumns;
            }
            audioWaveformRendered = waveformColumns >= 8;
            if (!audioWaveformRendered)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(audioWaveformRendered,
                "AudioSource inspector did not render the decoded waveform snapshot");
        click(audioSeekForwardControl);
        require(std::abs(audioScene.cursor_seconds(created, audioSystem) - 5.0) < 0.001,
                "AudioSource timeline seek did not move the scene voice cursor");
        click(audioTimelineRegion);
        require(std::abs(audioScene.cursor_seconds(created, audioSystem) - 15.0) < 0.001,
                "AudioSource continuous timeline seek did not target the clicked position");
        editor.execute_command(EditorCommand::MediaSeek, audioTarget + ":absolute:nan", world);
        require(editor.last_status().find("invalid") != std::string::npos,
                "AudioSource absolute seek accepted a non-finite payload");
        bool audioTimelineShowsDuration = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("15.00 / 30.00 s") != std::string::npos) {
                audioTimelineShowsDuration = true;
                break;
            }
        }
        require(audioTimelineShowsDuration, "AudioSource timeline did not show the resource duration");
        gesture.delta.y = 1000.0f;
        fake->queue.push_back(gesture);
        tick();
        click(audioPauseControl);
        require(audioScene.transport_state(created, audioSystem) == "Paused",
                "AudioSource Pause did not pause the scene voice");
        click(audioPlayControl);
        require(audioScene.transport_state(created, audioSystem) == "Playing",
                "AudioSource Play did not resume a paused scene voice");
        click(audioStopControl);
        require(audioScene.transport_state(created, audioSystem) == "Stopped",
                "AudioSource Stop did not release the scene voice");
        click("field:name");text("Edited Object");key("Return");
        require(world.find_object(created)->name()=="Edited Object","inspector did not commit name");
        std::string positionField;
        for (const auto& r:editor.editor_ui().interaction_regions()) if(r.first.find(":position")!=std::string::npos) positionField=r.first;
        require(!positionField.empty(),"missing transform input");click(positionField);text("3 4 5");key("Return");
        require(world.find_object(created)->get_component<components::TransformComponent>()->local.position.x==3,"transform edit failed");
        click(positionField);text("nan 4 5");key("Return");
        require(world.find_object(created)->get_component<components::TransformComponent>()->local.position.x==3,"invalid transform modified world");key("Escape");
        editor.execute_command(EditorCommand::Undo,{},world);tick();
        require(world.find_object(editor.layout().selectedObject)->get_component<components::TransformComponent>()->local.position.x==0,"undo failed");
        editor.execute_command(EditorCommand::Redo,{},world);tick();
        require(world.find_object(editor.layout().selectedObject)->get_component<components::TransformComponent>()->local.position.x==3,"redo failed");
        editor.execute_command(EditorCommand::SaveScene,"assets/Scenes/test.scene",world);
        require(std::filesystem::exists(project/"assets/Scenes/test.scene"),"save scene failed");
        editor.execute_command(EditorCommand::NewScene,{},world);require(world.object_count()==0,"new scene failed");
        editor.execute_command(EditorCommand::OpenScene,"assets/Scenes/test.scene",world);require(world.object_count()==5,"open scene failed");
        AssetReferenceComponent* liveDeletedReference = nullptr;
        world.each_object([&](GameObject& object) {
            auto* reference = object.get_component<AssetReferenceComponent>();
            if (reference && reference->path() == "assets/Folder-extra.txt") liveDeletedReference = reference;
        });
        require(liveDeletedReference != nullptr, "restored scene lost the delete invalidation fixture");
        editor.set_panel_visible("build", false);
        editor.set_panel_visible("viewport", true);
        editor.dock_workspace().activate_tab("viewport");
        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "asset manifest did not settle before the viewport drop interaction");
        tick();
        click("assets.filter");
        key("Left Ctrl");
        key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace");
        text("preview.obj");
        tick();
        const auto objectCountBeforeDrop = world.object_count();
        const auto assetSource = region("asset:assets/preview.obj");
        const auto viewportDropTarget = region("viewport.surface");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:left";
        gesture.position = {assetSource.x + assetSource.width * 0.5f,
                             assetSource.y + assetSource.height * 0.5f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position = {viewportDropTarget.x + viewportDropTarget.width * 0.75f,
                             viewportDropTarget.y + viewportDropTarget.height * 0.25f};
        fake->queue.push_back(gesture);
        tick();
        bool dropCueVisible = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Text && command.text.find("Drop preview.obj") != std::string::npos) {
                dropCueVisible = true;
                break;
            }
        }
        require(dropCueVisible, "asset drag did not retain a viewport drop cue");
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture);
        tick();
        require(world.object_count() == objectCountBeforeDrop + 1,
                "asset drag did not create a scene object");
        GameObject* droppedAssetObject = nullptr;
        world.each_object([&](GameObject& object) {
            if (object.get_component<AssetReferenceComponent>() != nullptr) droppedAssetObject = &object;
        });
        require(droppedAssetObject != nullptr &&
                droppedAssetObject->get_component<AssetReferenceComponent>()->path() == "assets/preview.obj" &&
                droppedAssetObject->get_component<AssetReferenceComponent>()->asset_id() == previewModelAssetId &&
                droppedAssetObject->get_component<components::TransformComponent>()->local.position.x > 1.0f &&
                droppedAssetObject->get_component<components::TransformComponent>()->local.position.y > 1.0f &&
                editor.last_status().find("Created asset reference") != std::string::npos,
                "asset drop did not publish a manifest identity, bounded reference and viewport position");
        EditorDocument droppedDocument;
        std::string droppedDocumentError;
        std::string droppedDocumentJson;
        require(EditorDocument::capture(world, editor.layout().selectedObject, droppedDocument, droppedDocumentError) &&
                droppedDocument.to_json(droppedDocumentJson, droppedDocumentError) &&
                droppedDocumentJson.find("assetId") != std::string::npos,
                "asset reference manifest identity was not serialized");
        editor.execute_command(EditorCommand::Undo, {}, world);
        require(world.object_count() == objectCountBeforeDrop, "asset drop was not undoable");
        editor.execute_command(EditorCommand::Redo, {}, world);
        require(world.object_count() == objectCountBeforeDrop + 1,
                "asset drop was not redoable");
        bool restoredDroppedAsset = false;
        world.each_object([&](GameObject& object) {
            const auto* reference = object.get_component<AssetReferenceComponent>();
            restoredDroppedAsset = restoredDroppedAsset ||
                (reference != nullptr && reference->path() == "assets/preview.obj");
        });
        require(restoredDroppedAsset, "asset reference was not restored by redo");
        bool restoredAssetId = false;
        world.each_object([&](GameObject& object) {
            const auto* reference = object.get_component<AssetReferenceComponent>();
            restoredAssetId = restoredAssetId ||
                (reference != nullptr && reference->path() == "assets/preview.obj" &&
                 reference->asset_id() == previewModelAssetId);
        });
        require(restoredAssetId, "asset reference manifest identity was not restored by redo");
        for (int i = 0; i < 160 && editor.model_scene_loaded_asset_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.model_scene_loaded_asset_count() == 1,
                "AssetId-backed model reference did not enter the asynchronous scene-instance cache");
        require(editor.model_scene_instance_count() == 0 &&
                editor.model_scene_status().find("GPU model scene fallback") != std::string::npos,
                "Null renderer did not expose the model scene backend fallback honestly");
        click("assets.filter");
        key("Left Ctrl");
        key("A");
        {input::InputEvent e;e.type=input::InputEventType::KeyUp;e.control="key:Left Ctrl";fake->queue.push_back(e);tick();}
        key("Backspace");
        key("Return");
        tick();
        const auto objectCountBeforeRejectedDrop = world.object_count();
        const auto unsupportedSource = region("asset:assets/Folder-extra.txt");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:left";
        gesture.position = {unsupportedSource.x + unsupportedSource.width * 0.5f,
                             unsupportedSource.y + unsupportedSource.height * 0.5f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position = {viewportDropTarget.x + viewportDropTarget.width * 0.25f,
                             viewportDropTarget.y + viewportDropTarget.height * 0.25f};
        fake->queue.push_back(gesture);
        tick();
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture);
        tick();
        require(world.object_count() == objectCountBeforeRejectedDrop &&
                editor.last_status().find("not instantiable") != std::string::npos,
                "unsupported asset drop changed the scene");
        auto rightClick = [&](const std::string& id) {
            const auto r = region(id);
            fake->mouse.position = {r.x + r.width * 0.5f, r.y + r.height * 0.5f};
            input::InputEvent right;
            right.type = input::InputEventType::MouseButtonDown;
            right.control = "mouse:right";
            right.position = fake->mouse.position;
            fake->queue.push_back(right);
            tick();
        };
        rightClick("asset:assets/Folder-extra.txt");
        click("asset-context:2");
        click("asset-delete-confirm");
        bool deletedReferenceInvalidated = false;
        world.each_object([&](GameObject& object) {
            const auto* reference = object.get_component<AssetReferenceComponent>();
            deletedReferenceInvalidated = deletedReferenceInvalidated ||
                (reference != nullptr && reference->path() == "assets/Folder-extra.txt" &&
                 reference->asset_id() == 0);
        });
        require(!std::filesystem::exists(project / "assets/Folder-extra.txt") && deletedReferenceInvalidated,
                "asset delete did not invalidate the live reference identity");
        bool recycledDeleteFound = false;
        if (std::filesystem::exists(project / ".shinkou" / "recycle")) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(project / ".shinkou" / "recycle")) {
                if (entry.path().filename() == "Folder-extra.txt") recycledDeleteFound = true;
            }
        }
        require(recycledDeleteFound, "asset delete did not retain a recoverable recycle entry");
        for (const auto& entry : resourceSystem.scan_sources()) {
            const auto relative = std::filesystem::relative(entry.sourcePath, project).generic_string();
            require(relative.rfind(".shinkou/", 0) != 0,
                    "editor recycle metadata leaked into the AssetSystem manifest");
        }
        require_unloaded_reference("assets/Folder-extra.txt");
        auto read_file_history = [&]() {
            FileSystemService historyFiles(project);
            FileHistoryDocument history;
            std::string historyJson, historyError;
            require(historyFiles.read_text(".shinkou/file-history.json", historyJson, &historyError) &&
                        FileHistoryDocument::from_json(historyJson, history, historyError),
                    "file operation journal could not be read");
            return history;
        };
        const auto deletedHistory = read_file_history();
        require(deletedHistory.entries.size() == 1 &&
                    deletedHistory.entries.front().kind == "recycle-delete" &&
                    deletedHistory.entries.front().sourcePath == "assets/Folder-extra.txt",
                "file delete did not persist a validated operation journal");
        EditorLayer restartedEditor;
        restartedEditor.set_project_root(project.generic_string());
        require(restartedEditor.initialize(false) && restartedEditor.file_history_undo_count() == 1,
                "a new editor session did not load the recoverable file journal");
        AssetReferenceComponent* currentDeletedReference = nullptr;
        world.each_object([&](GameObject& object) {
            auto* reference = object.get_component<AssetReferenceComponent>();
            if (reference && reference->path() == "assets/Folder-extra.txt") currentDeletedReference = reference;
        });
        require(currentDeletedReference != nullptr, "live delete reference disappeared before file Undo");
        editor.execute_command(EditorCommand::Undo, {}, world);
        require(std::filesystem::exists(project / "assets/Folder-extra.txt"),
                "file Undo did not restore the deleted resource");
        for (int i = 0; i < 400 &&
                    (currentDeletedReference->asset_id() == 0 || editor.asset_system_manifest_count() == 0); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assets::AssetId restoredManifestId = 0;
        for (const auto& entry : resourceSystem.scan_sources()) {
            if (std::filesystem::relative(entry.sourcePath, project).generic_string() ==
                "assets/Folder-extra.txt") restoredManifestId = entry.id;
        }
        assets::AssetId editorManifestId = 0;
        std::string editorManifestUri;
        std::string editorManifestType;
        for (const auto& entry : editor.asset_system_manifest()) {
            if (std::filesystem::relative(entry.sourcePath, project).generic_string() ==
                "assets/Folder-extra.txt") {
                editorManifestId = entry.id;
                editorManifestUri = entry.key.uri;
                editorManifestType = entry.key.type;
            }
        }
        require(currentDeletedReference->asset_id() == folderExtraAssetId,
                ("file Undo did not rebind the restored resource identity: id=" +
                 std::to_string(currentDeletedReference->asset_id()) +
                 " expected=" + std::to_string(folderExtraAssetId) +
                 " rescanned=" + std::to_string(restoredManifestId) +
                 " editor=" + std::to_string(editorManifestId) +
                 " uri=" + editorManifestUri + " type=" + editorManifestType +
                 " manifest=" + std::to_string(editor.asset_system_manifest_count()) +
                 " status=" + editor.asset_system_manifest_status()).c_str());
        require(read_file_history().entries.empty(),
                "file Undo did not persist the cleared operation journal");
        editor.execute_command(EditorCommand::Redo, {}, world);
        require(!std::filesystem::exists(project / "assets/Folder-extra.txt") &&
                    currentDeletedReference->asset_id() == 0,
                "file Redo did not reapply the recoverable delete");
        require(read_file_history().entries.size() == 1 &&
                    read_file_history().entries.front().kind == "recycle-delete",
                "file Redo did not persist the recoverable operation journal");
        for (int i = 0; i < 160 && editor.asset_system_manifest_count() == 0; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_system_manifest_count() > 0 &&
                editor.asset_system_manifest_status().find("AssetSystem manifest ready") != std::string::npos,
                "asset manifest did not settle after reference-invalidating delete");
        editor.execute_command(EditorCommand::OpenScene, "assets/Scenes/Unloaded.prefab", world);
        AssetReferenceComponent* reopenedRenamedReference = nullptr;
        AssetReferenceComponent* reopenedDeletedReference = nullptr;
        world.each_object([&](GameObject& object) {
            auto* reference = object.get_component<AssetReferenceComponent>();
            if (!reference) return;
            if (reference->path() == "assets/Folder/Nested/renamed.txt") reopenedRenamedReference = reference;
            if (reference->path() == "assets/Folder-extra.txt") reopenedDeletedReference = reference;
        });
        require(reopenedRenamedReference != nullptr && reopenedDeletedReference != nullptr,
                "reopening migrated prefab lost one of its structured references");
        require(reopenedRenamedReference->asset_id() == renamedNeedleAssetId &&
                reopenedDeletedReference->asset_id() == 0,
                "reopening migrated prefab did not reconcile manifest and missing AssetIds");
        editor.execute_command(EditorCommand::OpenScene, "assets/Scenes/test.scene", world);
        require(world.object_count() == 5, "reopening the active scene after prefab rebind damaged the scene");
        const auto objectCountBeforeBoundaryDrops = world.object_count();
        require(!editor.create_asset_reference_at_viewport(
                    world, "assets/Folder", {viewportDropTarget.x + 10.0f, viewportDropTarget.y + 10.0f}) &&
                editor.last_status().find("folders cannot be instantiated") != std::string::npos,
                "directory asset drop was not rejected at the editor boundary");
        require(!editor.create_asset_reference_at_viewport(
                    world, "../outside.txt", {viewportDropTarget.x + 10.0f, viewportDropTarget.y + 10.0f}) &&
                editor.last_status().find("outside the project") != std::string::npos,
                "out-of-root asset drop was not rejected at the editor boundary");
        require(!editor.create_asset_reference_at_viewport(
                    world, "assets/preview.obj", {viewportDropTarget.x - 1.0f, viewportDropTarget.y}) &&
                editor.last_status().find("position is invalid") != std::string::npos &&
                world.object_count() == objectCountBeforeBoundaryDrops,
                "invalid viewport coordinate changed the scene");
        const auto objectCountBeforeNativeDrop = world.object_count();
        require(editor.create_asset_reference_from_window_drop(
                    world, (project / "assets/preview.obj").generic_string(),
                    {viewportDropTarget.x + 20.0f, viewportDropTarget.y + 20.0f}) &&
                world.object_count() == objectCountBeforeNativeDrop + 1 &&
                editor.last_status().find("Created asset reference") != std::string::npos,
                "native absolute file drop did not route through the validated editor ingress");
        const auto objectCountAfterNativeDrop = world.object_count();
        require(!editor.create_asset_reference_from_window_drop(
                    world, (project.parent_path() / "outside-native.obj").generic_string(),
                    {viewportDropTarget.x + 20.0f, viewportDropTarget.y + 20.0f}) &&
                world.object_count() == objectCountAfterNativeDrop &&
                editor.last_status().find("native path") != std::string::npos,
                "native file drop boundary accepted an outside-project path");
        editor.execute_command(EditorCommand::Undo, {}, world);
        require(world.object_count() == objectCountBeforeNativeDrop,
                "native file drop did not remain undoable through the shared editor transaction");
        tick();
        require(!editor.consume_simulation_step(),"edit mode advances simulation");
        key("Left Ctrl");
        { input::InputEvent lost; lost.type=input::InputEventType::FocusLost; fake->queue.push_back(lost); tick(); }
        click("field:name"); text("After focus loss"); key("Return");
        require(world.find_object(editor.layout().selectedObject)->name()=="After focus loss","focus loss left Ctrl latched");
        editor.execute_command(EditorCommand::Play,{},world);require(editor.consume_simulation_step(),"play failed");
        editor.execute_command(EditorCommand::Pause,{},world);require(!editor.consume_simulation_step(),"pause failed");
        editor.execute_command(EditorCommand::Step,{},world);require(editor.consume_simulation_step()&&!editor.consume_simulation_step(),"step did not advance exactly one frame");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.wav", world);
        for (int i = 0; i < 100 && editor.media_preview().kind != "Audio"; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.media_preview().kind == "Audio" && editor.media_preview().path == "assets/preview.wav" &&
                editor.media_preview().available && editor.media_preview().status == "AudioSystem connected",
                "audio asset did not expose the connected AudioSystem preview state");
        editor.set_panel_visible("media", false);
        editor.execute_command(EditorCommand::TogglePage, "media", world); tick();
        require(region("command:media-play").width > 0 && region("command:media-stop").width > 0,
                "audio media transport controls were not registered");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.png", world);
        for (int i = 0; i < 120 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().kind == "Image", "image asset did not enter the image preview state");
        if (editor.asset_preview().imageSnapshot) {
            require(editor.asset_preview().imageSnapshot->valid(),
                    "image asset produced an invalid decoded thumbnail snapshot");
        bool imageCommand = false;
        for (const auto& command : editor.editor_ui().render_list().commands()) {
            if (command.type == ui::DrawCommandType::Image && command.imageSnapshot) {
                imageCommand = true;
                break;
            }
        }
        require(imageCommand, "retained inspector did not submit the image snapshot command");
        } else {
            require(editor.asset_preview().status.find("unavailable") != std::string::npos,
                    "image provider failure was not exposed honestly");
        }
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.avi", world);
        for (int i = 0; i < 120 && (editor.media_preview().kind != "Video" || editor.media_preview().previewLoading); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.media_preview().kind == "Video" && editor.media_preview().path == "assets/preview.avi",
                "video asset did not enter the media preview state");
        require(!editor.media_preview().videoPreview &&
                editor.media_preview().previewStatus.find("unavailable") != std::string::npos,
                "video provider failure was not exposed honestly");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.obj", world);
        for (int i = 0; i < 120 && editor.asset_preview().loading; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().kind == "Mesh" && editor.asset_preview().modelPreview &&
                editor.asset_preview().modelPreview->valid(),
                "model asset did not produce a bounded wireframe preview");
        require(editor.asset_preview().modelPreview->minX == -4.0f &&
                editor.asset_preview().modelPreview->maxY == 4.0f,
                "model preview did not consume the AssetSystem structured source artifact");
        require(editor.asset_preview().modelPreviewScene && editor.asset_preview().modelPreviewScene->valid(),
                "model asset did not publish an isolated preview scene state");
        require(region("asset-model-material-prev").width > 0.0f &&
                region("asset-model-material-next").width > 0.0f &&
                region("asset-model-texture-prev").width > 0.0f &&
                region("asset-model-texture-next").width > 0.0f,
                "model material/texture selector regions were not retained");
        click("asset-model-material-next");
        click("asset-model-texture-next");
        require(editor.asset_preview().modelMaterialIndex == -1 &&
                editor.asset_preview().modelTextureIndex == -1,
                "empty model material/texture selectors changed state");
        const auto worldObjectsBeforeModelOrbit = world.object_count();
        const auto modelProjectionBeforeOrbit = editor.asset_preview().modelPreviewScene->projectionRevision;
        const auto modelSurface = region("asset-model-preview");
        gesture.type = input::InputEventType::MouseButtonDown;
        gesture.control = "mouse:middle";
        gesture.position = {modelSurface.x + modelSurface.width * 0.5f, modelSurface.y + modelSurface.height * 0.5f};
        fake->queue.push_back(gesture); tick();
        gesture.type = input::InputEventType::MouseMove;
        gesture.position.x += 28.0f;
        gesture.position.y -= 12.0f;
        fake->queue.push_back(gesture); tick();
        gesture.type = input::InputEventType::MouseButtonUp;
        fake->queue.push_back(gesture); tick();
        require(editor.asset_preview().modelPreviewScene->projectionRevision != modelProjectionBeforeOrbit,
                "model preview orbit did not update the isolated camera");
        require(world.object_count() == worldObjectsBeforeModelOrbit,
                "model preview orbit mutated the active world");
        click("asset-model-preview-reset");
        require(editor.asset_preview().modelPreviewScene->camera.yaw == 0.55f,
                "model preview reset did not restore the camera");
        editor.execute_command(EditorCommand::OpenAsset, "assets/preview.gltf", world);
        for (int i = 0; i < 240 &&
             (editor.asset_preview().loading || editor.asset_preview().modelTextureStatus.empty() ||
              editor.asset_preview().modelTextureStatus.find("Loading") != std::string::npos); ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().modelPreview && editor.asset_preview().modelPreview->valid() &&
                editor.asset_preview().modelMaterialIndex == 0 &&
                editor.asset_preview().modelTextureIndex == 0 &&
                editor.asset_preview().modelTextureImageIndex == 0 &&
                editor.asset_preview().modelMaterialLabel.find("MaterialA") != std::string::npos &&
                editor.asset_preview().modelTextureLabel.find("TextureA") != std::string::npos,
                "GLTF material-to-texture selection was not published");
        require(editor.asset_preview().modelTextureSnapshot &&
                editor.asset_preview().modelTextureSnapshot->valid(),
                "GLTF image artifact was not decoded by the WIC preview seam");
        click("asset-model-material-next");
        for (int i = 0; i < 120 && editor.asset_preview().modelTextureStatus.find("Loading") != std::string::npos; ++i) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(editor.asset_preview().modelMaterialIndex == 1 &&
                editor.asset_preview().modelTextureIndex == 1 &&
                editor.asset_preview().modelTextureImageIndex == 1 &&
                editor.asset_preview().modelMaterialLabel.find("MaterialB") != std::string::npos &&
                editor.asset_preview().modelTextureLabel.find("TextureB") != std::string::npos,
                "GLTF material selection did not follow its base-color texture reference");
        editor.shutdown();
        audioScene.shutdown(audioSystem);
        audioSystem.shutdown();
        resourceSystem.shutdown();
        std::filesystem::remove_all(project);
        std::cout << "Editor document, real filesystem, input routing, inspector, undo and simulation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
