#include "shinkou/platform/Window.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    using shinkou::platform::Window;
    Window window;
    std::string received;
    std::int32_t receivedX = 0;
    std::int32_t receivedY = 0;
    std::size_t calls = 0;
    window.set_file_drop_handler([&](std::string path, std::int32_t x, std::int32_t y) {
        received = std::move(path);
        receivedX = x;
        receivedY = y;
        ++calls;
    });

    window.dispatch_file_drop("C:/Project/assets/preview.obj", 123, 456);
    assert(calls == 1);
    assert(received == "C:/Project/assets/preview.obj");
    assert(receivedX == 123 && receivedY == 456);

    // Destroy clears the adapter so a late native message cannot call an
    // editor object after the window/lifecycle has been torn down.
    window.destroy();
    window.dispatch_file_drop("C:/Project/assets/late.obj", 1, 2);
    assert(calls == 1);

    std::cout << "Window file-drop adapter dispatch and teardown passed\n";
    return 0;
}
