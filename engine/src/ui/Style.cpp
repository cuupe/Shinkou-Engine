#include "shinkou/ui/Style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shinkou::ui {
namespace {

void set_error(std::string* error, std::string message) { if (error) *error = std::move(message); }

std::size_t value_start(std::string_view input, std::string_view key) {
    const auto keyPosition = input.find('"' + std::string(key) + '"');
    if (keyPosition == std::string_view::npos) return keyPosition;
    const auto colon = input.find(':', keyPosition + key.size() + 2);
    if (colon == std::string_view::npos) return colon;
    std::size_t position = colon + 1;
    while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) ++position;
    return position;
}

bool json_string(std::string_view input, std::string_view key, std::string& output) {
    std::size_t position = value_start(input, key);
    if (position == std::string_view::npos || position >= input.size() || input[position] != '"') return false;
    ++position;
    output.clear();
    while (position < input.size()) {
        char character = input[position++];
        if (character == '"') return true;
        if (character == '\\' && position < input.size()) {
            character = input[position++];
            if (character == 'n') output.push_back('\n');
            else if (character == 'r') output.push_back('\r');
            else if (character == 't') output.push_back('\t');
            else output.push_back(character);
        } else output.push_back(character);
    }
    return false;
}

bool json_float(std::string_view input, std::string_view key, float& output) {
    const auto position = value_start(input, key);
    if (position == std::string_view::npos) return false;
    const std::string number(input.substr(position));
    char* end = nullptr;
    output = std::strtof(number.c_str(), &end);
    return end != number.c_str() && std::isfinite(output);
}

bool json_bool(std::string_view input, std::string_view key, bool& output) {
    const auto position = value_start(input, key);
    if (position == std::string_view::npos) return false;
    if (input.substr(position, 4) == "true") { output = true; return true; }
    if (input.substr(position, 5) == "false") { output = false; return true; }
    return false;
}

bool json_color(std::string_view input, std::string_view key, ThemeColor& output) {
    auto position = value_start(input, key);
    if (position == std::string_view::npos || position >= input.size() || input[position] != '[') return false;
    ++position;
    float values[4]{};
    for (float& value : values) {
        while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) ++position;
        const std::string number(input.substr(position));
        char* end = nullptr;
        value = std::strtof(number.c_str(), &end);
        if (end == number.c_str() || !std::isfinite(value)) return false;
        position += static_cast<std::size_t>(end - number.c_str());
        while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) ++position;
        if (&value != &values[3]) {
            if (position >= input.size() || input[position++] != ',') return false;
        }
    }
    output = ThemeColor{values[0], values[1], values[2], values[3]};
    return output.valid();
}

std::string json_escape(std::string_view input) {
    std::ostringstream output;
    output << '"';
    for (const char character : input) {
        if (character == '"') output << "\\\"";
        else if (character == '\\') output << "\\\\";
        else if (character == '\n') output << "\\n";
        else if (character == '\r') output << "\\r";
        else if (character == '\t') output << "\\t";
        else output << character;
    }
    output << '"';
    return output.str();
}

void json_color(std::ostream& output, const ThemeColor& color) {
    output << '[' << color.r << ',' << color.g << ',' << color.b << ',' << color.a << ']';
}

const char* mode_name(BackgroundMode mode) noexcept {
    switch (mode) {
    case BackgroundMode::Gradient: return "gradient";
    case BackgroundMode::Image: return "image";
    default: return "solid";
    }
}

BackgroundMode parse_mode(std::string_view mode) noexcept {
    if (mode == "gradient") return BackgroundMode::Gradient;
    if (mode == "image") return BackgroundMode::Image;
    return BackgroundMode::Solid;
}

std::string xml_escape(std::string_view input) {
    std::string output;
    for (const char character : input) {
        if (character == '&') output += "&amp;";
        else if (character == '<') output += "&lt;";
        else if (character == '>') output += "&gt;";
        else output.push_back(character);
    }
    return output;
}

bool xml_value(std::string_view input, std::string_view tag, std::string& output) {
    const std::string open = '<' + std::string(tag) + '>';
    const std::string close = "</" + std::string(tag) + '>';
    const auto start = input.find(open);
    if (start == std::string_view::npos) return false;
    const auto content = start + open.size();
    const auto end = input.find(close, content);
    if (end == std::string_view::npos) return false;
    output.assign(input.substr(content, end - content));
    return true;
}

