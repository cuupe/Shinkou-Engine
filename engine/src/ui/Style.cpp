#include "shinkou/data/Read.h"
#include "shinkou/data/Json.h"
#include "shinkou/ui/Style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shinkou::ui {
namespace {

void set_error(std::string *error, std::string message) {
    if (error)
        *error = std::move(message);
}

bool json_color(const data::Value& root, std::string_view key, ThemeColor& output) {
    const auto* value = root.find(key);
    if (!value || value->kind() != data::Value::Kind::Array || value->array_items().size() != 4) return false;
    float channels[4];
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& channel = value->array_items()[i];
        const auto number = channel.number_or();
        if (channel.kind() != data::Value::Kind::Number || !std::isfinite(number) || number < 0 || number > 1) return false;
        channels[i] = static_cast<float>(number);
    }
    output = {channels[0],channels[1],channels[2],channels[3]}; return true;
}

const char *mode_name(BackgroundMode mode) noexcept {
    switch (mode) {
    case BackgroundMode::Gradient:
        return "gradient";
    case BackgroundMode::Image:
        return "image";
    default:
        return "solid";
    }
}

BackgroundMode parse_mode(std::string_view mode) noexcept {
    if (mode == "gradient")
        return BackgroundMode::Gradient;
    if (mode == "image")
        return BackgroundMode::Image;
    return BackgroundMode::Solid;
}

std::string xml_escape(std::string_view input) {
    std::string output;
    for (const char character : input) {
        if (character == '&')
            output += "&amp;";
        else if (character == '<')
            output += "&lt;";
        else if (character == '>')
            output += "&gt;";
        else
            output.push_back(character);
    }
    return output;
}

bool xml_value(std::string_view input, std::string_view tag, std::string &output) {
    const std::string open = '<' + std::string(tag) + '>';
    const std::string close = "</" + std::string(tag) + '>';
    const auto start = input.find(open);
    if (start == std::string_view::npos)
        return false;
    const auto content = start + open.size();
    const auto end = input.find(close, content);
    if (end == std::string_view::npos)
        return false;
    output.assign(input.substr(content, end - content));
    return true;
}

bool parse_csv_color(std::string_view value, ThemeColor &output) {
    std::stringstream stream{std::string(value)};
    float values[4]{};
    char comma = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        float &component = values[index];
        if (!(stream >> component))
            return false;
        if (index < 3 && (!(stream >> comma) || comma != ','))
            return false;
    }
    output = ThemeColor{values[0], values[1], values[2], values[3]};
    return output.valid();
}

} // namespace

bool UiStyleConfig::valid(std::string *error) const {
    if (version != kStyleSchemaVersion) {
        set_error(error, "unsupported UI style version");
        return false;
    }
    if (activeTheme.empty() || !std::isfinite(uiScale) || uiScale < 0.5f || uiScale > 3.0f ||
        !std::isfinite(fontSize) || fontSize < 6.0f || fontSize > 96.0f) {
        set_error(error, "UI scale or font size is invalid");
        return false;
    }
    if (!background.primary.valid() || !background.secondary.valid() ||
        !std::isfinite(background.opacity) || background.opacity < 0.0f ||
        background.opacity > 1.0f) {
        set_error(error, "UI background is invalid");
        return false;
    }
    return true;
}

