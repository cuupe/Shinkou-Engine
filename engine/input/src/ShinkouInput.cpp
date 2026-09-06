#include "shinkou_input/ShinkouInputApi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ShinkouInputHandle {
    SDL_Window* window{nullptr};
    void* native_window{nullptr};
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads;
    std::vector<ShinkouInputEvent> events;
    std::vector<ShinkouInputGamepadState> gamepad_states;
    ShinkouInputMouseState mouse{};
    std::array<uint8_t, SDL_SCANCODE_COUNT> keys{};
    bool initialized{false};
};

namespace {
void copy_string(char* destination, std::size_t capacity, const char* source) {
    if (capacity == 0) return;
    if (source == nullptr) source = "";
    std::strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

void push_event(ShinkouInputHandle& input, uint32_t type, const char* control = nullptr) {
    ShinkouInputEvent event{};
    event.type = type;
    copy_string(event.control, sizeof(event.control), control);
    input.events.push_back(event);
}

void open_gamepad(ShinkouInputHandle& input, SDL_JoystickID id) {
    if (input.gamepads.find(id) != input.gamepads.end()) return;
    if (SDL_Gamepad* gamepad = SDL_OpenGamepad(id)) input.gamepads.emplace(id, gamepad);
}

void close_gamepad(ShinkouInputHandle& input, SDL_JoystickID id) {
    const auto iterator = input.gamepads.find(id);
    if (iterator == input.gamepads.end()) return;
    SDL_CloseGamepad(iterator->second);
    input.gamepads.erase(iterator);
}

void refresh_gamepads(ShinkouInputHandle& input) {
    input.gamepad_states.clear();
    std::vector<SDL_JoystickID> ids;
    ids.reserve(input.gamepads.size());
    for (const auto& [id, gamepad] : input.gamepads) {
        (void)gamepad;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const SDL_JoystickID id : ids) {
        const auto iterator = input.gamepads.find(id);
        if (iterator == input.gamepads.end()) continue;
        ShinkouInputGamepadState state{};
        state.instance_id = static_cast<int64_t>(id);
        state.connected = SDL_GamepadConnected(iterator->second) ? 1 : 0;
        state.player_index = static_cast<uint32_t>(std::max(0, SDL_GetGamepadPlayerIndex(iterator->second)));
        copy_string(state.name, sizeof(state.name), SDL_GetGamepadName(iterator->second));
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && button < 32; ++button) {
            state.buttons[button] = SDL_GetGamepadButton(iterator->second, static_cast<SDL_GamepadButton>(button)) ? 1 : 0;
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT && axis < 6; ++axis) {
            const auto raw = SDL_GetGamepadAxis(iterator->second, static_cast<SDL_GamepadAxis>(axis));
            state.axes[axis] = axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                ? static_cast<float>(raw) / 32767.0f
                : static_cast<float>(raw) / 32768.0f;
        }
        input.gamepad_states.push_back(state);
    }
}

void process_event(ShinkouInputHandle& input, const SDL_Event& source) {
    switch (source.type) {
    case SDL_EVENT_QUIT:
        push_event(input, SHINKOU_INPUT_EVENT_QUIT);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (source.key.scancode < SDL_SCANCODE_COUNT) input.keys[source.key.scancode] = source.key.down ? 1 : 0;
        ShinkouInputEvent event{};
        event.type = source.key.down ? SHINKOU_INPUT_EVENT_KEY_DOWN : SHINKOU_INPUT_EVENT_KEY_UP;
        event.device = static_cast<uint32_t>(source.key.which);
        event.value = source.key.down ? 1.0f : 0.0f;
        event.repeat = source.key.repeat ? 1 : 0;
        std::string control = "key:";
        control += SDL_GetScancodeName(source.key.scancode);
        copy_string(event.control, sizeof(event.control), control.c_str());
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        ShinkouInputEvent event{};
        event.type = SHINKOU_INPUT_EVENT_TEXT_INPUT;
        copy_string(event.text, sizeof(event.text), source.text.text);
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        input.mouse.x = source.motion.x;
        input.mouse.y = source.motion.y;
        input.mouse.delta_x += source.motion.xrel;
        input.mouse.delta_y += source.motion.yrel;
        ShinkouInputEvent event{};
        event.type = SHINKOU_INPUT_EVENT_MOUSE_MOVE;
        event.device = static_cast<uint32_t>(source.motion.which);
        event.x = input.mouse.x;
        event.y = input.mouse.y;
        event.delta_x = source.motion.xrel;
        event.delta_y = source.motion.yrel;
        copy_string(event.control, sizeof(event.control), "mouse:motion");
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto button = static_cast<std::size_t>(source.button.button);
        if (button < sizeof(input.mouse.buttons)) input.mouse.buttons[button] = source.button.down ? 1 : 0;
        ShinkouInputEvent event{};
        event.type = source.button.down ? SHINKOU_INPUT_EVENT_MOUSE_BUTTON_DOWN : SHINKOU_INPUT_EVENT_MOUSE_BUTTON_UP;
        event.device = static_cast<uint32_t>(source.button.which);
        event.value = source.button.down ? 1.0f : 0.0f;
        event.x = source.button.x;
        event.y = source.button.y;
        std::string control = "mouse:";
        control += button == 1 ? "left" : button == 2 ? "middle" : button == 3 ? "right" : std::to_string(button);
        copy_string(event.control, sizeof(event.control), control.c_str());
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        input.mouse.wheel_x += source.wheel.x;
        input.mouse.wheel_y += source.wheel.y;
        ShinkouInputEvent event{};
        event.type = SHINKOU_INPUT_EVENT_MOUSE_WHEEL;
        event.x = source.wheel.mouse_x;
        event.y = source.wheel.mouse_y;
        event.device = static_cast<uint32_t>(source.wheel.which);
        event.delta_x = source.wheel.x;
        event.delta_y = source.wheel.y;
        copy_string(event.control, sizeof(event.control), "mouse:wheel");
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        open_gamepad(input, source.gdevice.which);
        push_event(input, SHINKOU_INPUT_EVENT_GAMEPAD_ADDED, "gamepad:added");
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close_gamepad(input, source.gdevice.which);
        push_event(input, SHINKOU_INPUT_EVENT_GAMEPAD_REMOVED, "gamepad:removed");
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ShinkouInputEvent event{};
        event.type = source.gbutton.down ? SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_DOWN : SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_UP;
        event.device = static_cast<uint32_t>(source.gbutton.which);
        event.value = source.gbutton.down ? 1.0f : 0.0f;
        std::string control = "gamepad:" + std::to_string(source.gbutton.which) + ":";
        control += SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(source.gbutton.button));
        copy_string(event.control, sizeof(event.control), control.c_str());
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        ShinkouInputEvent event{};
        event.type = SHINKOU_INPUT_EVENT_GAMEPAD_AXIS_MOTION;
        event.device = static_cast<uint32_t>(source.gaxis.which);
        event.value = source.gaxis.axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER
            ? static_cast<float>(source.gaxis.value) / 32767.0f
            : static_cast<float>(source.gaxis.value) / 32768.0f;
        std::string control = "gamepad:" + std::to_string(source.gaxis.which) + ":";
        control += SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(source.gaxis.axis));
        copy_string(event.control, sizeof(event.control), control.c_str());
        input.events.push_back(event);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input.keys.fill(0);
        std::memset(input.mouse.buttons, 0, sizeof(input.mouse.buttons));
        push_event(input, SHINKOU_INPUT_EVENT_FOCUS_LOST);
        break;
    default:
        break;
    }
}