bool parse_csv_color(std::string_view value, ThemeColor& output) {
    std::stringstream stream{std::string(value)};
    float values[4]{};
    char comma = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        float& component = values[index];
        if (!(stream >> component)) return false;
        if (index < 3 && (!(stream >> comma) || comma != ',')) return false;
    }
    output = ThemeColor{values[0], values[1], values[2], values[3]};
    return output.valid();
}

} // namespace

bool UiStyleConfig::valid(std::string* error) const {
    if (version != kStyleSchemaVersion) { set_error(error, "unsupported UI style version"); return false; }
    if (activeTheme.empty() || !std::isfinite(uiScale) || uiScale < 0.5f || uiScale > 3.0f ||
        !std::isfinite(fontSize) || fontSize < 6.0f || fontSize > 96.0f) {
        set_error(error, "UI scale or font size is invalid"); return false;
    }
    if (!background.primary.valid() || !background.secondary.valid() || !std::isfinite(background.opacity) ||
        background.opacity < 0.0f || background.opacity > 1.0f) {
        set_error(error, "UI background is invalid"); return false;
    }
    return true;
}

std::string serialize_json(const UiStyleConfig& config, bool pretty) {
    std::ostringstream output;
    if (pretty) output << std::fixed << std::setprecision(4);
    output << "{\"schema\":\"" << kStyleSchema << "\",\"version\":" << config.version
           << ",\"theme\":" << json_escape(config.activeTheme)
           << ",\"uiScale\":" << config.uiScale << ",\"fontSize\":" << config.fontSize
           << ",\"fontPath\":" << json_escape(config.fontPath)
           << ",\"fallbackFontPath\":" << json_escape(config.fallbackFontPath)
           << ",\"compactControls\":" << (config.compactControls ? "true" : "false")
           << ",\"enableAnimations\":" << (config.enableAnimations ? "true" : "false")
           << ",\"reduceMotion\":" << (config.reduceMotion ? "true" : "false")
           << ",\"background\":{\"mode\":\"" << mode_name(config.background.mode) << "\",\"primary\":";
    json_color(output, config.background.primary);
    output << ",\"secondary\":";
    json_color(output, config.background.secondary);
    output << ",\"imagePath\":" << json_escape(config.background.imagePath)
           << ",\"opacity\":" << config.background.opacity << "},\"themeOverride\":{\"colors\":{\"accent\":";
    const auto accent = config.themeOverride.colors.find("accent");
    json_color(output, accent == config.themeOverride.colors.end() ? ThemeColor{} : accent->second);
    output << "}}}";
    return output.str();
}

bool deserialize_json(std::string_view json, UiStyleConfig& config, std::string* error) {
    if (json.size() > 1024u * 1024u) { set_error(error, "UI style JSON is too large"); return false; }
    std::string schema;
    float version = 0.0f;
    if (!json_string(json, "schema", schema) || schema != kStyleSchema || !json_float(json, "version", version) || version != 1.0f) {
        set_error(error, "unsupported or missing UI style schema/version"); return false;
    }
    UiStyleConfig parsed = config;
    json_string(json, "theme", parsed.activeTheme);
    json_float(json, "uiScale", parsed.uiScale);
    json_float(json, "fontSize", parsed.fontSize);
    json_string(json, "fontPath", parsed.fontPath);
    json_string(json, "fallbackFontPath", parsed.fallbackFontPath);
    json_bool(json, "compactControls", parsed.compactControls);
    json_bool(json, "enableAnimations", parsed.enableAnimations);
    json_bool(json, "reduceMotion", parsed.reduceMotion);
    std::string mode;
    json_string(json, "mode", mode);
    parsed.background.mode = parse_mode(mode);
    json_color(json, "primary", parsed.background.primary);
    json_color(json, "secondary", parsed.background.secondary);
    json_string(json, "imagePath", parsed.background.imagePath);
    json_float(json, "opacity", parsed.background.opacity);
    parsed.version = kStyleSchemaVersion;
    if (!parsed.valid(error)) return false;
    config = std::move(parsed);
    return true;
}

