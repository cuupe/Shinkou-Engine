#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::network {

using Headers = std::unordered_map<std::string, std::string>;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    Headers headers;
    std::string body;
    std::string remoteAddress;
};

struct HttpResponse {
    int status{200};
    std::string contentType{"text/plain; charset=utf-8"};
    Headers headers;
    std::string body;
    bool keepAlive{false};
};

struct NetworkConfig {
    bool enabled{true};
    bool enableHttp{false};
    bool enableTcp{false};
    bool enableUdp{false};
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t httpPort{0};
    std::uint16_t tcpPort{0};
    std::uint16_t udpPort{0};
    std::size_t maxConnections{64};
    std::size_t maxRequestBytes{1024u * 1024u};
    std::filesystem::path staticRoot{};
    std::string staticPrefix{"/"};
    std::string webSocketPath{"/ws"};
    bool enableCors{false};
};

struct NetworkStats {
    bool initialized{false};
    bool httpRunning{false};
    bool tcpRunning{false};
    bool udpRunning{false};
    std::uint16_t httpPort{0};
    std::uint16_t tcpPort{0};
    std::uint16_t udpPort{0};
    std::size_t httpConnections{0};
    std::size_t webSocketConnections{0};
    std::size_t tcpConnections{0};
    std::uint64_t bytesReceived{0};
    std::uint64_t bytesSent{0};
    std::string lastError;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;
using WebSocketHandler = std::function<void(std::uint64_t, std::string_view)>;
using TcpHandler = std::function<void(std::uint64_t, std::string_view, std::string_view)>;
using UdpHandler = std::function<void(std::string_view, std::string_view)>;

class NetworkSystem final {
public:
    explicit NetworkSystem(NetworkConfig config = {});
    ~NetworkSystem();
    NetworkSystem(const NetworkSystem&) = delete;
    NetworkSystem& operator=(const NetworkSystem&) = delete;

    bool initialize();
    void poll();
    void shutdown() noexcept;

    bool route(std::string method, std::string path, HttpHandler handler);
    bool serve_static(std::filesystem::path root, std::string prefix = "/");
    void set_websocket_handler(WebSocketHandler handler);
    void set_tcp_handler(TcpHandler handler);
    void set_udp_handler(UdpHandler handler);
    bool broadcast_websocket(std::string_view text);
    bool send_tcp(std::uint64_t connectionId, std::string_view bytes);
    bool send_udp(std::string_view host, std::uint16_t port, std::string_view bytes);

    bool initialized() const noexcept;
    NetworkStats stats() const;
    const NetworkConfig& config() const noexcept { return config_; }

private:
    struct Impl;
    NetworkConfig config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace shinkou::network
