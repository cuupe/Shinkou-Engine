#include "shinkou/ui/Components.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace shinkou::ui {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool finite(float value) noexcept { return std::isfinite(static_cast<double>(value)); }

bool valid_color(ThemeColor color) noexcept {
    return finite(color.r) && finite(color.g) && finite(color.b) && finite(color.a) &&
           color.r >= 0.0f && color.r <= 1.0f && color.g >= 0.0f && color.g <= 1.0f &&
           color.b >= 0.0f && color.b <= 1.0f && color.a >= 0.0f && color.a <= 1.0f;
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20u) output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                          << static_cast<unsigned int>(character) << std::dec;
            else output << static_cast<char>(character);
            break;
        }
    }
    output << '"';
    return output.str();
}

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Object, Array };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string string;
    std::map<std::string, JsonValue, std::less<>> object;
    std::vector<JsonValue> array;

    static JsonValue object_value() { JsonValue value; value.kind = Kind::Object; return value; }
    static JsonValue array_value() { JsonValue value; value.kind = Kind::Array; return value; }
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        if (input_.size() > 8u * 1024u * 1024u) return fail(error, "component JSON is too large");
        skip_space();
        if (!parse_value(value, error, 0)) return false;
        skip_space();
        return position_ == input_.size() || fail(error, "trailing JSON data");
    }

private:
    std::string_view input_;
    std::size_t position_{0};

    bool fail(std::string& error, std::string message) const {
        error = std::move(message);
        return false;
    }

    void skip_space() noexcept {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool consume(char character) noexcept {
        if (position_ >= input_.size() || input_[position_] != character) return false;
        ++position_;
        return true;
    }

    bool parse_value(JsonValue& value, std::string& error, unsigned depth) {
        if (depth > 48) return fail(error, "component JSON nesting is too deep");
        skip_space();
        if (position_ >= input_.size()) return fail(error, "unexpected end of JSON");
        switch (input_[position_]) {
        case '{': return parse_object(value, error, depth + 1);
        case '[': return parse_array(value, error, depth + 1);
        case '"': value.kind = JsonValue::Kind::String; return parse_string(value.string, error);
        case 't': return parse_literal(value, "true", JsonValue::Kind::Boolean, true, error);
        case 'f': return parse_literal(value, "false", JsonValue::Kind::Boolean, false, error);
        case 'n': return parse_literal(value, "null", JsonValue::Kind::Null, false, error);
        default: return parse_number(value, error);
        }
    }

    bool parse_literal(JsonValue& value, std::string_view literal, JsonValue::Kind kind,
                       bool boolean, std::string& error) {
        if (input_.substr(position_, literal.size()) != literal) return fail(error, "invalid JSON literal");
        position_ += literal.size();
        value.kind = kind;
        value.boolean = boolean;
        return true;
    }

    bool parse_number(JsonValue& value, std::string& error) {
        const std::size_t start = position_;
        while (position_ < input_.size() && std::string_view("0123456789+-.eE").find(input_[position_]) != std::string_view::npos) ++position_;
        if (start == position_) return fail(error, "invalid JSON value");
        const std::string number(input_.substr(start, position_ - start));
        char* end = nullptr;
        const double parsed = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !std::isfinite(parsed)) return fail(error, "invalid JSON number");
        value.kind = JsonValue::Kind::Number;
        value.number = parsed;
        return true;
    }

    static bool hex_digit(char value, unsigned& result) noexcept {
        if (value >= '0' && value <= '9') { result = static_cast<unsigned>(value - '0'); return true; }
        if (value >= 'a' && value <= 'f') { result = static_cast<unsigned>(value - 'a' + 10); return true; }
        if (value >= 'A' && value <= 'F') { result = static_cast<unsigned>(value - 'A' + 10); return true; }
        return false;
    }

    static void append_utf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20u) return fail(error, "control character in JSON string");
            if (character != '\\') { output.push_back(static_cast<char>(character)); continue; }
            if (position_ >= input_.size()) return fail(error, "unfinished JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    if (position_ >= input_.size()) return fail(error, "unfinished unicode escape");
                    unsigned digit = 0;
                    if (!hex_digit(input_[position_++], digit)) return fail(error, "invalid unicode escape");
                    codepoint = (codepoint << 4) | digit;
                }
                append_utf8(output, codepoint);
                break;
            }
            default: return fail(error, "invalid JSON escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    bool parse_object(JsonValue& value, std::string& error, unsigned depth) {
        consume('{');
        value = JsonValue::object_value();
        skip_space();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            skip_space();
            std::string key;
            if (!parse_string(key, error)) return false;
            skip_space();
            if (!consume(':')) return fail(error, "expected ':' in JSON object");
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            if (!value.object.emplace(std::move(key), std::move(child)).second) return fail(error, "duplicate JSON key");
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON object");
        }
        return fail(error, "unterminated JSON object");
    }

    bool parse_array(JsonValue& value, std::string& error, unsigned depth) {
        consume('[');
        value = JsonValue::array_value();
        skip_space();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            JsonValue child;
            if (!parse_value(child, error, depth)) return false;
            value.array.push_back(std::move(child));
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) return fail(error, "expected ',' in JSON array");
            skip_space();
        }
        return fail(error, "unterminated JSON array");
    }
};

