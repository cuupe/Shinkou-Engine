#include "shinkou/uikit/Xml.h"
#include <cassert>

using namespace shinkou::uikit;

int main() {
    XmlDocument document;
    std::string error;
    assert(document.parse("<?xml version=\"1.0\"?><root label=\"A &amp; B\"><!-- c --><item>hello &lt;ui&gt;</item></root>", &error));
    assert(document.root() && document.root()->name == "root");
    assert(document.root()->attribute("label") == "A & B");
    assert(document.root()->child("item")->text == "hello <ui>");
    const std::string serialized = document.serialize();
    assert(serialized.find("A &amp; B") != std::string::npos);
    assert(!document.parse("<a><b></a>", &error));
    return 0;
}

