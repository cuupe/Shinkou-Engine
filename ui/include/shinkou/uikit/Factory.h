#pragma once

#include "Widgets.h"
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace shinkou::uikit {

class WidgetRegistry {
public:
    using Creator = std::function<std::unique_ptr<Widget>(const XmlNode&)>;

    WidgetRegistry();
    void register_type(std::string type, Creator creator);
    bool unregister_type(const std::string& type);
    bool has_type(const std::string& type) const;
    std::unique_ptr<Widget> create(const XmlNode& node, std::string* error = nullptr) const;
    std::unique_ptr<Widget> create_document(const std::string& source, std::string* error = nullptr) const;
    XmlNode serialize(const Widget& widget) const;

private:
    std::map<std::string, Creator> m_creators;
};

} // namespace shinkou::uikit