const JsonValue* member(const JsonValue& value, std::string_view name) {
    if (value.kind != JsonValue::Kind::Object) return nullptr;
    const auto found = value.object.find(name);
    return found == value.object.end() ? nullptr : &found->second;
}

bool string_value(const JsonValue* value, std::string& output) {
    if (!value || value->kind != JsonValue::Kind::String) return false;
    output = value->string;
    return true;
}

bool bool_value(const JsonValue* value, bool& output) {
    if (!value || value->kind != JsonValue::Kind::Boolean) return false;
    output = value->boolean;
    return true;
}

bool number_value(const JsonValue* value, float& output) {
    if (!value || value->kind != JsonValue::Kind::Number || !std::isfinite(value->number) ||
        value->number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value->number > static_cast<double>(std::numeric_limits<float>::max())) return false;
    output = static_cast<float>(value->number);
    return finite(output);
}

bool size_value(const JsonValue* value, std::size_t& output) {
    if (!value || value->kind != JsonValue::Kind::Number || value->number < 0.0 ||
        std::floor(value->number) != value->number || value->number > static_cast<double>(std::numeric_limits<std::size_t>::max())) return false;
    output = static_cast<std::size_t>(value->number);
    return true;
}

bool rect_value(const JsonValue* value, Rect& output) {
    if (!value || value->kind != JsonValue::Kind::Array || value->array.size() != 4) return false;
    float values[4]{};
    for (std::size_t index = 0; index < 4; ++index) if (!number_value(&value->array[index], values[index])) return false;
    output = Rect{values[0], values[1], values[2], values[3]};
    return true;
}

bool insets_value(const JsonValue* value, Insets& output) {
    if (!value || value->kind != JsonValue::Kind::Array || value->array.size() != 4) return false;
    float values[4]{};
    for (std::size_t index = 0; index < 4; ++index) if (!number_value(&value->array[index], values[index])) return false;
    output = Insets{values[0], values[1], values[2], values[3]};
    return true;
}

bool color_value(const JsonValue* value, ThemeColor& output) {
    if (!value || value->kind != JsonValue::Kind::Array || value->array.size() != 4) return false;
    float values[4]{};
    for (std::size_t index = 0; index < 4; ++index) if (!number_value(&value->array[index], values[index])) return false;
    output = ThemeColor{values[0], values[1], values[2], values[3]};
    return valid_color(output);
}

void indent(std::ostream& output, bool pretty, unsigned depth) {
    if (!pretty) return;
    output << '\n' << std::string(depth * 2u, ' ');
}

void write_color(std::ostream& output, ThemeColor color) {
    output << '[' << color.r << ',' << color.g << ',' << color.b << ',' << color.a << ']';
}

void write_bool_field(std::ostream& output, bool pretty, unsigned depth, bool& first,
                      std::string_view name, bool value) {
    if (!first) output << ',';
    first = false;
    indent(output, pretty, depth);
    output << json_escape(name) << (pretty ? ": " : ":") << (value ? "true" : "false");
}

template<class Writer>
void write_field(std::ostream& output, bool pretty, unsigned depth, bool& first,
                 std::string_view name, Writer&& writer) {
    if (!first) output << ',';
    first = false;
    indent(output, pretty, depth);
    output << json_escape(name) << (pretty ? ": " : ":");
    writer();
}

void write_state(std::ostream& output, const ComponentState& state, bool pretty, unsigned depth) {
    output << '{';
    bool first = true;
    write_bool_field(output, pretty, depth + 1, first, "hovered", state.hovered);
    write_bool_field(output, pretty, depth + 1, first, "pressed", state.pressed);
    write_bool_field(output, pretty, depth + 1, first, "focused", state.focused);
    write_bool_field(output, pretty, depth + 1, first, "checked", state.checked);
    write_bool_field(output, pretty, depth + 1, first, "invalid", state.invalid);
    indent(output, pretty, depth);
    output << '}';
}

