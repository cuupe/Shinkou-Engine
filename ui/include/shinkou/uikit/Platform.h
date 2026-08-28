#pragma once

#include "Ui.h"
#include <string>

namespace shinkou::uikit {

struct PlatformCapabilities {
    bool dpiAware = false;
    bool nativeTextInput = false;
    bool fileDialogs = false;
    bool media = false;
};

class IPlatformAdapter {
public:
    virtual ~IPlatformAdapter() = default;
    virtual bool initialize(void* nativeWindow) = 0;
    virtual void shutdown() = 0;
    virtual void poll_events(UiContext& context) = 0;
    virtual float dpi_scale(void* nativeWindow) const = 0;
    virtual std::string font_directory() const = 0;
    virtual std::string open_file_dialog(const std::string& title, const std::string& filter) = 0;
    virtual std::string save_file_dialog(const std::string& title, const std::string& filter) = 0;
    virtual bool read_clipboard(std::string& value) const = 0;
    virtual bool write_clipboard(const std::string& value) const = 0;
    virtual void* native_handle() const = 0;
    virtual PlatformCapabilities capabilities() const = 0;
};

} // namespace shinkou::uikit
