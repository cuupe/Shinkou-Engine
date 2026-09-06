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
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::function<void(std::uint32_t)> menuCommandHandler_;
    std::function<bool()> closeHandler_;
public:
    bool create(const WindowConfig& config);
    void set_close_handler(std::function<bool()> handler) { closeHandler_ = std::move(handler); }
    bool request_close() { return !closeHandler_ || closeHandler_(); }
    void process_events();
    void destroy();
    bool is_open() const noexcept { return open_; }
    void* native_handle() const noexcept { return nativeHandle_; }
    void set_menu_command_handler(std::function<void(std::uint32_t)> handler) { menuCommandHandler_ = std::move(handler); }
    void dispatch_menu_command(std::uint32_t command) { if (menuCommandHandler_) menuCommandHandler_(command); }
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    float dpi_scale() const noexcept;
    void set_client_size(std::uint32_t width, std::uint32_t height) noexcept { width_ = width; height_ = height; }
};
}
