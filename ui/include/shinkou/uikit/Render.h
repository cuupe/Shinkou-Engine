#pragma once

#include "Types.h"
#include <cstddef>
#include <string>
#include <vector>

namespace shinkou::uikit {

enum class DrawCommandType { BeginClip, EndClip, Rect, Border, Line, Text, Image, Gradient };

struct DrawCommand {
    DrawCommandType type = DrawCommandType::Rect;
    Rect rect{};
    Vec2 from{};
    Vec2 to{};
    Color color{};
    Color secondaryColor{};
    Insets radii{};
    float thickness = 0.0f;
    std::string text;
    std::string resource;
    std::string fontFamily;
    float fontSize = 14.0f;
    std::uint32_t flags = 0;
};

struct RenderStats {
    std::size_t commandCount = 0;
    std::size_t textCount = 0;
    std::size_t imageCount = 0;
    std::size_t clipCount = 0;
};

class RenderList {
public:
    void clear();
    void reserve(std::size_t count);
    void rect(Rect bounds, Color color, Insets radii = {});
    void border(Rect bounds, Color color, float width, Insets radii = {});
    void line(Vec2 from, Vec2 to, Color color, float width = 1.0f);
    void text(Rect bounds, std::string value, Color color, std::string font = "Microsoft YaHei", float size = 14.0f);
    void image(Rect bounds, std::string resource, Color tint = {1, 1, 1, 1});
    void gradient(Rect bounds, Color start, Color end, Insets radii = {});
    void begin_clip(Rect bounds);
    void end_clip();
    const std::vector<DrawCommand>& commands() const { return m_commands; }
    RenderStats stats() const;

private:
    std::vector<DrawCommand> m_commands;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual void begin_frame(Size viewport, float dpiScale) = 0;
    virtual void submit(const RenderList& list) = 0;
    virtual void end_frame() = 0;
};

} // namespace shinkou::uikit