void write_component_body(std::ostream& output, const Component& component, bool pretty, unsigned depth) {
    output << '{';
    bool first = true;
    write_field(output, pretty, depth + 1, first, "type", [&] { output << json_escape(component_type_name(component.type())); });
    write_field(output, pretty, depth + 1, first, "id", [&] { output << json_escape(component.id()); });
    write_field(output, pretty, depth + 1, first, "bounds", [&] {
        const Rect bounds = component.bounds();
        output << '[' << bounds.x << ',' << bounds.y << ',' << bounds.width << ',' << bounds.height << ']';
    });
    write_bool_field(output, pretty, depth + 1, first, "visible", component.visible());
    write_bool_field(output, pretty, depth + 1, first, "enabled", component.enabled());
    write_field(output, pretty, depth + 1, first, "state", [&] { write_state(output, component.state(), pretty, depth + 1); });

    if (const auto* button = dynamic_cast<const Button*>(&component)) {
        write_field(output, pretty, depth + 1, first, "label", [&] { output << json_escape(button->label()); });
        write_bool_field(output, pretty, depth + 1, first, "toggleable", button->toggleable());
        write_bool_field(output, pretty, depth + 1, first, "checked", button->checked());
    } else if (const auto* slider = dynamic_cast<const Slider*>(&component)) {
        write_field(output, pretty, depth + 1, first, "value", [&] { output << slider->value(); });
        write_field(output, pretty, depth + 1, first, "minimum", [&] { output << slider->minimum(); });
        write_field(output, pretty, depth + 1, first, "maximum", [&] { output << slider->maximum(); });
        write_field(output, pretty, depth + 1, first, "step", [&] { output << slider->step(); });
        write_field(output, pretty, depth + 1, first, "orientation", [&] {
            output << json_escape(slider->orientation() == SliderOrientation::Vertical ? "vertical" : "horizontal");
        });
    } else if (const auto* textBox = dynamic_cast<const TextBox*>(&component)) {
        write_field(output, pretty, depth + 1, first, "text", [&] { output << json_escape(textBox->text()); });
        write_field(output, pretty, depth + 1, first, "placeholder", [&] { output << json_escape(textBox->placeholder()); });
        write_field(output, pretty, depth + 1, first, "maxLength", [&] { output << textBox->max_length(); });
        write_bool_field(output, pretty, depth + 1, first, "multiline", textBox->multiline());
        write_bool_field(output, pretty, depth + 1, first, "readOnly", textBox->read_only());
        write_field(output, pretty, depth + 1, first, "selection", [&] {
            output << '[' << textBox->selection_start() << ',' << textBox->selection_end() << ']';
        });
    } else if (const auto* richTextBox = dynamic_cast<const RichTextBox*>(&component)) {
        write_bool_field(output, pretty, depth + 1, first, "readOnly", richTextBox->read_only());
        write_field(output, pretty, depth + 1, first, "runs", [&] {
            output << '[';
            bool runFirst = true;
            for (const auto& run : richTextBox->runs()) {
                if (!runFirst) output << ',';
                runFirst = false;
                indent(output, pretty, depth + 2);
                output << '{';
                bool fieldFirst = true;
                write_field(output, pretty, depth + 3, fieldFirst, "text", [&] { output << json_escape(run.text); });
                write_bool_field(output, pretty, depth + 3, fieldFirst, "bold", run.style.bold);
                write_bool_field(output, pretty, depth + 3, fieldFirst, "italic", run.style.italic);
                write_bool_field(output, pretty, depth + 3, fieldFirst, "underline", run.style.underline);
                write_field(output, pretty, depth + 3, fieldFirst, "color", [&] { write_color(output, run.style.color); });
                indent(output, pretty, depth + 2);
                output << '}';
            }
            indent(output, pretty, depth + 1);
            output << ']';
        });
    } else if (const auto* panel = dynamic_cast<const Panel*>(&component)) {
        write_field(output, pretty, depth + 1, first, "title", [&] { output << json_escape(panel->title()); });
        write_bool_field(output, pretty, depth + 1, first, "collapsible", panel->collapsible());
        write_bool_field(output, pretty, depth + 1, first, "collapsed", panel->collapsed());
        write_bool_field(output, pretty, depth + 1, first, "scrollable", panel->scrollable());
        write_field(output, pretty, depth + 1, first, "padding", [&] {
            const Insets padding = panel->padding();
            output << '[' << padding.left << ',' << padding.top << ',' << padding.right << ',' << padding.bottom << ']';
        });
        write_field(output, pretty, depth + 1, first, "children", [&] {
            output << '[';
            bool childFirst = true;
            for (const auto& child : panel->child_ids()) {
                if (!childFirst) output << ',';
                childFirst = false;
                if (pretty) output << ' ';
                output << json_escape(child);
            }
            output << ']';
        });
    }
    indent(output, pretty, depth);
    output << '}';
}

bool read_common(const JsonValue& value, Component& component, std::string& error) {
    std::string id;
    if (!string_value(member(value, "id"), id) || !component.set_id(std::move(id))) {
        error = "component id is missing or invalid";
        return false;
    }
    if (const auto* bounds = member(value, "bounds")) {
        Rect rect;
        if (!rect_value(bounds, rect)) { error = "component bounds are invalid"; return false; }
        component.set_bounds(rect);
    }
    bool boolean = false;
    if (const auto* visible = member(value, "visible")) {
        if (!bool_value(visible, boolean)) { error = "component visibility is invalid"; return false; }
        component.set_visible(boolean);
    }
    if (const auto* enabled = member(value, "enabled")) {
        if (!bool_value(enabled, boolean)) { error = "component enabled state is invalid"; return false; }
        component.set_enabled(boolean);
    }
    ComponentState state = component.state();
    if (const auto* stateValue = member(value, "state")) {
        if (stateValue->kind != JsonValue::Kind::Object) { error = "component state is invalid"; return false; }
        if (const auto* field = member(*stateValue, "hovered")) if (!bool_value(field, state.hovered)) { error = "hovered state is invalid"; return false; }
        if (const auto* field = member(*stateValue, "pressed")) if (!bool_value(field, state.pressed)) { error = "pressed state is invalid"; return false; }
        if (const auto* field = member(*stateValue, "focused")) if (!bool_value(field, state.focused)) { error = "focused state is invalid"; return false; }
        if (const auto* field = member(*stateValue, "checked")) if (!bool_value(field, state.checked)) { error = "checked state is invalid"; return false; }
        if (const auto* field = member(*stateValue, "invalid")) if (!bool_value(field, state.invalid)) { error = "invalid state is invalid"; return false; }
    }
    component.set_state(state);
    return true;
}

