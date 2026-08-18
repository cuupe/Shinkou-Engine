#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace shinkou::platform {
struct WindowConfig {
    std::string title{"ShinkouEngine"};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool visible{true};
};

class Window final {
    void* nativeHandle_{nullptr};
    bool open_{false};
    std::function<void(std::uint32_t)> menuCommandHandler_;
public:
    bool create(const WindowConfig& config);
    void process_events();
    void destroy();
    bool is_open() const noexcept { return open_; }
    void* native_handle() const noexcept { return nativeHandle_; }
    void set_menu_command_handler(std::function<void(std::uint32_t)> handler) { menuCommandHandler_ = std::move(handler); }
    void dispatch_menu_command(std::uint32_t command) { if (menuCommandHandler_) menuCommandHandler_(command); }
};
}