std::string serialize_xml(const UiStyleConfig& config, bool pretty) {
    const char* newline = pretty ? "\n" : "";
    std::ostringstream output;
    output << "<ui-style schema=\"" << kStyleSchema << "\" version=\"" << config.version << "\">" << newline
           << "<theme>" << xml_escape(config.activeTheme) << "</theme>" << newline
           << "<uiScale>" << config.uiScale << "</uiScale><fontSize>" << config.fontSize << "</fontSize>" << newline
           << "<fontPath>" << xml_escape(config.fontPath) << "</fontPath><fallbackFontPath>" << xml_escape(config.fallbackFontPath) << "</fallbackFontPath>" << newline
           << "<compactControls>" << (config.compactControls ? "true" : "false") << "</compactControls>" << newline
           << "<enableAnimations>" << (config.enableAnimations ? "true" : "false") << "</enableAnimations><reduceMotion>" << (config.reduceMotion ? "true" : "false") << "</reduceMotion>" << newline
           << "<background><mode>" << mode_name(config.background.mode) << "</mode><primary>" << config.background.primary.r << ',' << config.background.primary.g << ',' << config.background.primary.b << ',' << config.background.primary.a << "</primary><secondary>"
           << config.background.secondary.r << ',' << config.background.secondary.g << ',' << config.background.secondary.b << ',' << config.background.secondary.a << "</secondary><imagePath>" << xml_escape(config.background.imagePath) << "</imagePath><opacity>" << config.background.opacity << "</opacity></background>" << newline
           << "</ui-style>";
    return output.str();
}

bool deserialize_xml(std::string_view xml, UiStyleConfig& config, std::string* error) {
    if (xml.find("<ui-style") == std::string_view::npos || xml.find("schema=\"shinkou.ui-style\"") == std::string_view::npos) {
        set_error(error, "unsupported or missing UI style XML schema"); return false;
    }
    UiStyleConfig parsed = config;
    std::string value;
    if (xml_value(xml, "theme", value)) parsed.activeTheme = value;
    if (xml_value(xml, "uiScale", value)) parsed.uiScale = std::strtof(value.c_str(), nullptr);
    if (xml_value(xml, "fontSize", value)) parsed.fontSize = std::strtof(value.c_str(), nullptr);
    if (xml_value(xml, "fontPath", value)) parsed.fontPath = value;
    if (xml_value(xml, "fallbackFontPath", value)) parsed.fallbackFontPath = value;
    if (xml_value(xml, "compactControls", value)) parsed.compactControls = value == "true";
    if (xml_value(xml, "enableAnimations", value)) parsed.enableAnimations = value == "true";
    if (xml_value(xml, "reduceMotion", value)) parsed.reduceMotion = value == "true";
    if (xml_value(xml, "mode", value)) parsed.background.mode = parse_mode(value);
    if (xml_value(xml, "primary", value)) parse_csv_color(value, parsed.background.primary);
    if (xml_value(xml, "secondary", value)) parse_csv_color(value, parsed.background.secondary);
    if (xml_value(xml, "imagePath", value)) parsed.background.imagePath = value;
    if (xml_value(xml, "opacity", value)) parsed.background.opacity = std::strtof(value.c_str(), nullptr);
    parsed.version = kStyleSchemaVersion;
    if (!parsed.valid(error)) return false;
    config = std::move(parsed);
    return true;
}

bool load_style_file(const std::filesystem::path& path, UiStyleConfig& config, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { set_error(error, "cannot open UI style file: " + path.string()); return false; }
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto extension = path.extension().string();
    if (extension == ".xml" || extension == ".XML") return deserialize_xml(contents, config, error);
    return deserialize_json(contents, config, error);
}

bool save_style_file(const std::filesystem::path& path, const UiStyleConfig& config, std::string* error) {
    if (!config.valid(error)) return false;
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) { set_error(error, fsError.message()); return false; }
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) { set_error(error, "cannot open temporary UI style file"); return false; }
    const auto content = path.extension() == ".xml" ? serialize_xml(config) : serialize_json(config);
    file << content;
    file.close();
    std::filesystem::rename(temporary, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, path, fsError);
    }
    if (fsError) { std::filesystem::remove(temporary, fsError); set_error(error, "cannot commit UI style file"); return false; }
    return true;
}

} // namespace shinkou::ui
