#pragma once

#include "shinkou/ui/Theme.h"
#include "shinkou/ui/Ui.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace shinkou::ui {

inline constexpr std::uint32_t kComponentSchemaVersion = 1;
inline constexpr std::string_view kComponentSchema = "shinkou.ui-components";

enum class ComponentType : std::uint8_t { Button, Slider, TextBox, RichTextBox, Panel };

const char* component_type_name(ComponentType type) noexcept;
bool parse_component_type(std::string_view name, ComponentType& type) noexcept;

enum class ComponentEventType : std::uint8_t {
    PointerEnter,
    PointerLeave,
    PointerDown,
    PointerUp,
    Click,
    ValueChanged,
    TextChanged,
    Submit,
    FocusGained,
    FocusLost
};

struct ComponentEvent {
    ComponentEventType type{ComponentEventType::Click};
    std::string componentId{};
    Vec2 position{};
    float value{0.0f};
    std::string text{};
    bool accepted{false};
    std::uint32_t modifiers{0};
};

struct ComponentState {
    bool hovered{false};
    bool pressed{false};
    bool focused{false};
    bool checked{false};
    bool invalid{false};

    friend constexpr bool operator==(const ComponentState& left, const ComponentState& right) noexcept {
        return left.hovered == right.hovered && left.pressed == right.pressed &&
               left.focused == right.focused && left.checked == right.checked &&
               left.invalid == right.invalid;
    }
};

class Component;
using ComponentEventCallback = std::function<EventResult(Component&, ComponentEvent&)>;

// Backend-neutral component base. It owns only user-facing state and never knows
// about ImGui, GPU resources, or a platform window.
class Component {
public:
    virtual ~Component() = default;

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) noexcept = default;
    Component& operator=(Component&&) noexcept = default;

    ComponentType type() const noexcept { return type_; }
    std::string_view id() const noexcept { return id_; }
    bool set_id(std::string id);

    const Rect& bounds() const noexcept { return bounds_; }
    void set_bounds(Rect bounds) noexcept { bounds_ = bounds; }
    bool visible() const noexcept { return visible_; }
    void set_visible(bool visible) noexcept { visible_ = visible; }
    bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept;
    const ComponentState& state() const noexcept { return state_; }
    void set_state(ComponentState state) noexcept { state_ = state; }

    void set_event_callback(ComponentEventCallback callback) { callback_ = std::move(callback); }

    template<class Handler>
    void set_event_callback(Handler&& handler) {
        using Callable = std::decay_t<Handler>;
        callback_ = [callable = Callable(std::forward<Handler>(handler))](Component& component, ComponentEvent& event) mutable {
            if constexpr (std::is_invocable_r_v<EventResult, Callable&, Component&, ComponentEvent&>) {
                return callable(component, event);
            } else if constexpr (std::is_invocable_r_v<bool, Callable&, Component&, ComponentEvent&>) {
                return callable(component, event) ? EventResult::Handled : EventResult::Continue;
            } else if constexpr (std::is_invocable_v<Callable&, Component&, ComponentEvent&>) {
                callable(component, event);
                return EventResult::Continue;
            } else if constexpr (std::is_invocable_r_v<EventResult, Callable&, ComponentEvent&>) {
                return callable(event);
            } else if constexpr (std::is_invocable_r_v<bool, Callable&, ComponentEvent&>) {
                return callable(event) ? EventResult::Handled : EventResult::Continue;
            } else if constexpr (std::is_invocable_v<Callable&, ComponentEvent&>) {
                callable(event);
                return EventResult::Continue;
            } else {
                static_assert(std::is_invocable_v<Callable&, ComponentEvent&>,
                              "component event handlers must accept (Component&, ComponentEvent&) or (ComponentEvent&)");
            }
        };
    }

    void clear_event_callback() noexcept { callback_ = {}; }
    bool dispatch(ComponentEvent& event);

    virtual bool valid(std::string* error = nullptr) const;
    virtual std::unique_ptr<Component> clone() const = 0;

protected:
    explicit Component(ComponentType type, std::string id = {});
    void copy_common_to(Component& target) const;
    bool emit(ComponentEventType type, float value = 0.0f, std::string text = {});

private:
    ComponentType type_;
    std::string id_;
    Rect bounds_{};
    bool visible_{true};
    bool enabled_{true};
    ComponentState state_{};
    ComponentEventCallback callback_{};
};

class Button final : public Component {
public:
    explicit Button(std::string id = {}, std::string label = {});

    std::string_view label() const noexcept { return label_; }
    void set_label(std::string label) { label_ = std::move(label); }
    bool toggleable() const noexcept { return toggleable_; }
    void set_toggleable(bool toggleable) noexcept { toggleable_ = toggleable; }
    bool checked() const noexcept { return checked_; }
    void set_checked(bool checked) noexcept;
    bool click();

    bool valid(std::string* error = nullptr) const override;
    std::unique_ptr<Component> clone() const override;

private:
    std::string label_;
    bool toggleable_{false};
    bool checked_{false};
};

enum class SliderOrientation : std::uint8_t { Horizontal, Vertical };

class Slider final : public Component {
public:
    explicit Slider(std::string id = {}, float value = 0.0f);

    float value() const noexcept { return value_; }
    float minimum() const noexcept { return minimum_; }
    float maximum() const noexcept { return maximum_; }
    float step() const noexcept { return step_; }
    bool set_range(float minimum, float maximum);
    bool set_step(float step) noexcept;
    bool set_value(float value, bool notify = true);
    float normalized_value() const noexcept;
    SliderOrientation orientation() const noexcept { return orientation_; }
    void set_orientation(SliderOrientation orientation) noexcept { orientation_ = orientation; }

