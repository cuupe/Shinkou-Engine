#pragma once

#include "Platform.h"

namespace shinkou::uikit {

class Win32PlatformAdapter final : public IPlatformAdapter {
public:
    bool initialize(void* nativeWindow) override;
    void shutdown() override;
    void poll_events(UiContext& context) override;
    float dpi_scale(void* nativeWindow) const override;
    std::string font_directory() const override;
    std::string open_file_dialog(const std::string& title, const std::string& filter) override;
    std::string save_file_dialog(const std::string& title, const std::string& filter) override;
    bool read_clipboard(std::string& value) const override;
    bool write_clipboard(const std::string& value) const override;
    void* native_handle() const override { return m_nativeWindow; }
    PlatformCapabilities capabilities() const override;

private:
    void* m_nativeWindow = nullptr;
};

} // namespace shinkou::uikit