std::string serialize_json(const UiStyleConfig &config, bool pretty) {
    if (!config.valid()) return {};
    auto root = data::document({kStyleSchema,config.version});
    auto& fields = root.object_items();
    fields["theme"] = config.activeTheme;
    fields["uiScale"] = config.uiScale;
    fields["fontSize"] = config.fontSize;
    fields["fontPath"] = config.fontPath;
    fields["fallbackFontPath"] = config.fallbackFontPath;
    fields["compactControls"] = config.compactControls;
    fields["enableAnimations"] = config.enableAnimations;
    fields["reduceMotion"] = config.reduceMotion;
    auto background = data::Value::object();
    background.object_items() = {{"mode",mode_name(config.background.mode)},
        {"primary",data::Value::Array{config.background.primary.r,config.background.primary.g,config.background.primary.b,config.background.primary.a}},
        {"secondary",data::Value::Array{config.background.secondary.r,config.background.secondary.g,config.background.secondary.b,config.background.secondary.a}},
        {"imagePath",config.background.imagePath},{"opacity",config.background.opacity}};
    fields["background"] = std::move(background);
    auto overrides = data::Value::object();
    auto colors = data::Value::object();
    for (const auto& [name,color] : config.themeOverride.colors)
        colors.object_items()[name] = data::Value::Array{color.r,color.g,color.b,color.a};
    overrides.object_items()["colors"] = std::move(colors);
    fields["themeOverride"] = std::move(overrides);
    auto encoded = data::write_json(root,{pretty});
    return encoded ? std::move(encoded).value() : std::string{};
}

bool deserialize_json(std::string_view json, UiStyleConfig &config, std::string *error) {
    if (json.size() > 1024u * 1024u) {
        set_error(error, "UI style JSON is too large");
        return false;
    }
    auto decoded = data::parse_json(json, data::JsonLimits{1024u*1024u, 32});
    if (!decoded) { set_error(error, decoded.error().describe()); return false; }
    const auto& root = decoded.value();
    const auto schema = data::validate_document(root, {kStyleSchema, kStyleSchemaVersion});
    if (!schema) { set_error(error, schema.error().describe()); return false; }
    UiStyleConfig parsed = config;
    data::ObjectReader fields(root);
    fields.read("theme",parsed.activeTheme);
    fields.read("uiScale",parsed.uiScale);
    fields.read("fontSize",parsed.fontSize);
    fields.read("fontPath",parsed.fontPath);
    fields.read("fallbackFontPath",parsed.fallbackFontPath);
    fields.read("compactControls",parsed.compactControls);
    fields.read("enableAnimations",parsed.enableAnimations);
    fields.read("reduceMotion",parsed.reduceMotion);
    if (!fields) { set_error(error,fields.error().describe()); return false; }
    const auto* backgroundValue = root.find("background");
    const auto& background = backgroundValue ? *backgroundValue : root;
    if (backgroundValue && background.kind() != data::Value::Kind::Object) { set_error(error,"background must be an object"); return false; }
    std::string mode;
    data::ObjectReader backgroundFields(background,"/background");
    backgroundFields.read("mode",mode);
    parsed.background.mode = parse_mode(mode);
    if (background.find("primary") && !json_color(background,"primary",parsed.background.primary)) { set_error(error,"invalid background primary color"); return false; }
    if (background.find("secondary") && !json_color(background,"secondary",parsed.background.secondary)) { set_error(error,"invalid background secondary color"); return false; }
    backgroundFields.read("imagePath",parsed.background.imagePath);
    backgroundFields.read("opacity",parsed.background.opacity);
    if (!backgroundFields) { set_error(error,backgroundFields.error().describe()); return false; }
    if (const auto* overrides = root.find("themeOverride")) {
        if (overrides->kind() != data::Value::Kind::Object) { set_error(error,"themeOverride must be an object"); return false; }
        if (const auto* colors = overrides->find("colors")) {
            if (colors->kind() != data::Value::Kind::Object) { set_error(error,"override colors must be an object"); return false; }
            for (const auto& [name,value] : colors->object_items()) {
                ThemeColor color;
                if (!json_color(*colors,name,color)) { set_error(error,"invalid override color: "+name); return false; }
                parsed.themeOverride.colors[name] = color;
            }
        }
    }
    parsed.version = kStyleSchemaVersion;
    if (!parsed.valid(error))
        return false;
    config = std::move(parsed);
    return true;
}

