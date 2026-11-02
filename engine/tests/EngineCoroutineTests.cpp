#include "shinkou/Engine.h"
#include "shinkou/coroutines/Coroutine.h"
#include <iostream>
#include <vector>

using namespace shinkou::coroutines;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Task<void> pipeline(std::vector<int>& order, int& destroyed) {
    struct Guard { int& value; ~Guard() { ++value; } } guard{destroyed};
    order.push_back(1);
    co_await yield(Phase::LateUpdate);
    order.push_back(3);
    co_await yield(Phase::EndOfFrame);
    order.push_back(5);
    co_await sleep_for(1000);
}
}
int main() {
    try {
        shinkou::EngineConfig config;
        config.renderBackend = shinkou::render::BackendApi::Null;
        config.window.visible = false;
        config.window.width = config.window.height = 32;
        config.audio.startDevice = false;
        config.logging.file = config.logging.console = false;
        std::cerr << "construct engine\n";
        shinkou::Engine engine(config);
        std::vector<int> order;
        int destroyed = 0;
        auto handle = engine.coroutines().spawn(pipeline(order, destroyed));
        engine.world().add_system([&](auto&, auto) { order.push_back(2); });
        engine.set_render_callback([&](auto&, auto&, auto, auto) { order.push_back(4); });
        std::cerr << "initialize engine\n";
        require(engine.initialize(), "hidden null-backend engine failed to initialize");
        std::cerr << "tick engine\n";
        engine.tick(0.016f);
        require(order == std::vector<int>({1, 2, 3, 4, 5}), "engine coroutine phases are not aligned with world/render stages");
        require(engine.coroutines().frame() == 1, "engine did not advance scheduler frame");
        std::cerr << "shutdown engine\n";
        engine.shutdown();
        require(handle.status() == Status::Cancelled && destroyed == 1, "engine shutdown did not synchronously destroy suspended frames");
        require(engine.coroutines().stats().active == 0, "engine shutdown retained tasks");
        auto pending = engine.coroutines().spawn(pipeline(order, destroyed));
        engine.shutdown();
        require(pending.status() == Status::Cancelled && destroyed == 1, "shutdown before initialization executed a pending body");
        std::cout << "engine coroutine lifecycle passed\n";
        std::cerr << "destroy engine\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
