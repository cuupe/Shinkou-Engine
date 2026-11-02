#pragma once
#include "shinkou/editor/EditorUiModel.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {
enum class EditorInputContext : std::uint8_t { Global, Scene, Hierarchy, FlyCamera };
enum class EditorKey : std::uint16_t {
    None,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Escape,
    Enter,
    Tab,
    Space,
    Delete,
    Backspace,
    Comma,
    Period,
    Home,
    End,
    Left,
    Right,
    Up,
    Down,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Shift,
    Control,
    Alt,
    Count
};
enum EditorModifier : std::uint8_t {
    NoModifier = 0,
    ControlModifier = 1,
    ShiftModifier = 2,
    AltModifier = 4
};
struct EditorKeyBinding {
    std::string action;
    std::uint16_t key{0};
    std::uint8_t modifiers{0};
};
struct EditorActionDefinition {
    std::string id, label, category;
    EditorCommand command{EditorCommand::None};
    EditorInputContext context{EditorInputContext::Global};
    bool held{false};
    EditorKey defaultKey{EditorKey::None};
    std::uint8_t defaultModifiers{0};
    std::string target;
};
class EditorActionRegistry {
    std::vector<EditorActionDefinition> actions_;

  public:
    EditorActionRegistry();
    bool register_action(EditorActionDefinition definition);
    const EditorActionDefinition *find(std::string_view id) const noexcept;
    const EditorActionDefinition *find_command(EditorCommand,
                                               std::string_view target = {}) const noexcept;
    const auto &actions() const noexcept {
        return actions_;
    }
    std::vector<EditorKeyBinding> default_bindings() const;
};
struct EditorInputFrame {
    static constexpr auto keyCount = static_cast<std::size_t>(EditorKey::Count);
    std::array<bool, keyCount> down{}, pressed{};
    std::uint8_t modifiers{};
    bool textInput{false}, itemActive{false}, popupOpen{false}, focused{true};
};
struct EditorActionBatch {
    std::array<const EditorActionDefinition *, 64> actions{};
    std::size_t count{};
    bool contains(std::string_view id) const noexcept;
};
class EditorInputRouter {
  public:
    EditorActionBatch route(const EditorActionRegistry &, const std::vector<EditorKeyBinding> &,
                            const EditorInputFrame &, EditorInputContext) const noexcept;
    static bool contexts_overlap(EditorInputContext, EditorInputContext) noexcept;
    static std::string conflict(const EditorActionRegistry &, const std::vector<EditorKeyBinding> &,
                                const EditorKeyBinding &candidate);
};
std::string editor_key_name(EditorKey);
std::string editor_binding_name(const EditorKeyBinding &);
const char *editor_context_name(EditorInputContext) noexcept;
} // namespace shinkou::editor
