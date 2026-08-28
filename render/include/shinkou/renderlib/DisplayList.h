#pragma once

#include "Types.h"
#include <cstddef>
#include <vector>

namespace shinkou::renderlib {

enum class DisplayCommandType { Clear, Rect, Border, Line, Gradient };
struct DisplayCommand {
    DisplayCommandType type = DisplayCommandType::Rect;
    Rect rect{};
    Point from{};
    Point to{};
    Color color{};
    Color secondary{};
    float thickness = 1.0f;
    float radius = 0.0f;
};

class DisplayList {
public:
    void clear();
    void reserve(std::size_t count);
    void clear_color(Color color);
    void rect(Rect rect, Color color, float radius = 0.0f);
    void border(Rect rect, Color color, float thickness = 1.0f, float radius = 0.0f);
    void line(Point from, Point to, Color color, float thickness = 1.0f);
    void gradient(Rect rect, Color start, Color end);
    const std::vector<DisplayCommand>& commands() const { return m_commands; }
    std::size_t size() const { return m_commands.size(); }

private:
    std::vector<DisplayCommand> m_commands;
};

} // namespace shinkou::renderlib
