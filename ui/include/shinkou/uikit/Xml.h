#pragma once

#include <map>
#include <string>
#include <vector>

namespace shinkou::uikit {

struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
    std::string text;

    const std::string& attribute(const std::string& key, const std::string& fallback = {}) const;
    const XmlNode* child(const std::string& childName) const;
    std::vector<const XmlNode*> children_named(const std::string& childName) const;
};

class XmlDocument {
public:
    bool parse(const std::string& source, std::string* error = nullptr, std::size_t maxDepth = 64);
    void set_root(XmlNode root) { m_root = std::move(root); }
    const XmlNode* root() const { return m_root.name.empty() ? nullptr : &m_root; }
    XmlNode* root() { return m_root.name.empty() ? nullptr : &m_root; }
    std::string serialize() const;

private:
    XmlNode m_root;
};

} // namespace shinkou::uikit
