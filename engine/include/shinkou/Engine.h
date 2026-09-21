#pragma once

#include "shinkou/assets/AssetSystem.h"
#include "shinkou/World.h"
#include "shinkou/audio/AudioSceneSystem.h"
#include "shinkou/audio/AudioSystem.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/input/InputSystem.h"
#include "shinkou/log/Log.h"
#include "shinkou/network/NetworkSystem.h"
#include "shinkou/platform/Window.h"
#include "shinkou/physics/PhysicsWorld.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/scripting/ScriptHost.h"
#include <functional>
#include <cstdint>
#include <string>

namespace shinkou {
struct EngineConfig {
    render::BackendApi renderBackend{render::BackendApi::Null};
    platform::WindowConfig window{};
    assets::AssetSystemConfig assets{};
    audio::AudioConfig audio{};
    network::NetworkConfig network{};
    log::LogConfig logging{};
    bool editor{false};
    // Physics is an opt-in runtime component. Rendering, audio, input and
    // editor startup must not load a physics backend or allocate a physics
    // world unless the application explicitly requests it.
    bool enablePhysics{false};
    // Set this for editor applications so resource browsing is independent
    // of the executable's launch directory.
    std::string editorProjectRoot{};
    double targetFrameRate{60.0};
    Seconds maxDeltaSeconds{0.1f};
    physics::PhysicsWorldConfig physics{};
};

class Engine {
public:
    using RenderCallback = std::function<void(render::Renderer&, World&, Seconds, FrameIndex)>;

private:
    EngineConfig config_;
    platform::Window window_;
    World world_;
    assets::AssetSystem assets_;
    render::Renderer renderer_;
    std::unique_ptr<physics::IPhysicsWorld> physics_;
    audio::AudioSystem audio_;
    audio::AudioSceneSystem audioScene_;
    network::NetworkSystem network_;
    input::InputSystem input_;
    scripting::ScriptHost scripts_;
    editor::EditorLayer editor_;
    FrameIndex frame_{0};
    bool initialized_{false};
    bool running_{false};
    Seconds lastDeltaSeconds_{0};
    std::uint32_t windowWidth_{0};
    std::uint32_t windowHeight_{0};
    RenderCallback renderCallback_;
public:
    explicit Engine(const EngineConfig& config = {});
    bool initialize();
    void tick(Seconds dt);
    bool run(std::uint64_t maxFrames = 0);
    void shutdown();
    void set_render_callback(RenderCallback callback) { renderCallback_ = std::move(callback); }
    void stop() noexcept { running_ = false; }
    bool running() const noexcept { return running_; }
    FrameIndex frame_index() const noexcept { return frame_; }
    Seconds last_delta_seconds() const noexcept { return lastDeltaSeconds_; }
    World& world() noexcept { return world_; }
    assets::AssetSystem& assets() noexcept { return assets_; }
    const assets::AssetSystem& assets() const noexcept { return assets_; }
    render::Renderer& renderer() noexcept { return renderer_; }
    editor::EditorLayer& editor() noexcept { return editor_; }
    const editor::EditorLayer& editor() const noexcept { return editor_; }
    platform::Window& window() noexcept { return window_; }
    input::InputSystem& input() noexcept { return input_; }
    audio::AudioSystem& audio() noexcept { return audio_; }
    const audio::AudioSystem& audio() const noexcept { return audio_; }
    audio::AudioSceneSystem& audio_scene() noexcept { return audioScene_; }
    const audio::AudioSceneSystem& audio_scene() const noexcept { return audioScene_; }
    network::NetworkSystem& network() noexcept { return network_; }
    const network::NetworkSystem& network() const noexcept { return network_; }
    physics::IPhysicsWorld* physics() noexcept { return physics_.get(); }
    const physics::IPhysicsWorld* physics() const noexcept { return physics_.get(); }
    bool physics_enabled() const noexcept { return physics_ != nullptr; }
};
}