bool parse_component_value(const JsonValue& value, std::unique_ptr<Component>& output, std::string& error) {
    if (value.kind != JsonValue::Kind::Object) { error = "component must be a JSON object"; return false; }
    std::string typeName;
    ComponentType type{};
    if (!string_value(member(value, "type"), typeName) || !parse_component_type(typeName, type)) {
        error = "component type is missing or unsupported";
        return false;
    }
    std::unique_ptr<Component> component;
    switch (type) {
    case ComponentType::Button: component = std::make_unique<Button>(); break;
    case ComponentType::Slider: component = std::make_unique<Slider>(); break;
    case ComponentType::TextBox: component = std::make_unique<TextBox>(); break;
    case ComponentType::RichTextBox: component = std::make_unique<RichTextBox>(); break;
    case ComponentType::Panel: component = std::make_unique<Panel>(); break;
    }
    if (!read_common(value, *component, error)) return false;
    bool boolean = false;
    std::string text;
    if (auto* button = dynamic_cast<Button*>(component.get())) {
        if (const auto* field = member(value, "label")) {
            if (!string_value(field, text)) { error = "button label is invalid"; return false; }
            button->set_label(text);
        }
        if (const auto* field = member(value, "toggleable")) {
            if (!bool_value(field, boolean)) { error = "button toggleable state is invalid"; return false; }
            button->set_toggleable(boolean);
        }
        if (const auto* field = member(value, "checked")) {
            if (!bool_value(field, boolean)) { error = "button checked state is invalid"; return false; }
            button->set_checked(boolean);
        }
    } else if (auto* slider = dynamic_cast<Slider*>(component.get())) {
        float number = 0.0f;
        float minimum = slider->minimum();
        float maximum = slider->maximum();
        float step = slider->step();
        if (const auto* field = member(value, "minimum")) if (!number_value(field, minimum)) { error = "slider minimum is invalid"; return false; }
        if (const auto* field = member(value, "maximum")) if (!number_value(field, maximum)) { error = "slider maximum is invalid"; return false; }
        if (!slider->set_range(minimum, maximum)) { error = "slider range is invalid"; return false; }
        if (const auto* field = member(value, "step")) if (!number_value(field, step) || !slider->set_step(step)) { error = "slider step is invalid"; return false; }
        if (const auto* field = member(value, "value")) if (!number_value(field, number) || !slider->set_value(number, false)) { error = "slider value is invalid"; return false; }
        if (const auto* field = member(value, "orientation")) {
            if (!string_value(field, text)) { error = "slider orientation is invalid"; return false; }
            if (text == "vertical") slider->set_orientation(SliderOrientation::Vertical);
            else if (text == "horizontal") slider->set_orientation(SliderOrientation::Horizontal);
            else { error = "slider orientation is unsupported"; return false; }
        }
    } else if (auto* textBox = dynamic_cast<TextBox*>(component.get())) {
        if (const auto* field = member(value, "text")) {
            if (!string_value(field, text)) { error = "text box text is invalid"; return false; }
            textBox->set_text(text);
        }
        if (const auto* field = member(value, "placeholder")) {
            if (!string_value(field, text)) { error = "text box placeholder is invalid"; return false; }
            textBox->set_placeholder(text);
        }
        std::size_t length = 0;
        if (const auto* field = member(value, "maxLength")) {
            if (!size_value(field, length)) { error = "text box max length is invalid"; return false; }
            textBox->set_max_length(length);
        }
        if (const auto* field = member(value, "multiline")) {
            if (!bool_value(field, boolean)) { error = "text box multiline state is invalid"; return false; }
            textBox->set_multiline(boolean);
        }
        if (const auto* field = member(value, "readOnly")) {
            if (!bool_value(field, boolean)) { error = "text box read-only state is invalid"; return false; }
            textBox->set_read_only(boolean);
        }
        if (const auto* field = member(value, "selection")) {
            if (field->kind != JsonValue::Kind::Array || field->array.size() != 2 || !size_value(&field->array[0], length)) { error = "text box selection is invalid"; return false; }
            std::size_t end = 0;
            if (!size_value(&field->array[1], end)) { error = "text box selection is invalid"; return false; }
            textBox->set_selection(length, end);
        }
    } else if (auto* richTextBox = dynamic_cast<RichTextBox*>(component.get())) {
        if (const auto* field = member(value, "readOnly")) {
            if (!bool_value(field, boolean)) { error = "rich text box read-only state is invalid"; return false; }
            richTextBox->set_read_only(boolean);
        }
        if (const auto* field = member(value, "runs")) {
            if (field->kind != JsonValue::Kind::Array) { error = "rich text runs are invalid"; return false; }
            std::vector<RichTextRun> runs;
            for (const auto& item : field->array) {
                if (item.kind != JsonValue::Kind::Object) { error = "rich text run is invalid"; return false; }
                RichTextRun run;
                if (!string_value(member(item, "text"), run.text)) { error = "rich text run text is invalid"; return false; }
                if (const auto* style = member(item, "bold")) if (!bool_value(style, run.style.bold)) { error = "rich text bold state is invalid"; return false; }
                if (const auto* style = member(item, "italic")) if (!bool_value(style, run.style.italic)) { error = "rich text italic state is invalid"; return false; }
                if (const auto* style = member(item, "underline")) if (!bool_value(style, run.style.underline)) { error = "rich text underline state is invalid"; return false; }
                if (const auto* style = member(item, "color")) if (!color_value(style, run.style.color)) { error = "rich text color is invalid"; return false; }
                runs.push_back(std::move(run));
            }
            richTextBox->set_runs(std::move(runs));
        }
    } else if (auto* panel = dynamic_cast<Panel*>(component.get())) {
        if (const auto* field = member(value, "title")) {
            if (!string_value(field, text)) { error = "panel title is invalid"; return false; }
            panel->set_title(text);
        }
        if (const auto* field = member(value, "collapsible")) {
            if (!bool_value(field, boolean)) { error = "panel collapsible state is invalid"; return false; }
            panel->set_collapsible(boolean);
        }
        if (const auto* field = member(value, "collapsed")) {
            if (!bool_value(field, boolean) || !panel->set_collapsed(boolean)) { error = "panel collapsed state is invalid"; return false; }
        }
        if (const auto* field = member(value, "scrollable")) {
            if (!bool_value(field, boolean)) { error = "panel scrollable state is invalid"; return false; }
            panel->set_scrollable(boolean);
        }
        Insets padding;
        if (const auto* field = member(value, "padding")) {
            if (!insets_value(field, padding)) { error = "panel padding is invalid"; return false; }
            panel->set_padding(padding);
        }
        if (const auto* field = member(value, "children")) {
            if (field->kind != JsonValue::Kind::Array) { error = "panel children are invalid"; return false; }
            for (const auto& child : field->array) {
                if (!string_value(&child, text) || !panel->add_child(text)) { error = "panel child id is invalid"; return false; }
            }
        }
    }
    if (!component->valid(&error)) return false;
    output = std::move(component);
    return true;
}

