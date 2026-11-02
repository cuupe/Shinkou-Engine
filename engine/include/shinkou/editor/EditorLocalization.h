#pragma once

#include <string>
#include <string_view>

namespace shinkou::editor {

// Small, dependency-free localization table for the retained editor shell.
// Content names, paths and compiler diagnostics stay data-driven; only editor
// chrome is translated here so asset previews remain faithful to their source.
class EditorLocalization final {
  public:
    EditorLocalization() = default;
    explicit EditorLocalization(std::string_view language) {
        set_language(language);
    }

    void set_language(std::string_view language) noexcept;
    std::string_view language() const noexcept {
        return language_;
    }
    bool is_chinese() const noexcept {
        return language_ == "zh-CN";
    }

    std::string_view text(std::string_view english) const noexcept;
    static bool supported(std::string_view language) noexcept;

  private:
    std::string language_{"zh-CN"};
};

} // namespace shinkou::editor