    bool valid(std::string* error = nullptr) const override;
    std::unique_ptr<Component> clone() const override;

private:
    float value_{0.0f};
    float minimum_{0.0f};
    float maximum_{1.0f};
    float step_{0.01f};
    SliderOrientation orientation_{SliderOrientation::Horizontal};
};

class TextBox final : public Component {
public:
    explicit TextBox(std::string id = {}, std::string text = {});

    std::string_view text() const noexcept { return text_; }
    void set_text(std::string text, bool notify = false);
    std::string_view placeholder() const noexcept { return placeholder_; }
    void set_placeholder(std::string placeholder) { placeholder_ = std::move(placeholder); }
    std::size_t max_length() const noexcept { return maxLength_; }
    void set_max_length(std::size_t maxLength) noexcept;
    bool multiline() const noexcept { return multiline_; }
    void set_multiline(bool multiline) noexcept { multiline_ = multiline; }
    bool read_only() const noexcept { return readOnly_; }
    void set_read_only(bool readOnly) noexcept { readOnly_ = readOnly; }
    std::size_t selection_start() const noexcept { return selectionStart_; }
    std::size_t selection_end() const noexcept { return selectionEnd_; }
    void set_selection(std::size_t start, std::size_t end) noexcept;
    bool insert_text(std::string_view text);
    bool erase_selection();
    bool submit();

    bool valid(std::string* error = nullptr) const override;
    std::unique_ptr<Component> clone() const override;

private:
    std::string text_;
    std::string placeholder_;
    std::size_t maxLength_{0};
    bool multiline_{false};
    bool readOnly_{false};
    std::size_t selectionStart_{0};
    std::size_t selectionEnd_{0};
};

struct RichTextStyle {
    bool bold{false};
    bool italic{false};
    bool underline{false};
    ThemeColor color{1.0f, 1.0f, 1.0f, 1.0f};

    friend bool operator==(const RichTextStyle& left, const RichTextStyle& right) noexcept {
        return left.bold == right.bold && left.italic == right.italic &&
               left.underline == right.underline && left.color == right.color;
    }
};

struct RichTextRun {
    std::string text;
    RichTextStyle style{};

    friend bool operator==(const RichTextRun& left, const RichTextRun& right) noexcept {
        return left.text == right.text && left.style == right.style;
    }
};

class RichTextBox final : public Component {
public:
    explicit RichTextBox(std::string id = {});

    const std::vector<RichTextRun>& runs() const noexcept { return runs_; }
    void set_runs(std::vector<RichTextRun> runs) { runs_ = std::move(runs); }
    bool append_run(RichTextRun run);
    void clear_runs() noexcept { runs_.clear(); }
    std::string plain_text() const;
    bool set_plain_text(std::string text, RichTextStyle style = {});
    bool read_only() const noexcept { return readOnly_; }
    void set_read_only(bool readOnly) noexcept { readOnly_ = readOnly; }
    bool submit();

    bool valid(std::string* error = nullptr) const override;
    std::unique_ptr<Component> clone() const override;

private:
    std::vector<RichTextRun> runs_;
    bool readOnly_{false};
};

class Panel final : public Component {
public:
    explicit Panel(std::string id = {}, std::string title = {});

    std::string_view title() const noexcept { return title_; }
    void set_title(std::string title) { title_ = std::move(title); }
    bool collapsible() const noexcept { return collapsible_; }
    void set_collapsible(bool collapsible) noexcept { collapsible_ = collapsible; }
    bool collapsed() const noexcept { return collapsed_; }
    bool set_collapsed(bool collapsed);
    bool scrollable() const noexcept { return scrollable_; }
    void set_scrollable(bool scrollable) noexcept { scrollable_ = scrollable; }
    const Insets& padding() const noexcept { return padding_; }
    void set_padding(Insets padding) noexcept { padding_ = padding; }
    const std::vector<std::string>& child_ids() const noexcept { return childIds_; }
    bool add_child(std::string childId);
    bool remove_child(std::string_view childId);

    bool valid(std::string* error = nullptr) const override;
    std::unique_ptr<Component> clone() const override;

private:
    std::string title_;
    bool collapsible_{false};
    bool collapsed_{false};
    bool scrollable_{false};
    Insets padding_{};
    std::vector<std::string> childIds_;
};

class ComponentDocument final {
public:
    ComponentDocument() = default;
    ComponentDocument(const ComponentDocument&) = delete;
    ComponentDocument& operator=(const ComponentDocument&) = delete;
    ComponentDocument(ComponentDocument&&) noexcept = default;
    ComponentDocument& operator=(ComponentDocument&&) noexcept = default;

    bool add(std::unique_ptr<Component> component);

    template<class T, class... Args>
    T* emplace(Args&&... args) {
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T* result = component.get();
        if (!add(std::move(component))) return nullptr;
        return result;
    }

    bool remove(std::string_view id);
    void clear() noexcept { components_.clear(); }
    std::size_t size() const noexcept { return components_.size(); }
    const std::vector<std::unique_ptr<Component>>& components() const noexcept { return components_; }
    const Component* find(std::string_view id) const noexcept;
    Component* find(std::string_view id) noexcept;
    bool valid(std::string* error = nullptr) const;

private:
    std::vector<std::unique_ptr<Component>> components_;
};

std::string serialize_json(const Component& component, bool pretty = true);
bool deserialize_json(std::string_view json, std::unique_ptr<Component>& component,
                      std::string* error = nullptr);
std::string serialize_json(const ComponentDocument& document, bool pretty = true);
bool deserialize_json(std::string_view json, ComponentDocument& document,
                      std::string* error = nullptr);

} // namespace shinkou::ui
