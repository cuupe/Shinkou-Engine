#include "shinkou/uikit/Animation.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    AnimationTimeline timeline;
    float value = 0.0f;
    timeline.add_float(0.0f, 100.0f, 100.0f, [&value](const AnimationTimeline::Value& next) { value = std::get<float>(next); });
    timeline.update(0.05f);
    assert(value > 0.0f && value < 100.0f);
    timeline.update(0.05f);
    assert(value == 100.0f && timeline.size() == 0);
    Color color{};
    timeline.add_color({}, {1, 0, 0, 1}, 500.0f, [&color](const AnimationTimeline::Value& next) { color = std::get<Color>(next); });
    timeline.update(0.01f, {true});
    assert(color.r == 1.0f && timeline.size() == 0);
    return 0;
}

