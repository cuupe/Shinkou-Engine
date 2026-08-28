#include "shinkou/renderlib/SoftwareDisplay.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace shinkou::renderlib {

Color Color::from_rgba8(std::uint32_t value) { return {((value >> 24) & 0xff) / 255.0f, ((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f}; }
std::uint32_t Color::to_rgba8() const { return (static_cast<std::uint32_t>(to_byte(r)) << 24) | (static_cast<std::uint32_t>(to_byte(g)) << 16) | (static_cast<std::uint32_t>(to_byte(b)) << 8) | to_byte(a); }

bool SoftwareDisplayBackend::initialize(const RendererConfig& config) { m_size = config.size; m_pixels.assign(static_cast<std::size_t>(m_size.width) * m_size.height * 4, 0); m_initialized = true; m_error.clear(); return true; }
void SoftwareDisplayBackend::shutdown() { m_pixels.clear(); m_size = {}; m_initialized = false; }
bool SoftwareDisplayBackend::resize(Size size) { if (!m_initialized) return false; m_size = size; m_pixels.assign(static_cast<std::size_t>(size.width) * size.height * 4, 0); return true; }
void SoftwareDisplayBackend::begin_frame(Color clear) { if (!m_initialized) return; for (std::size_t i = 0; i < m_pixels.size(); i += 4) { m_pixels[i] = to_byte(clear.r); m_pixels[i + 1] = to_byte(clear.g); m_pixels[i + 2] = to_byte(clear.b); m_pixels[i + 3] = to_byte(clear.a); } }
void SoftwareDisplayBackend::blend_pixel(int x, int y, Color color) {
    if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= m_size.width || static_cast<std::uint32_t>(y) >= m_size.height) return;
    const std::size_t index = (static_cast<std::size_t>(y) * m_size.width + static_cast<std::size_t>(x)) * 4;
    const float alpha = std::clamp(color.a, 0.0f, 1.0f);
    m_pixels[index] = to_byte(color.r * alpha + m_pixels[index] / 255.0f * (1.0f - alpha));
    m_pixels[index + 1] = to_byte(color.g * alpha + m_pixels[index + 1] / 255.0f * (1.0f - alpha));
    m_pixels[index + 2] = to_byte(color.b * alpha + m_pixels[index + 2] / 255.0f * (1.0f - alpha));
    m_pixels[index + 3] = 255;
}
void SoftwareDisplayBackend::fill_rect(Rect rect, Color color) { const int left = static_cast<int>(std::floor(rect.x)); const int top = static_cast<int>(std::floor(rect.y)); const int right = static_cast<int>(std::ceil(rect.x + rect.width)); const int bottom = static_cast<int>(std::ceil(rect.y + rect.height)); for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) blend_pixel(x, y, color); }
static bool inside_rounded_rect(Rect rect, float radius, float px, float py) { if (radius <= 0.0f) return px >= rect.x && py >= rect.y && px <= rect.x + rect.width && py <= rect.y + rect.height; const float rx = std::min(radius, rect.width * 0.5f); const float ry = std::min(radius, rect.height * 0.5f); const float cx = px < rect.x + rx ? rect.x + rx : (px > rect.x + rect.width - rx ? rect.x + rect.width - rx : px); const float cy = py < rect.y + ry ? rect.y + ry : (py > rect.y + rect.height - ry ? rect.y + rect.height - ry : py); const float dx = px - cx; const float dy = py - cy; return dx * dx + dy * dy <= rx * ry; }
void SoftwareDisplayBackend::fill_rounded_rect(Rect rect, Color color, float radius) { const int left = static_cast<int>(std::floor(rect.x)); const int top = static_cast<int>(std::floor(rect.y)); const int right = static_cast<int>(std::ceil(rect.x + rect.width)); const int bottom = static_cast<int>(std::ceil(rect.y + rect.height)); for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) if (inside_rounded_rect(rect, radius, x + 0.5f, y + 0.5f)) blend_pixel(x, y, color); }
void SoftwareDisplayBackend::draw_border(Rect rect, Color color, float thickness) { fill_rect({rect.x, rect.y, rect.width, thickness}, color); fill_rect({rect.x, rect.y + rect.height - thickness, rect.width, thickness}, color); fill_rect({rect.x, rect.y, thickness, rect.height}, color); fill_rect({rect.x + rect.width - thickness, rect.y, thickness, rect.height}, color); }
void SoftwareDisplayBackend::draw_line(Point from, Point to, Color color, float thickness) { const int steps = std::max(1, static_cast<int>(std::max(std::abs(to.x - from.x), std::abs(to.y - from.y)))); const float radius = std::max(0.5f, thickness * 0.5f); for (int i = 0; i <= steps; ++i) { const float t = static_cast<float>(i) / steps; fill_rect({from.x + (to.x - from.x) * t - radius, from.y + (to.y - from.y) * t - radius, radius * 2, radius * 2}, color); } }
void SoftwareDisplayBackend::draw_gradient(Rect rect, Color start, Color end) { const int top = static_cast<int>(std::floor(rect.y)); const int bottom = static_cast<int>(std::ceil(rect.y + rect.height)); for (int y = top; y < bottom; ++y) { const float t = rect.height <= 0 ? 0.0f : std::clamp((static_cast<float>(y) - rect.y) / rect.height, 0.0f, 1.0f); fill_rect({rect.x, static_cast<float>(y), rect.width, 1}, {start.r + (end.r - start.r) * t, start.g + (end.g - start.g) * t, start.b + (end.b - start.b) * t, start.a + (end.a - start.a) * t}); } }
void SoftwareDisplayBackend::submit(const DisplayList& list) { if (!m_initialized) return; for (const DisplayCommand& command : list.commands()) { switch (command.type) { case DisplayCommandType::Clear: begin_frame(command.color); break; case DisplayCommandType::Rect: fill_rounded_rect(command.rect, command.color, command.radius); break; case DisplayCommandType::Border: draw_border(command.rect, command.color, command.thickness); break; case DisplayCommandType::Line: draw_line(command.from, command.to, command.color, command.thickness); break; case DisplayCommandType::Gradient: draw_gradient(command.rect, command.color, command.secondary); break; } ++m_stats.commands; ++m_stats.drawCalls; } }
bool SoftwareDisplayBackend::end_frame() { if (!m_initialized) return false; ++m_stats.frames; ++m_stats.presents; return true; }
RendererCapabilities SoftwareDisplayBackend::capabilities() const { return {m_initialized, true, true, PixelFormat::RGBA8Unorm}; }
Color SoftwareDisplayBackend::pixel(std::uint32_t x, std::uint32_t y) const { if (x >= m_size.width || y >= m_size.height) return {}; const std::size_t index = (static_cast<std::size_t>(y) * m_size.width + x) * 4; return Color::from_rgba8((static_cast<std::uint32_t>(m_pixels[index]) << 24) | (static_cast<std::uint32_t>(m_pixels[index + 1]) << 16) | (static_cast<std::uint32_t>(m_pixels[index + 2]) << 8) | m_pixels[index + 3]); }
bool SoftwareDisplayBackend::save_ppm(const std::string& path) const { std::ofstream output(path, std::ios::binary); if (!output) return false; output << "P6\n" << m_size.width << ' ' << m_size.height << "\n255\n"; for (std::size_t i = 0; i < m_pixels.size(); i += 4) { output.put(static_cast<char>(m_pixels[i])); output.put(static_cast<char>(m_pixels[i + 1])); output.put(static_cast<char>(m_pixels[i + 2])); } return static_cast<bool>(output); }
std::unique_ptr<SoftwareDisplayBackend> create_software_backend() { return std::make_unique<SoftwareDisplayBackend>(); }

} // namespace shinkou::renderlib
