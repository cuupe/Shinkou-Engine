#include "shinkou/renderlib/DisplayList.h"
#include <utility>

namespace shinkou::renderlib {

void DisplayList::clear() { m_commands.clear(); }
void DisplayList::reserve(std::size_t count) { m_commands.reserve(count); }
void DisplayList::clear_color(Color color) { DisplayCommand command; command.type = DisplayCommandType::Clear; command.color = color; m_commands.push_back(std::move(command)); }
void DisplayList::rect(Rect bounds, Color color, float radius) { DisplayCommand command; command.type = DisplayCommandType::Rect; command.rect = bounds; command.color = color; command.radius = radius; m_commands.push_back(std::move(command)); }
void DisplayList::border(Rect bounds, Color color, float thickness, float radius) { DisplayCommand command; command.type = DisplayCommandType::Border; command.rect = bounds; command.color = color; command.thickness = thickness; command.radius = radius; m_commands.push_back(std::move(command)); }
void DisplayList::line(Point from, Point to, Color color, float thickness) { DisplayCommand command; command.type = DisplayCommandType::Line; command.from = from; command.to = to; command.color = color; command.thickness = thickness; m_commands.push_back(std::move(command)); }
void DisplayList::gradient(Rect bounds, Color start, Color end) { DisplayCommand command; command.type = DisplayCommandType::Gradient; command.rect = bounds; command.color = start; command.secondary = end; m_commands.push_back(std::move(command)); }

} // namespace shinkou::renderlib