bool read_document_root(const JsonValue& root, const JsonValue*& components, std::string& error) {
    if (root.kind != JsonValue::Kind::Object) { error = "component document must be an object"; return false; }
    std::string schema;
    float version = 0.0f;
    if (!string_value(member(root, "schema"), schema) || schema != kComponentSchema ||
        !number_value(member(root, "version"), version) || version != static_cast<float>(kComponentSchemaVersion)) {
        error = "unsupported or missing component schema/version";
        return false;
    }
    components = member(root, "components");
    if (!components || components->kind != JsonValue::Kind::Array) { error = "component list is missing or invalid"; return false; }
    return true;
}

} // namespace

const char* component_type_name(ComponentType type) noexcept {
    switch (type) {
    case ComponentType::Button: return "button";
    case ComponentType::Slider: return "slider";
    case ComponentType::TextBox: return "textBox";
    case ComponentType::RichTextBox: return "richTextBox";
    case ComponentType::Panel: return "panel";
    }
    return "unknown";
}

bool parse_component_type(std::string_view name, ComponentType& type) noexcept {
    if (name == "button") type = ComponentType::Button;
    else if (name == "slider") type = ComponentType::Slider;
    else if (name == "textBox" || name == "textbox") type = ComponentType::TextBox;
    else if (name == "richTextBox" || name == "richtextbox") type = ComponentType::RichTextBox;
    else if (name == "panel") type = ComponentType::Panel;
    else return false;
    return true;
}

Component::Component(ComponentType type, std::string id) : type_(type), id_(std::move(id)) {}

bool Component::set_id(std::string id) {
    if (id.empty() || id.size() > 256) return false;
    id_ = std::move(id);
    return true;
}

void Component::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
    if (!enabled_) {
        state_.hovered = false;
        state_.pressed = false;
        state_.focused = false;
    }
}

bool Component::dispatch(ComponentEvent& event) {
    if (!visible_ || !enabled_) return false;
    event.componentId = id_;
    switch (event.type) {
    case ComponentEventType::PointerEnter: state_.hovered = true; break;
    case ComponentEventType::PointerLeave: state_.hovered = false; state_.pressed = false; break;
    case ComponentEventType::PointerDown: state_.pressed = true; break;
    case ComponentEventType::PointerUp: state_.pressed = false; break;
    case ComponentEventType::FocusGained: state_.focused = true; break;
    case ComponentEventType::FocusLost: state_.focused = false; state_.pressed = false; break;
    default: break;
    }
    if (!callback_) return false;
    const EventResult result = callback_(*this, event);
    event.accepted = result != EventResult::Continue;
    return result != EventResult::Continue;
}

