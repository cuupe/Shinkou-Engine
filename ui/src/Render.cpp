#include "shinkou/uikit/Render.h"

namespace shinkou::uikit {

void RenderList::clear() { m_commands.clear(); }
void RenderList::reserve(std::size_t count) { m_commands.reserve(count); }

void RenderList::rect(Rect bounds, Color color, Insets radii) {
    DrawCommand command;
    command.type = DrawCommandType::Rect;
    command.rect = bounds;
    command.color = color;
    command.radii = radii;
    m_commands.push_back(std::move(command));
}

void RenderList::border(Rect bounds, Color color, float width, Insets radii) {
    DrawCommand command;
    command.type = DrawCommandType::Border;
    command.rect = bounds;
    command.color = color;
    command.thickness = width;
    command.radii = radii;
    m_commands.push_back(std::move(command));
}

void RenderList::line(Vec2 from, Vec2 to, Color color, float width) {
    DrawCommand command;
    command.type = DrawCommandType::Line;
    command.from = from;
    command.to = to;
    command.color = color;
    command.thickness = width;
    m_commands.push_back(std::move(command));
}

void RenderList::text(Rect bounds, std::string value, Color color, std::string font, float size) {
    DrawCommand command;
    command.type = DrawCommandType::Text;
    command.rect = bounds;
    command.text = std::move(value);
    command.color = color;
    command.fontFamily = std::move(font);
    command.fontSize = size;
    m_commands.push_back(std::move(command));
}

void RenderList::image(Rect bounds, std::string resource, Color tint) {
    DrawCommand command;
    command.type = DrawCommandType::Image;
    command.rect = bounds;
    command.resource = std::move(resource);
    command.color = tint;
    m_commands.push_back(std::move(command));
}

void RenderList::gradient(Rect bounds, Color start, Color end, Insets radii) {
    DrawCommand command;
    command.type = DrawCommandType::Gradient;
    command.rect = bounds;
    command.color = start;
    command.secondaryColor = end;
    command.radii = radii;
    m_commands.push_back(std::move(command));
}

void RenderList::begin_clip(Rect bounds) {
    DrawCommand command;
    command.type = DrawCommandType::BeginClip;
    command.rect = bounds;
    m_commands.push_back(std::move(command));
}

void RenderList::end_clip() {
    DrawCommand command;
    command.type = DrawCommandType::EndClip;
    m_commands.push_back(std::move(command));
}

RenderStats RenderList::stats() const {
    RenderStats result;
    result.commandCount = m_commands.size();
    for (const DrawCommand& command : m_commands) {
        if (command.type == DrawCommandType::Text) ++result.textCount;
        if (command.type == DrawCommandType::Image) ++result.imageCount;
        if (command.type == DrawCommandType::BeginClip || command.type == DrawCommandType::EndClip) ++result.clipCount;
    }
    return result;
}

} // namespace shinkou::uikit

