#pragma once

#include <array>
#include <functional>
#include <string>

namespace shinkou::editor::widgets {
struct EditEvent {
    bool changed{}, activated{}, deactivated{}, submitted{}, cancelled{}, reset{};
};
EditEvent text_input(const char *id, std::string &value, const char *hint = "", int flags = 0,
                     std::size_t limit = 4096);
bool checkbox(const char *label, bool &value);
bool slider(const char *label, float &value, float minimum, float maximum,
            const char *format = "%.2f");
void clipped_text(const char *text, float width);

struct VectorInput {
    float scale{1}, speed{0.025f}, minimum{}, maximum{};
    std::array<float, 4> resetValues{};
    const char *resetLabel{"Reset"};
    std::function<void(int, const EditEvent &)> onAxis;
};
// Shared XYZ/XYZW control: identical reset, sizing, focus and editing behavior
// for reflected properties, transforms and asset settings.
void vector_input(float *values, int count, const VectorInput &options);
} // namespace shinkou::editor::widgets
