#include "shinkou/uikit/ShinkouUI.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace shinkou::uikit;

static std::string svg_color(Color color) {
    std::ostringstream stream;
    stream << '#' << std::hex << std::setw(8) << std::setfill('0') << color.to_rgba8();
    return stream.str();
}

static std::string xml_escape(const std::string& value) {
    std::string result;
    for (const char character : value) {
        if (character == '&') result += "&amp;";
        else if (character == '<') result += "&lt;";
        else if (character == '>') result += "&gt;";
        else result += character;
    }
    return result;
}

static void write_svg(const RenderList& list, Size viewport) {
    std::ofstream file("shinkou_ui_preview.svg", std::ios::binary);
    if (!file) return;
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << viewport.width << "\" height=\"" << viewport.height << "\" viewBox=\"0 0 " << viewport.width << ' ' << viewport.height << "\">\n";
    file << "<style>text{font-family:'Microsoft YaHei',sans-serif;dominant-baseline:middle} .muted{fill:#64748b;font-size:13px}</style>\n";
    for (const DrawCommand& command : list.commands()) {
        if (command.type == DrawCommandType::Rect) file << "<rect x=\"" << command.rect.x << "\" y=\"" << command.rect.y << "\" width=\"" << command.rect.width << "\" height=\"" << command.rect.height << "\" rx=\"" << command.radii.left << "\" fill=\"" << svg_color(command.color) << "\"/>\n";
        else if (command.type == DrawCommandType::Border) file << "<rect x=\"" << command.rect.x << "\" y=\"" << command.rect.y << "\" width=\"" << command.rect.width << "\" height=\"" << command.rect.height << "\" rx=\"" << command.radii.left << "\" fill=\"none\" stroke=\"" << svg_color(command.color) << "\" stroke-width=\"" << command.thickness << "\"/>\n";
        else if (command.type == DrawCommandType::Line) file << "<line x1=\"" << command.from.x << "\" y1=\"" << command.from.y << "\" x2=\"" << command.to.x << "\" y2=\"" << command.to.y << "\" stroke=\"" << svg_color(command.color) << "\" stroke-width=\"" << command.thickness << "\" stroke-linecap=\"round\"/>\n";
        else if (command.type == DrawCommandType::Text) file << "<text x=\"" << command.rect.x + 12 << "\" y=\"" << command.rect.y + command.rect.height * 0.5f << "\" fill=\"" << svg_color(command.color) << "\" font-size=\"" << command.fontSize << "\">" << xml_escape(command.text) << "</text>\n";
        else if (command.type == DrawCommandType::Image) { file << "<rect x=\"" << command.rect.x << "\" y=\"" << command.rect.y << "\" width=\"" << command.rect.width << "\" height=\"" << command.rect.height << "\" fill=\"#e2e8f0\"/>\n"; file << "<text class=\"muted\" x=\"" << command.rect.x + 16 << "\" y=\"" << command.rect.y + 24 << "\">" << xml_escape(command.resource) << "</text>\n"; }
    }
    file << "</svg>\n";
}

int main() {
    UiContext ui;
    ui.set_viewport({1280, 720}, 1.0f);
    Panel& toolbar = ui.root().emplace<Panel>();
    toolbar.layout = LayoutMode::Horizontal;
    toolbar.padding = Insets(12, 8);
    Button& play = toolbar.emplace<Button>("运行场景");
    play.styleClass = "primary-button";
    toolbar.emplace<Button>("打开项目");
    toolbar.emplace<Slider>().value = 0.75f;
    TextBox& search = toolbar.emplace<TextBox>("搜索资源...");
    search.flex = 1.0f;

    Panel& content = ui.root().emplace<Panel>();
    content.flex = 1.0f;
    content.clipChildren = true;
    content.emplace<Image>("asset://preview/scene.png");
    content.children().back()->flex = 1.0f;

    ui.tick(1.0f / 60.0f);
    RenderList list;
    ui.paint(list);
    write_svg(list, ui.viewport());
    const RenderStats stats = list.stats();
    std::cout << "ShinkouUI preview\n"
              << "font=" << ui.style().font.family << "\n"
              << "viewport=" << ui.viewport().width << 'x' << ui.viewport().height << "\n"
              << "commands=" << stats.commandCount << " text=" << stats.textCount << " images=" << stats.imageCount << '\n';
    return list.commands().empty() ? 1 : 0;
}