int mouse_button(std::string_view name) {
    if (name == "left" || name == "1") return 1;
    if (name == "middle" || name == "2") return 2;
    if (name == "right" || name == "3") return 3;
    if (name == "x1" || name == "4") return 4;
    if (name == "x2" || name == "5") return 5;
    return 0;
}

int gamepad_button(std::string_view name) {
    static constexpr const char* names[] = {
        "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "dpadup", "dpaddown", "dpadleft", "dpadright", "misc1",
        "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"
    };
    for (int index = 0; index < static_cast<int>(std::size(names)); ++index) if (name == names[index]) return index;
    if (name == "a") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (name == "b") return SDL_GAMEPAD_BUTTON_EAST;
    if (name == "x") return SDL_GAMEPAD_BUTTON_WEST;
    if (name == "y") return SDL_GAMEPAD_BUTTON_NORTH;
    return -1;
}

int gamepad_axis(std::string_view name) {
    if (name == "leftx") return SDL_GAMEPAD_AXIS_LEFTX;
    if (name == "lefty") return SDL_GAMEPAD_AXIS_LEFTY;
    if (name == "rightx") return SDL_GAMEPAD_AXIS_RIGHTX;
    if (name == "righty") return SDL_GAMEPAD_AXIS_RIGHTY;
    if (name == "lefttrigger" || name == "triggerleft") return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    if (name == "righttrigger" || name == "triggerright") return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    return -1;
}

SDL_Gamepad* gamepad_by_index(const ShinkouInputHandle& input, uint32_t index) {
    if (index >= input.gamepad_states.size()) return nullptr;
    return SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(input.gamepad_states[index].instance_id));
}
}

extern "C" {
SHINKOU_INPUT_API ShinkouInputHandle* SHINKOU_INPUT_CALL shinkou_input_create(void* native_window) {
    auto* input = new ShinkouInputHandle{};
    input->native_window = native_window;
    return input;
}

SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_destroy(ShinkouInputHandle* input) {
    if (input == nullptr) return;
    shinkou_input_shutdown(input);
    delete input;
}

SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_initialize(ShinkouInputHandle* input) {
    if (input == nullptr) return 0;
    if (input->initialized) return 1;
    if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_VIDEO)) return 0;
    if (input->native_window != nullptr) {
        SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties != 0) {
            SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, input->native_window);
            SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
            input->window = SDL_CreateWindowWithProperties(properties);
            SDL_DestroyProperties(properties);
            if (input->window) SDL_StartTextInput(input->window);
        }
    }
    if (input->native_window && !input->window) {
        std::fprintf(stderr,"ShinkouInput could not attach the editor window: %s\n",SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD|SDL_INIT_VIDEO|SDL_INIT_EVENTS);return 0;
    }
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int index = 0; index < count; ++index) open_gamepad(*input, ids[index]);
        SDL_free(ids);
    }
    refresh_gamepads(*input);
    input->initialized = true;
    return 1;
}

SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_shutdown(ShinkouInputHandle* input) {
    if (input == nullptr || !input->initialized) return;
    for (auto& [id, gamepad] : input->gamepads) {
        (void)id;
        SDL_CloseGamepad(gamepad);
    }
    input->gamepads.clear();
    input->gamepad_states.clear();
    if (input->window != nullptr) { SDL_StopTextInput(input->window); SDL_DestroyWindow(input->window); }
    input->window = nullptr;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    input->initialized = false;
}

SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_attach_window(ShinkouInputHandle* input, void* native_window) {
    if (input != nullptr) input->native_window = native_window;
}

SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_poll(ShinkouInputHandle* input) {
    if (input == nullptr || !input->initialized) return;
    input->events.clear();
    input->mouse.delta_x = input->mouse.delta_y = 0.0f;
    input->mouse.wheel_x = input->mouse.wheel_y = 0.0f;
    SDL_Event event{};
    while (SDL_PollEvent(&event)) process_event(*input, event);
    refresh_gamepads(*input);
}

SHINKOU_INPUT_API float SHINKOU_INPUT_CALL shinkou_input_control_value(const ShinkouInputHandle* input, const char* raw_control) {
    if (input == nullptr || raw_control == nullptr) return 0.0f;
    const std::string control = lower(raw_control);
    if (control.rfind("key:", 0) == 0) {
        const SDL_Scancode code = SDL_GetScancodeFromName(control.substr(4).c_str());
        return code < SDL_SCANCODE_COUNT && input->keys[code] ? 1.0f : 0.0f;
    }
    if (control.rfind("mouse:", 0) == 0) {
        const auto name = std::string_view(control).substr(6);
        if (name == "x") return input->mouse.delta_x;
        if (name == "y") return input->mouse.delta_y;
        if (name == "wheelx") return input->mouse.wheel_x;
        if (name == "wheely" || name == "wheel") return input->mouse.wheel_y;
        const int button = mouse_button(name);
        return button > 0 && input->mouse.buttons[button] ? 1.0f : 0.0f;
    }
    std::size_t offset = control.rfind("gamepad:", 0) == 0 ? 8 : control.rfind("pad:", 0) == 0 ? 4 : std::string::npos;
    if (offset == std::string::npos) return 0.0f;
    if (offset >= control.size() || !std::isdigit(static_cast<unsigned char>(control[offset]))) return 0.0f;
    uint32_t index = 0;
    while (offset < control.size() && std::isdigit(static_cast<unsigned char>(control[offset]))) {
        index = index * 10u + static_cast<uint32_t>(control[offset] - '0');
        ++offset;
    }
    if (offset >= control.size() || control[offset] != ':') return 0.0f;
    ++offset;
    SDL_Gamepad* gamepad = gamepad_by_index(*input, index);
    if (gamepad == nullptr) return 0.0f;
    const auto name = std::string_view(control).substr(offset);
    const int button = gamepad_button(name);
    if (button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT) return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(button)) ? 1.0f : 0.0f;
    const int axis = gamepad_axis(name);
    if (axis >= 0) {
        const auto raw = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(axis));
        return axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? static_cast<float>(raw) / 32767.0f : static_cast<float>(raw) / 32768.0f;
    }
    return 0.0f;
}

SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_mouse(const ShinkouInputHandle* input, ShinkouInputMouseState* state) {
    if (input == nullptr || state == nullptr) return 0;
    *state = input->mouse;
    return 1;
}

SHINKOU_INPUT_API uint32_t SHINKOU_INPUT_CALL shinkou_input_gamepad_count(const ShinkouInputHandle* input) {
    return input == nullptr ? 0u : static_cast<uint32_t>(input->gamepad_states.size());
}

SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_gamepad(const ShinkouInputHandle* input, uint32_t index, ShinkouInputGamepadState* state) {
    if (input == nullptr || state == nullptr || index >= input->gamepad_states.size()) return 0;
    *state = input->gamepad_states[index];
    return 1;
}

SHINKOU_INPUT_API uint32_t SHINKOU_INPUT_CALL shinkou_input_event_count(const ShinkouInputHandle* input) {
    return input == nullptr ? 0u : static_cast<uint32_t>(input->events.size());
}

SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_event(const ShinkouInputHandle* input, uint32_t index, ShinkouInputEvent* event) {
    if (input == nullptr || event == nullptr || index >= input->events.size()) return 0;
    *event = input->events[index];
    return 1;
}
}
