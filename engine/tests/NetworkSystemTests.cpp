#include "shinkou/network/NetworkSystem.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
void close_socket(int socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}
}

int main() {
    shinkou::network::NetworkConfig config;
    config.enableHttp = true;
    config.httpPort = 0;
    config.maxRequestBytes = 64 * 1024;
    shinkou::network::NetworkSystem network(config);
    if (!network.route("GET", "/health", [](const shinkou::network::HttpRequest&) {
        return shinkou::network::HttpResponse{200, "application/json; charset=utf-8", {}, "{\"ok\":true}", false};
    })) return 1;
    if (!network.initialize()) { std::cerr << network.stats().lastError << '\n'; return 2; }
    const auto stats = network.stats();
    if (!stats.initialized || !stats.httpRunning || stats.httpPort == 0) return 3;
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) return 6;
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(stats.httpPort);
    ::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) { close_socket(socket); return 7; }
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (::send(socket, request.data(), static_cast<int>(request.size()), 0) != static_cast<int>(request.size())) { close_socket(socket); return 8; }
    std::string response;
    char buffer[1024]{};
    int received = 0;
    while ((received = ::recv(socket, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, static_cast<std::size_t>(received));
    close_socket(socket);
    if (response.find("200 OK") == std::string::npos || response.find("{\"ok\":true}") == std::string::npos) return 9;
    network.shutdown();
    if (network.initialized()) return 4;

    shinkou::network::NetworkConfig disabledConfig;
    disabledConfig.enabled = false;
    shinkou::network::NetworkSystem disabled(disabledConfig);
    if (!disabled.initialize() || !disabled.initialized()) return 5;
    disabled.shutdown();
    return 0;
}
