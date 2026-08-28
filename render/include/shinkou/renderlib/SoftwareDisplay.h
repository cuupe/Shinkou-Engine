#pragma once

#include "Renderer.h"
#include <string>
#include <vector>

namespace shinkou::renderlib {

class SoftwareDisplayBackend final : public IDisplayBackend {
public:
    bool initialize(const RendererConfig& config) override;
    void shutdown() override;
    bool resize(Size size) override;
    void begin_frame(Color clear) override;
    void submit(const DisplayList& list) override;
    bool end_frame() override;
    RendererCapabilities capabilities() const override;
    RendererStats stats() const override { return m_stats; }
    std::string last_error() const override { return m_error; }
    Size size() const { return m_size; }
    const std::vector<std::uint8_t>& pixels() const { return m_pixels; }
    Color pixel(std::uint32_t x, std::uint32_t y) const;
    bool save_ppm(const std::string& path) const;

private:
    void blend_pixel(int x, int y, Color color);
    void fill_rect(Rect rect, Color color);
    void fill_rounded_rect(Rect rect, Color color, float radius);
    void draw_border(Rect rect, Color color, float thickness);
    void draw_line(Point from, Point to, Color color, float thickness);
    void draw_gradient(Rect rect, Color start, Color end);
    Size m_size{};
    std::vector<std::uint8_t> m_pixels;
    RendererStats m_stats{};
    std::string m_error;
    bool m_initialized = false;
};

std::unique_ptr<SoftwareDisplayBackend> create_software_backend();

} // namespace shinkou::renderlib
