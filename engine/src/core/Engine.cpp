#include "shinkou/Engine.h"
#include "shinkou/physics/SimplePhysicsWorld.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace shinkou {
Engine::Engine(const EngineConfig& config)
    : config_(config), assets_(config.assets), renderer_(config.renderBackend), physics_(std::make_unique<physics::SimplePhysicsWorld>()),
      audio_(audio::create_audio_backend(), config.audio), network_(config.network), input_(input::create_sdl3_input_backend()),
      scripts_(scripting::create_runtime(scripting::Language::CSharp)) {
    if (!log::initialize(config_.logging)) return;
    SHINKOU_LOG_DEBUG("Engine object created");
}

bool Engine::initialize() {
    if (initialized_) return true;
    SHINKOU_LOG_INFO("Engine initialization started; backend={}, window={}x{}",
        static_cast<int>(config_.renderBackend), config_.window.width, config_.window.height);
    if (!assets_.initialize()) {
        SHINKOU_LOG_ERROR("Asset system initialization failed");
        return false;
    }
    if (!network_.initialize()) {
        SHINKOU_LOG_ERROR("Network system initialization failed: {}", network_.stats().lastError);
        shutdown();
        return false;
    }
    if (!window_.create(config_.window)) {
        SHINKOU_LOG_ERROR("Window creation failed");
        assets_.shutdown();
        return false;
    }
    input_.attach_window(window_.native_handle());
    windowWidth_ = window_.width();
    windowHeight_ = window_.height();
    if (config_.editor) {
        editor_.set_native_window(window_.native_handle());
        editor_.set_display_size(static_cast<float>(config_.window.width), static_cast<float>(config_.window.height));
        window_.set_menu_command_handler([this](std::uint32_t command) {
            editor_.handle_native_menu_command(command, world_);
        });
    }
    renderer_.set_config({window_.native_handle(), config_.window.width, config_.window.height, true});
    if (!renderer_.initialize()) {
        SHINKOU_LOG_ERROR("Renderer initialization failed: {}", renderer_.last_error());
        shutdown();
        return false;
    }
    if (!physics_ || !audio_.initialize() || !input_.initialize() || !scripts_.initialize() ||
        (config_.editor && !editor_.initialize())) {
        SHINKOU_LOG_ERROR("Engine subsystem initialization failed");
        shutdown();
        return false;
    }
    if (config_.editor) renderer_.initialize_imgui();
    initialized_ = true;
    running_ = true;
    frame_ = 0;
    SHINKOU_LOG_INFO("Engine initialized successfully");
    return true;
}

void Engine::tick(Seconds dt) {
    if (!initialized_ || !running_) return;
    lastDeltaSeconds_ = std::clamp(dt, 0.0f, config_.maxDeltaSeconds);
    window_.process_events();
    const auto currentWidth = window_.width();
    const auto currentHeight = window_.height();
    if (currentWidth != windowWidth_ || currentHeight != windowHeight_) {
        if (currentWidth > 0 && currentHeight > 0) {
            renderer_.resize(currentWidth, currentHeight);
            if (config_.editor) editor_.set_display_size(static_cast<float>(currentWidth), static_cast<float>(currentHeight));
            windowWidth_ = currentWidth;
            windowHeight_ = currentHeight;
        }
    }
    network_.poll();
    input_.poll();
    assets_.poll();
    assets_.trim();
    if (config_.editor) editor_.process_input(input_, world_);
    const bool quitRequested = std::any_of(input_.events().begin(), input_.events().end(),
        [](const input::InputEvent& event) { return event.type == input::InputEventType::Quit; });
    if (quitRequested || !window_.is_open()) {
        running_ = false;
        return;
    }
    physics_->step(lastDeltaSeconds_);
    world_.update(lastDeltaSeconds_);
    audio_.update(lastDeltaSeconds_);
    renderer_.begin_graph();
    if (renderCallback_) renderCallback_(renderer_, world_, dt, frame_);
    if (config_.editor) editor_.draw(renderer_, world_, lastDeltaSeconds_, frame_);
    renderer_.submit();
    ++frame_;
}

bool Engine::run(std::uint64_t maxFrames) {
    if (!initialized_ && !initialize()) return false;
    SHINKOU_LOG_INFO("Engine run started; maxFrames={}", maxFrames);
    const auto targetFrameTime = config_.targetFrameRate > 0.0
        ? std::chrono::duration<double>(1.0 / config_.targetFrameRate)
        : std::chrono::duration<double>::zero();
    std::uint64_t executedFrames = 0;
    auto previous = std::chrono::steady_clock::now();
    while (running_ && (maxFrames == 0 || executedFrames < maxFrames)) {
        const auto frameStart = std::chrono::steady_clock::now();
        const auto measuredDelta = std::chrono::duration<double>(frameStart - previous).count();
        previous = frameStart;
        tick(static_cast<Seconds>(measuredDelta));
        ++executedFrames;
        if (targetFrameTime > std::chrono::duration<double>::zero()) {
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            const auto remaining = targetFrameTime - elapsed;
            if (remaining > std::chrono::duration<double>::zero()) std::this_thread::sleep_for(remaining);
        }
    }
    SHINKOU_LOG_INFO("Engine run stopped; frames={}, running={}", executedFrames, running_);
    return true;
}

void Engine::shutdown() {
    if (!initialized_ && !window_.is_open() && !assets_.initialized()) return;
    SHINKOU_LOG_INFO("Engine shutdown started");
    running_ = false;
    network_.shutdown();
    scripts_.shutdown();
    assets_.shutdown();
    audio_.shutdown();
    input_.shutdown();
    if (config_.editor) {
        renderer_.shutdown_imgui();
        editor_.shutdown();
    }
    window_.destroy();
    initialized_ = false;
    windowWidth_ = 0;
    windowHeight_ = 0;
    lastDeltaSeconds_ = 0;
    log::flush();
}
}