bool Component::valid(std::string* error) const {
    if (id_.empty() || id_.size() > 256) { set_error(error, "component id is invalid"); return false; }
    if (!finite(bounds_.x) || !finite(bounds_.y) || !finite(bounds_.width) || !finite(bounds_.height) ||
        bounds_.width < 0.0f || bounds_.height < 0.0f) { set_error(error, "component bounds are invalid"); return false; }
    if (error) error->clear();
    return true;
}

void Component::copy_common_to(Component& target) const {
    target.id_ = id_;
    target.bounds_ = bounds_;
    target.visible_ = visible_;
    target.enabled_ = enabled_;
    target.state_ = state_;
    target.callback_ = {};
}

bool Component::emit(ComponentEventType type, float value, std::string text) {
    ComponentEvent event;
    event.type = type;
    event.value = value;
    event.text = std::move(text);
    return dispatch(event);
}

Button::Button(std::string id, std::string label) : Component(ComponentType::Button, std::move(id)), label_(std::move(label)) {}

void Button::set_checked(bool checked) noexcept {
    checked_ = checked;
    ComponentState state = this->state();
    state.checked = checked;
    set_state(state);
}

bool Button::click() {
    if (!visible() || !enabled()) return false;
    if (toggleable_) set_checked(!checked_);
    return emit(ComponentEventType::Click, checked_, label_);
}

bool Button::valid(std::string* error) const {
    if (!Component::valid(error)) return false;
    if (label_.size() > 16u * 1024u) { set_error(error, "button label is too long"); return false; }
    if (error) error->clear();
    return true;
}

std::unique_ptr<Component> Button::clone() const {
    auto result = std::make_unique<Button>(std::string(id()), label_);
    result->toggleable_ = toggleable_;
    result->checked_ = checked_;
    copy_common_to(*result);
    return result;
}

Slider::Slider(std::string id, float value) : Component(ComponentType::Slider, std::move(id)), value_(value) {
    set_value(value, false);
}

bool Slider::set_range(float minimum, float maximum) {
    if (!finite(minimum) || !finite(maximum) || maximum <= minimum) return false;
    minimum_ = minimum;
    maximum_ = maximum;
    set_value(value_, false);
    return true;
}

bool Slider::set_step(float step) noexcept {
    if (!finite(step) || step <= 0.0f) return false;
    step_ = step;
    set_value(value_, false);
    return true;
}

bool Slider::set_value(float value, bool notify) {
    if (!finite(value)) return false;
    const float clamped = std::clamp(value, minimum_, maximum_);
    const float stepped = std::clamp(minimum_ + std::round((clamped - minimum_) / step_) * step_, minimum_, maximum_);
    if (value_ == stepped) return true;
    value_ = stepped;
    if (notify) emit(ComponentEventType::ValueChanged, value_);
    return true;
}

float Slider::normalized_value() const noexcept {
    return maximum_ == minimum_ ? 0.0f : (value_ - minimum_) / (maximum_ - minimum_);
}

bool Slider::valid(std::string* error) const {
    if (!Component::valid(error)) return false;
    if (!finite(value_) || !finite(minimum_) || !finite(maximum_) || !finite(step_) ||
        maximum_ <= minimum_ || step_ <= 0.0f || value_ < minimum_ || value_ > maximum_) {
        set_error(error, "slider range or value is invalid"); return false;
    }
    if (error) error->clear();
    return true;
}

std::unique_ptr<Component> Slider::clone() const {
    auto result = std::make_unique<Slider>(std::string(id()), value_);
    result->minimum_ = minimum_;
    result->maximum_ = maximum_;
    result->step_ = step_;
    result->orientation_ = orientation_;
    copy_common_to(*result);
    return result;
}

TextBox::TextBox(std::string id, std::string text) : Component(ComponentType::TextBox, std::move(id)), text_(std::move(text)) {}

void TextBox::set_text(std::string text, bool notify) {
    if (maxLength_ != 0 && text.size() > maxLength_) text.resize(maxLength_);
    const bool changed = text_ != text;
    text_ = std::move(text);
    set_selection(selectionStart_, selectionEnd_);
    if (notify && changed) emit(ComponentEventType::TextChanged, 0.0f, text_);
}

void TextBox::set_max_length(std::size_t maxLength) noexcept {
    maxLength_ = maxLength;
    if (maxLength_ != 0 && text_.size() > maxLength_) text_.resize(maxLength_);
    set_selection(selectionStart_, selectionEnd_);
}

void TextBox::set_selection(std::size_t start, std::size_t end) noexcept {
    const std::size_t length = text_.size();
    selectionStart_ = std::min(start, length);
    selectionEnd_ = std::min(end, length);
    if (selectionStart_ > selectionEnd_) std::swap(selectionStart_, selectionEnd_);
}

