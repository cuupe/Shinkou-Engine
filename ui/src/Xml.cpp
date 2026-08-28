#include "shinkou/uikit/Xml.h"
#include <cctype>

namespace shinkou::uikit {
namespace {

class Parser {
public:
    Parser(const std::string& input, std::string* error, std::size_t maxDepth)
        : m_input(input), m_error(error), m_maxDepth(maxDepth) {}

    bool parse(XmlNode& result) {
        skip_misc();
        if (!parse_node(result, 0)) return false;
        skip_misc();
        if (m_pos != m_input.size()) return fail("unexpected content after root element");
        return true;
    }

private:
    bool fail(const char* message) {
        if (m_error) *m_error = std::string(message) + " at offset " + std::to_string(m_pos);
        return false;
    }
    void skip_space() { while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos]))) ++m_pos; }
    bool starts(const char* value) const { return m_input.compare(m_pos, std::char_traits<char>::length(value), value) == 0; }
    void skip_comment() {
        const std::size_t end = m_input.find("-->", m_pos + 4);
        m_pos = end == std::string::npos ? m_input.size() : end + 3;
    }
    void skip_misc() {
        while (true) {
            skip_space();
            if (starts("<?")) {
                const std::size_t end = m_input.find("?>", m_pos + 2);
                m_pos = end == std::string::npos ? m_input.size() : end + 2;
            } else if (starts("<!--")) {
                skip_comment();
            } else {
                return;
            }
        }
    }
    std::string name() {
        const std::size_t begin = m_pos;
        while (m_pos < m_input.size() && (std::isalnum(static_cast<unsigned char>(m_input[m_pos])) || m_input[m_pos] == '_' || m_input[m_pos] == '-' || m_input[m_pos] == ':')) ++m_pos;
        return m_input.substr(begin, m_pos - begin);
    }
    std::string decode(std::string value) {
        const std::pair<const char*, const char*> entities[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
        for (const auto& entity : entities) {
            std::size_t position = 0;
            while ((position = value.find(entity.first, position)) != std::string::npos) {
                value.replace(position, std::char_traits<char>::length(entity.first), entity.second);
                position += std::char_traits<char>::length(entity.second);
            }
        }
        return value;
    }
    bool parse_quoted(std::string& value) {
        skip_space();
        if (m_pos >= m_input.size() || (m_input[m_pos] != '\'' && m_input[m_pos] != '"')) return fail("expected quoted attribute");
        const char quote = m_input[m_pos++];
        const std::size_t begin = m_pos;
        while (m_pos < m_input.size() && m_input[m_pos] != quote) ++m_pos;
        if (m_pos >= m_input.size()) return fail("unterminated attribute");
        value = decode(m_input.substr(begin, m_pos - begin));
        ++m_pos;
        return true;
    }
    bool parse_node(XmlNode& node, std::size_t depth) {
        skip_misc();
        if (depth > m_maxDepth) return fail("maximum XML depth exceeded");
        if (m_pos >= m_input.size() || m_input[m_pos++] != '<') return fail("expected element");
        if (m_pos < m_input.size() && (m_input[m_pos] == '/' || m_input[m_pos] == '!' || m_input[m_pos] == '?')) return fail("invalid element");
        node.name = name();
        if (node.name.empty()) return fail("element name is empty");
        while (true) {
            skip_space();
            if (starts("/>")) { m_pos += 2; return true; }
            if (m_pos < m_input.size() && m_input[m_pos] == '>') { ++m_pos; break; }
            const std::string key = name();
            if (key.empty()) return fail("expected attribute");
            skip_space();
            if (m_pos >= m_input.size() || m_input[m_pos++] != '=') return fail("expected '='");
            std::string value;
            if (!parse_quoted(value)) return false;
            node.attributes[key] = value;
        }
        while (true) {
            if (m_pos >= m_input.size()) return fail("unterminated element");
            if (starts("</")) {
                m_pos += 2;
                const std::string closeName = name();
                skip_space();
                if (m_pos >= m_input.size() || m_input[m_pos++] != '>') return fail("expected closing '>'");
                if (closeName != node.name) return fail("mismatched closing element");
                return true;
            }
            if (starts("<!--")) { skip_comment(); continue; }
            if (m_input[m_pos] == '<') {
                XmlNode child;
                if (!parse_node(child, depth + 1)) return false;
                node.children.push_back(std::move(child));
            } else {
                const std::size_t begin = m_pos;
                while (m_pos < m_input.size() && m_input[m_pos] != '<') ++m_pos;
                node.text += decode(m_input.substr(begin, m_pos - begin));
            }
        }
    }

    const std::string& m_input;
    std::string* m_error = nullptr;
    std::size_t m_maxDepth = 64;
    std::size_t m_pos = 0;
};

void write_escaped(std::string& out, const std::string& value, bool attribute) {
    for (char character : value) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': if (attribute) out += "&quot;"; else out += character; break;
        case '\'': if (attribute) out += "&apos;"; else out += character; break;
        default: out += character; break;
        }
    }
}

void write_node(std::string& out, const XmlNode& node, std::size_t depth) {
    out.append(depth * 2, ' ');
    out += '<' + node.name;
    for (const auto& attribute : node.attributes) {
        out += ' ' + attribute.first + "=\"";
        write_escaped(out, attribute.second, true);
        out += '"';
    }
    if (node.children.empty() && node.text.empty()) { out += "/>\n"; return; }
    out += '>';
    if (!node.text.empty()) write_escaped(out, node.text, false);
    if (!node.children.empty()) {
        out += '\n';
        for (const XmlNode& child : node.children) write_node(out, child, depth + 1);
        out.append(depth * 2, ' ');
    }
    out += "</" + node.name + ">\n";
}

} // namespace

const std::string& XmlNode::attribute(const std::string& key, const std::string& fallback) const {
    const auto found = attributes.find(key);
    return found == attributes.end() ? fallback : found->second;
}

const XmlNode* XmlNode::child(const std::string& childName) const {
    for (const XmlNode& item : children) if (item.name == childName) return &item;
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::children_named(const std::string& childName) const {
    std::vector<const XmlNode*> result;
    for (const XmlNode& item : children) if (item.name == childName) result.push_back(&item);
    return result;
}

bool XmlDocument::parse(const std::string& source, std::string* error, std::size_t maxDepth) {
    m_root = {};
    if (source.size() > 4 * 1024 * 1024) {
        if (error) *error = "XML document exceeds 4 MiB";
        return false;
    }
    return Parser(source, error, maxDepth).parse(m_root);
}

std::string XmlDocument::serialize() const {
    if (m_root.name.empty()) return {};
    std::string result = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    write_node(result, m_root, 0);
    return result;
}

} // namespace shinkou::uikit