std::string serialize_xml(const UiStyleConfig &config, bool pretty) {
    const char *newline = pretty ? "\n" : "";
    std::ostringstream output;
    output << "<ui-style schema=\"" << kStyleSchema << "\" version=\"" << config.version << "\">"
           << newline << "<theme>" << xml_escape(config.activeTheme) << "</theme>" << newline
           << "<uiScale>" << config.uiScale << "</uiScale><fontSize>" << config.fontSize
           << "</fontSize>" << newline << "<fontPath>" << xml_escape(config.fontPath)
           << "</fontPath><fallbackFontPath>" << xml_escape(config.fallbackFontPath)
           << "</fallbackFontPath>" << newline << "<compactControls>"
           << (config.compactControls ? "true" : "false") << "</compactControls>" << newline
           << "<enableAnimations>" << (config.enableAnimations ? "true" : "false")
           << "</enableAnimations><reduceMotion>" << (config.reduceMotion ? "true" : "false")
           << "</reduceMotion>" << newline << "<background><mode>"
           << mode_name(config.background.mode) << "</mode><primary>" << config.background.primary.r
           << ',' << config.background.primary.g << ',' << config.background.primary.b << ','
           << config.background.primary.a << "</primary><secondary>"
           << config.background.secondary.r << ',' << config.background.secondary.g << ','
           << config.background.secondary.b << ',' << config.background.secondary.a
           << "</secondary><imagePath>" << xml_escape(config.background.imagePath)
           << "</imagePath><opacity>" << config.background.opacity << "</opacity></background>"
           << newline << "</ui-style>";
    return output.str();
}

bool deserialize_xml(std::string_view xml, UiStyleConfig &config, std::string *error) {
    if (xml.find("<ui-style") == std::string_view::npos ||
        xml.find("schema=\"shinkou.ui-style\"") == std::string_view::npos) {
        set_error(error, "unsupported or missing UI style XML schema");
        return false;
    }
    UiStyleConfig parsed = config;
    std::string value;
    if (xml_value(xml, "theme", value))
        parsed.activeTheme = value;
    if (xml_value(xml, "uiScale", value))
        parsed.uiScale = std::strtof(value.c_str(), nullptr);
    if (xml_value(xml, "fontSize", value))
        parsed.fontSize = std::strtof(value.c_str(), nullptr);
    if (xml_value(xml, "fontPath", value))
        parsed.fontPath = value;
    if (xml_value(xml, "fallbackFontPath", value))
        parsed.fallbackFontPath = value;
    if (xml_value(xml, "compactControls", value))
        parsed.compactControls = value == "true";
    if (xml_value(xml, "enableAnimations", value))
        parsed.enableAnimations = value == "true";
    if (xml_value(xml, "reduceMotion", value))
        parsed.reduceMotion = value == "true";
    if (xml_value(xml, "mode", value))
        parsed.background.mode = parse_mode(value);
    if (xml_value(xml, "primary", value))
        parse_csv_color(value, parsed.background.primary);
    if (xml_value(xml, "secondary", value))
        parse_csv_color(value, parsed.background.secondary);
    if (xml_value(xml, "imagePath", value))
        parsed.background.imagePath = value;
    if (xml_value(xml, "opacity", value))
        parsed.background.opacity = std::strtof(value.c_str(), nullptr);
    parsed.version = kStyleSchemaVersion;
    if (!parsed.valid(error))
        return false;
    config = std::move(parsed);
    return true;
}

bool load_style_file(const std::filesystem::path &path, UiStyleConfig &config, std::string *error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        set_error(error, "cannot open UI style file: " + path.string());
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    const auto extension = path.extension().string();
    if (extension == ".xml" || extension == ".XML")
        return deserialize_xml(contents, config, error);
    return deserialize_json(contents, config, error);
}

bool save_style_file(const std::filesystem::path &path, const UiStyleConfig &config,
                     std::string *error) {
    if (!config.valid(error))
        return false;
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) {
        set_error(error, fsError.message());
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        set_error(error, "cannot open temporary UI style file");
        return false;
    }
    const auto content =
        path.extension() == ".xml" ? serialize_xml(config) : serialize_json(config);
    file << content;
    file.close();
    std::filesystem::rename(temporary, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, path, fsError);
    }
    if (fsError) {
        std::filesystem::remove(temporary, fsError);
        set_error(error, "cannot commit UI style file");
        return false;
    }
    return true;
}

} // namespace shinkou::ui