bool TextBox::insert_text(std::string_view text) {
    if (readOnly_ || !visible() || !enabled()) return false;
    const std::size_t start = selectionStart_;
    const std::size_t end = selectionEnd_;
    std::string replacement(text);
    if (maxLength_ != 0) {
        const std::size_t available = maxLength_ - std::min(maxLength_, text_.size() - (end - start));
        if (replacement.size() > available) replacement.resize(available);
    }
    text_.replace(start, end - start, replacement);
    set_selection(start + replacement.size(), start + replacement.size());
    emit(ComponentEventType::TextChanged, 0.0f, text_);
    return true;
}

bool TextBox::erase_selection() {
    if (readOnly_ || selectionStart_ == selectionEnd_ || !visible() || !enabled()) return false;
    text_.erase(selectionStart_, selectionEnd_ - selectionStart_);
    set_selection(selectionStart_, selectionStart_);
    emit(ComponentEventType::TextChanged, 0.0f, text_);
    return true;
}

bool TextBox::submit() {
    return emit(ComponentEventType::Submit, 0.0f, text_);
}

bool TextBox::valid(std::string* error) const {
    if (!Component::valid(error)) return false;
    if ((maxLength_ != 0 && text_.size() > maxLength_) || selectionStart_ > text_.size() || selectionEnd_ > text_.size()) {
        set_error(error, "text box text or selection is invalid"); return false;
    }
    if (error) error->clear();
    return true;
}

std::unique_ptr<Component> TextBox::clone() const {
    auto result = std::make_unique<TextBox>(std::string(id()), text_);
    result->placeholder_ = placeholder_;
    result->maxLength_ = maxLength_;
    result->multiline_ = multiline_;
    result->readOnly_ = readOnly_;
    result->selectionStart_ = selectionStart_;
    result->selectionEnd_ = selectionEnd_;
    copy_common_to(*result);
    return result;
}

RichTextBox::RichTextBox(std::string id) : Component(ComponentType::RichTextBox, std::move(id)) {}

bool RichTextBox::append_run(RichTextRun run) {
    if (readOnly_ || !visible() || !enabled() || run.text.size() > 1024u * 1024u || !valid_color(run.style.color)) return false;
    runs_.push_back(std::move(run));
    emit(ComponentEventType::TextChanged, 0.0f, plain_text());
    return true;
}

std::string RichTextBox::plain_text() const {
    std::string result;
    for (const auto& run : runs_) result += run.text;
    return result;
}

bool RichTextBox::set_plain_text(std::string text, RichTextStyle style) {
    if (readOnly_ || text.size() > 1024u * 1024u || !valid_color(style.color)) return false;
    runs_.clear();
    if (!text.empty()) runs_.push_back(RichTextRun{std::move(text), style});
    emit(ComponentEventType::TextChanged, 0.0f, plain_text());
    return true;
}

bool RichTextBox::submit() {
    return emit(ComponentEventType::Submit, 0.0f, plain_text());
}

bool RichTextBox::valid(std::string* error) const {
    if (!Component::valid(error)) return false;
    for (const auto& run : runs_) {
        if (run.text.size() > 1024u * 1024u || !valid_color(run.style.color)) { set_error(error, "rich text run is invalid"); return false; }
    }
    if (error) error->clear();
    return true;
}

std::unique_ptr<Component> RichTextBox::clone() const {
    auto result = std::make_unique<RichTextBox>(std::string(id()));
    result->runs_ = runs_;
    result->readOnly_ = readOnly_;
    copy_common_to(*result);
    return result;
}

Panel::Panel(std::string id, std::string title) : Component(ComponentType::Panel, std::move(id)), title_(std::move(title)) {}

bool Panel::set_collapsed(bool collapsed) {
    if (collapsed && !collapsible_) return false;
    collapsed_ = collapsed;
    return true;
}

bool Panel::add_child(std::string childId) {
    if (childId.empty() || std::find(childIds_.begin(), childIds_.end(), childId) != childIds_.end()) return false;
    childIds_.push_back(std::move(childId));
    return true;
}

bool Panel::remove_child(std::string_view childId) {
    const auto found = std::find(childIds_.begin(), childIds_.end(), childId);
    if (found == childIds_.end()) return false;
    childIds_.erase(found);
    return true;
}

bool Panel::valid(std::string* error) const {
    if (!Component::valid(error)) return false;
    const float paddingValues[] = {padding_.left, padding_.top, padding_.right, padding_.bottom};
    for (const float value : paddingValues) if (!finite(value) || value < 0.0f) { set_error(error, "panel padding is invalid"); return false; }
    for (const auto& child : childIds_) if (child.empty()) { set_error(error, "panel child id is invalid"); return false; }
    if (title_.size() > 16u * 1024u) { set_error(error, "panel title is too long"); return false; }
    if (error) error->clear();
    return true;
}

