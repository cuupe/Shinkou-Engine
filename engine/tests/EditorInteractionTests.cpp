#include "shinkou/editor/EditorLayer.h"
#include "shinkou/editor/EditorDocument.h"
#include "shinkou/editor/EditorPrimitiveMesh.h"
#include "shinkou/audio/AudioSceneSystem.h"
#include "shinkou/World.h"
#include "shinkou/input/InputSystem.h"
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
    audio::AudioPlayParams lastPlay{}, lastSpatial{};
    float lastPitch{1}, lastPan{0}, lastVolume{1};
    std::size_t effectCount{0}, activeTracks{0};
    audio::AudioTrackId nextTrack{1};
    bool initialize(const audio::AudioConfig&) override { return true; }
    void shutdown() override { voices_.clear(); cursors_.clear(); }
    void update(Seconds) override {}
    audio::AudioAssetInfo inspect_asset(const audio::AudioAssetDesc& asset) const override {
        return {asset.streaming, true, 30.0, true};
    }
    audio::AudioVoiceId play(const audio::AudioAssetDesc&, const audio::AudioPlayParams& params) override {
        lastPlay=params;
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
    void set_volume(audio::AudioVoiceId, float value) override {lastVolume=value;}
    void set_pitch(audio::AudioVoiceId, float value) override {lastPitch=value;}
    void set_pan(audio::AudioVoiceId, float value) override {lastPan=value;}
    void set_spatial(audio::AudioVoiceId, const audio::AudioPlayParams& value) override {lastSpatial=value;}
    void set_listener(const audio::AudioListener&) override {}
    void set_bus_volume(audio::AudioBus, float) override {}
    void set_bus_muted(audio::AudioBus, bool) override {}
    audio::AudioTrackId create_track(const audio::AudioTrackDesc&) override {++activeTracks;return audio::make_audio_handle(nextTrack++,1);}
    void destroy_track(audio::AudioTrackId) override {--activeTracks;}
    void set_track_effects(audio::AudioTrackId,const std::vector<audio::AudioEffectDesc>& effects) override {effectCount=effects.size();}
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
          "animations":[{"name":"Idle","samplers":[{}],"channels":[{"sampler":0}]}],
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
        auto audioBackend=std::make_unique<TestAudioBackend>();auto* audioProbe=audioBackend.get();
        audio::AudioSystem audioSystem(std::move(audioBackend), audioConfig);
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
        for(int i=0;i<500 && (editor.ui_asset_file_count()<100 || editor.asset_system_manifest_count()==0);++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        require(editor.ui_asset_file_count()>100 && editor.asset_system_manifest_count()>0,"controller resource scan did not settle");
        require(editor.ui_command_count()==0 && editor.ui_interaction_regions().empty(),"controller-only initialization created an alternate UI");
        require(editor.asset_browser().directory=="assets","browser directory diverged from the controller");
        editor.set_asset_filter("NEEDLE");tick();
        require(editor.asset_browser().rows().size()==1,"shared browser did not apply case-insensitive descendant search");
        editor.set_asset_filter("");
        require(!editor.select_object(999999,world),"invalid object selection accepted");
        require(editor.select_object(selected,world),"valid object selection failed");tick();
        auto* transform=world.find_object(selected)->get_component<components::TransformComponent>();
        const auto field=std::to_string(transform->id())+":position";
        const auto before=transform->local.position;
        require(editor.begin_inspector_edit(field),"typed edit transaction did not start");
        require(editor.edit_inspector_value(field,math::Vec3{1,2,3}) && editor.edit_inspector_value(field,math::Vec3{4,5,6}),"typed vector edit failed");
        editor.finish_inspector_edit();
        editor.execute_command(EditorCommand::Undo,{},world);
        require(math::Length(world.find_object(selected)->get_component<components::TransformComponent>()->local.position-before)<0.0001f,"vector gesture was not one undo operation");
        editor.execute_command(EditorCommand::Redo,{},world);
        require(math::Length(world.find_object(selected)->get_component<components::TransformComponent>()->local.position-math::Vec3{4,5,6})<0.0001f,"typed vector redo failed");

        auto& live=world.create_object("Live file reference");const auto liveId=live.id();
        live.add_component<AssetReferenceComponent>("assets/Folder/Nested/needle.txt",needleAssetId);
        editor.set_selected_asset("assets/Folder/Nested/needle.txt");
        editor.handle_asset_action(EditorAssetAction::Rename,"assets/Folder","Renamed");tick();
        require(std::filesystem::exists(project/"assets/Renamed/Nested/needle.txt"),"folder rename failed");
        require(editor.asset_browser().selection=="assets/Renamed/Nested/needle.txt","selected asset was not remapped by rename");
        require(world.find_object(liveId)->get_component<AssetReferenceComponent>()->path()=="assets/Renamed/Nested/needle.txt","live reference did not follow rename");
        std::string prefab,error2;
        require(editor.file_system().read_text("assets/Scenes/Unloaded.prefab",prefab,&error2) && prefab.find("assets/Renamed/Nested/needle.txt")!=std::string::npos,"unloaded prefab reference was not remapped");
        editor.execute_command(EditorCommand::Undo,{},world);tick();
        require(std::filesystem::exists(project/"assets/Folder/Nested/needle.txt"),"file rename undo failed");
        std::filesystem::create_directory(project/"assets/Renamed");
        editor.execute_command(EditorCommand::Redo,{},world);
        require(std::filesystem::exists(project/"assets/Folder/Nested/needle.txt"),"failed redo changed its source");
        std::filesystem::remove(project/"assets/Renamed");
        editor.execute_command(EditorCommand::Redo,{},world);tick();
        require(std::filesystem::exists(project/"assets/Renamed/Nested/needle.txt"),("file rename redo failed: "+editor.last_status()).c_str());
        editor.handle_asset_action(EditorAssetAction::NewFolder,"assets","NewFolder");
        require(std::filesystem::is_directory(project/"assets/NewFolder"),"shared new-folder action failed");
        editor.handle_asset_action(EditorAssetAction::Delete,"assets/Folder-extra.txt",{});tick();
        require(!std::filesystem::exists(project/"assets/Folder-extra.txt") && editor.file_history_undo_count()>0,"recoverable delete failed");
        require(editor.file_system().read_text("assets/Scenes/Unloaded.prefab",prefab,&error2) && prefab.find("assets/Folder-extra.txt")!=std::string::npos,"delete damaged unloaded reference path");
        FileHistoryDocument history;std::string journal;
        require(editor.file_system().read_text(".shinkou/file-history.json",journal,&error2) && FileHistoryDocument::from_json(journal,history,error2) && !history.entries.empty(),"file journal was not persisted");
        editor.execute_command(EditorCommand::Undo,{},world);tick();
        require(std::filesystem::exists(project/"assets/Folder-extra.txt"),"recoverable delete undo failed");
        editor.execute_command(EditorCommand::Redo,{},world);tick();
        require(!std::filesystem::exists(project/"assets/Folder-extra.txt"),"recoverable delete redo failed");

        editor.set_selected_asset("assets/preview.wav");
        for(int i=0;i<500 && !editor.media_preview().audioPreview;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        require(editor.media_preview().audioPreview!=nullptr,"audio preview did not survive presenter removal");
        auto settings=editor.asset_settings();settings.pitch=1.5f;settings.pan=-0.25f;settings.spatialized=true;
        settings.positionX=2;settings.positionY=3;settings.positionZ=4;settings.lowPass=true;
        require(editor.set_asset_settings(settings,true),"shared audio settings could not be saved");
        editor.execute_command(EditorCommand::MediaPlay,{},world);tick();
        require(audioProbe->lastPlay.pitch==1.5f && audioProbe->lastPlay.pan==-0.25f,"audio transport bypassed engine settings");
        editor.execute_command(EditorCommand::MediaPause,{},world);tick();
        editor.execute_command(EditorCommand::MediaStop,{},world);tick();
        require(editor.create_audio_source_from_asset(world),"audio asset could not create an engine component");
        editor.set_selected_asset("assets/preview.obj");
        for(int i=0;i<500 && !editor.asset_preview().modelPreview;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        require(editor.asset_preview().modelPreview!=nullptr,"async model preview no longer works without the UI");
        require(editor.asset_preview().assetSystemStatus.find("AssetSystem")!=std::string::npos,"asset preview lost resource-system status");
        // Real factory commands must remain usable in a populated scene and
        // preserve their renderable data through undo, redo and JSON restore.
        const auto objectCount=world.object_count();
        for (const auto* shape:{"Cube","Sphere","Plane"}) {
            editor.execute_command(EditorCommand::Create3DObject,shape,world);tick();
            auto* created=world.find_object(editor.layout().selectedObject);
            require(created && created->name()==shape,"primitive factory did not select its new object");
            require(created->get_component<components::PrimitiveMeshComponent>() && !created->has_ecs_entity(),"primitive has no serializable mesh component");
            require(editor_primitive_mesh(shape)->valid(),"primitive geometry is invalid");
        }
        require(world.object_count()==objectCount+3,"primitive factory did not create three objects");
        editor.execute_command(EditorCommand::Undo,{},world);tick();
        require(world.object_count()==objectCount+2,"primitive creation undo failed");
        editor.execute_command(EditorCommand::Redo,{},world);tick();
        require(world.object_count()==objectCount+3,"primitive creation redo failed");
        EditorDocument scene,loaded;std::string sceneJson,sceneError;
        require(EditorDocument::capture(world,editor.layout().selectedObject,scene,sceneError),sceneError.c_str());
        require(scene.to_json(sceneJson,sceneError) && EditorDocument::from_json(sceneJson,loaded,sceneError),"primitive scene JSON round trip failed");
        World restored;restored.register_component_type<AssetReferenceComponent>("AssetReference");ObjectId restoredSelection{};
        require(loaded.restore(restored,restoredSelection,sceneError),sceneError.c_str());
        require(restored.find_object(restoredSelection)->get_component<components::PrimitiveMeshComponent>()->shape()=="Plane","restored primitive lost its shape");
        std::vector<EditorModelSceneInstance> instances;restored.refresh_transforms();append_editor_primitives(restored,instances);
        require(instances.size()==3,"restored primitives disappeared from the render extraction");
        editor.execute_command(EditorCommand::CreateEmpty,{},world);tick();
        require(world.object_count()==objectCount+4,"empty creation failed in a renderable scene");
        editor.shutdown(&renderer);audioSystem.shutdown();resourceSystem.shutdown();
        std::filesystem::remove_all(project);
        std::cout<<"Editor controller, typed edits, file transactions and media integration passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
