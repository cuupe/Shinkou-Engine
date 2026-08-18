#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(SHINKOU_INPUT_BUILDING_DLL)
#define SHINKOU_INPUT_API __declspec(dllexport)
#else
#define SHINKOU_INPUT_API __declspec(dllimport)
#endif
#define SHINKOU_INPUT_CALL __cdecl
#else
#define SHINKOU_INPUT_API
#define SHINKOU_INPUT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShinkouInputHandle ShinkouInputHandle;

typedef struct ShinkouInputMouseState {
    float x;
    float y;
    float delta_x;
    float delta_y;
    float wheel_x;
    float wheel_y;
    uint8_t buttons[8];
} ShinkouInputMouseState;

typedef struct ShinkouInputGamepadState {
    uint32_t player_index;
    int64_t instance_id;
    uint8_t connected;
    uint8_t buttons[32];
    float axes[6];
    char name[128];
} ShinkouInputGamepadState;

enum {
    SHINKOU_INPUT_EVENT_QUIT = 0,
    SHINKOU_INPUT_EVENT_KEY_DOWN,
    SHINKOU_INPUT_EVENT_KEY_UP,
    SHINKOU_INPUT_EVENT_TEXT_INPUT,
    SHINKOU_INPUT_EVENT_MOUSE_MOVE,
    SHINKOU_INPUT_EVENT_MOUSE_BUTTON_DOWN,
    SHINKOU_INPUT_EVENT_MOUSE_BUTTON_UP,
    SHINKOU_INPUT_EVENT_MOUSE_WHEEL,
    SHINKOU_INPUT_EVENT_GAMEPAD_ADDED,
    SHINKOU_INPUT_EVENT_GAMEPAD_REMOVED,
    SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_DOWN,
    SHINKOU_INPUT_EVENT_GAMEPAD_BUTTON_UP,
    SHINKOU_INPUT_EVENT_GAMEPAD_AXIS_MOTION
};

typedef struct ShinkouInputEvent {
    uint32_t type;
    uint32_t device;
    float value;
    float x;
    float y;
    float delta_x;
    float delta_y;
    uint8_t repeat;
    char control[128];
    char text[256];
} ShinkouInputEvent;

SHINKOU_INPUT_API ShinkouInputHandle* SHINKOU_INPUT_CALL shinkou_input_create(void* native_window);
SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_destroy(ShinkouInputHandle* input);
SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_initialize(ShinkouInputHandle* input);
SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_shutdown(ShinkouInputHandle* input);
SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_attach_window(ShinkouInputHandle* input, void* native_window);
SHINKOU_INPUT_API void SHINKOU_INPUT_CALL shinkou_input_poll(ShinkouInputHandle* input);
SHINKOU_INPUT_API float SHINKOU_INPUT_CALL shinkou_input_control_value(const ShinkouInputHandle* input, const char* control);
SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_mouse(const ShinkouInputHandle* input, ShinkouInputMouseState* state);
SHINKOU_INPUT_API uint32_t SHINKOU_INPUT_CALL shinkou_input_gamepad_count(const ShinkouInputHandle* input);
SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_gamepad(const ShinkouInputHandle* input, uint32_t index, ShinkouInputGamepadState* state);
SHINKOU_INPUT_API uint32_t SHINKOU_INPUT_CALL shinkou_input_event_count(const ShinkouInputHandle* input);
SHINKOU_INPUT_API int SHINKOU_INPUT_CALL shinkou_input_get_event(const ShinkouInputHandle* input, uint32_t index, ShinkouInputEvent* event);

#ifdef __cplusplus
}
#endif