std::unique_ptr<Component> Panel::clone() const {
    auto result = std::make_unique<Panel>(std::string(id()), title_);
    result->collapsible_ = collapsible_;
    result->collapsed_ = collapsed_;
    result->scrollable_ = scrollable_;
    result->padding_ = padding_;
    result->childIds_ = childIds_;
    copy_common_to(*result);
    return result;
}

bool ComponentDocument::add(std::unique_ptr<Component> component) {
    if (!component || !component->valid()) return false;
    if (find(component->id()) != nullptr) return false;
    components_.push_back(std::move(component));
    return true;
}

bool ComponentDocument::remove(std::string_view id) {
    const auto found = std::find_if(components_.begin(), components_.end(), [&](const auto& item) { return item->id() == id; });
    if (found == components_.end()) return false;
    components_.erase(found);
    return true;
}

const Component* ComponentDocument::find(std::string_view id) const noexcept {
    const auto found = std::find_if(components_.begin(), components_.end(), [&](const auto& item) { return item && item->id() == id; });
    return found == components_.end() ? nullptr : found->get();
}

Component* ComponentDocument::find(std::string_view id) noexcept {
    const auto found = std::find_if(components_.begin(), components_.end(), [&](const auto& item) { return item && item->id() == id; });
    return found == components_.end() ? nullptr : found->get();
}

bool ComponentDocument::valid(std::string* error) const {
    for (std::size_t index = 0; index < components_.size(); ++index) {
        if (!components_[index]) { set_error(error, "component document contains null component"); return false; }
        if (!components_[index]->valid(error)) return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (components_[previous]->id() == components_[index]->id()) { set_error(error, "component ids must be unique"); return false; }
        }
    }
    if (error) error->clear();
    return true;
}

std::string serialize_json(const Component& component, bool pretty) {
    if (!component.valid()) return {};
    std::ostringstream output;
    output << std::setprecision(9) << '{';
    bool first = true;
    write_field(output, pretty, 1, first, "schema", [&] { output << json_escape(kComponentSchema); });
    write_field(output, pretty, 1, first, "version", [&] { output << kComponentSchemaVersion; });
    write_field(output, pretty, 1, first, "component", [&] { write_component_body(output, component, pretty, 1); });
    indent(output, pretty, 0);
    output << '}';
    return output.str();
}

bool deserialize_json(std::string_view json, std::unique_ptr<Component>& component, std::string* error) {
    JsonValue root;
    std::string parseError;
    if (!JsonParser(json).parse(root, parseError)) { set_error(error, std::move(parseError)); return false; }
    const JsonValue* value = &root;
    if (root.kind == JsonValue::Kind::Object && member(root, "component")) {
        const JsonValue* ignored = nullptr;
        if (!read_document_root(root, ignored, parseError)) {
            // Single-component documents have a component object rather than a list.
            std::string schema;
            float version = 0.0f;
            if (!string_value(member(root, "schema"), schema) || schema != kComponentSchema ||
                !number_value(member(root, "version"), version) || version != static_cast<float>(kComponentSchemaVersion)) {
                set_error(error, parseError); return false;
            }
        }
        value = member(root, "component");
        if (!value) { set_error(error, "single component value is missing"); return false; }
    }
    std::unique_ptr<Component> parsed;
    if (!parse_component_value(*value, parsed, parseError)) { set_error(error, std::move(parseError)); return false; }
    component = std::move(parsed);
    if (error) error->clear();
    return true;
}

std::string serialize_json(const ComponentDocument& document, bool pretty) {
    if (!document.valid()) return {};
    std::ostringstream output;
    output << std::setprecision(9) << '{';
    bool first = true;
    write_field(output, pretty, 1, first, "schema", [&] { output << json_escape(kComponentSchema); });
    write_field(output, pretty, 1, first, "version", [&] { output << kComponentSchemaVersion; });
    write_field(output, pretty, 1, first, "components", [&] {
        output << '[';
        bool componentFirst = true;
        for (const auto& component : document.components()) {
            if (!componentFirst) output << ',';
            componentFirst = false;
            indent(output, pretty, 2);
            write_component_body(output, *component, pretty, 2);
        }
        indent(output, pretty, 1);
        output << ']';
    });
    indent(output, pretty, 0);
    output << '}';
    return output.str();
}

bool deserialize_json(std::string_view json, ComponentDocument& document, std::string* error) {
    JsonValue root;
    std::string parseError;
    if (!JsonParser(json).parse(root, parseError)) { set_error(error, std::move(parseError)); return false; }
    const JsonValue* values = nullptr;
    if (!read_document_root(root, values, parseError)) { set_error(error, std::move(parseError)); return false; }
    ComponentDocument parsed;
    for (const auto& value : values->array) {
        std::unique_ptr<Component> component;
        if (!parse_component_value(value, component, parseError) || !parsed.add(std::move(component))) {
            if (parseError.empty()) parseError = "component document contains an invalid or duplicate component";
            set_error(error, std::move(parseError));
            return false;
        }
    }
    if (!parsed.valid(&parseError)) { set_error(error, std::move(parseError)); return false; }
    document = std::move(parsed);
    if (error) error->clear();
    return true;
}

} // namespace shinkou::ui
