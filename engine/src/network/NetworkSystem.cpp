#include "shinkou/network/NetworkSystem.h"

#include "shinkou/log/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
using Socket = SOCKET;
constexpr Socket InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket InvalidSocket = -1;
#endif

namespace shinkou::network {
namespace {

void close_socket(Socket socket) noexcept {
    if (socket == InvalidSocket) return;
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdown_socket(Socket socket) noexcept {
    if (socket == InvalidSocket) return;
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

bool sockets_startup() noexcept {
#if defined(_WIN32)
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
}

std::string socket_error() {
#if defined(_WIN32)
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool send_all(Socket socket, const char* data, std::size_t size, std::atomic<std::uint64_t>& counter) {
    while (size != 0) {
        const auto sent = ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, 1u << 20u)), 0);
        if (sent <= 0) return false;
        counter.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool recv_exact(Socket socket, char* data, std::size_t size, std::atomic<std::uint64_t>& counter) {
    while (size != 0) {
        const auto received = ::recv(socket, data, static_cast<int>(std::min<std::size_t>(size, 1u << 20u)), 0);
        if (received <= 0) return false;
        counter.fetch_add(static_cast<std::uint64_t>(received), std::memory_order_relaxed);
        data += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

Socket make_listener(const std::string& address, std::uint16_t port, int type, std::uint16_t& boundPort, std::string& error) {
    Socket socket = ::socket(AF_INET, type, 0);
    if (socket == InvalidSocket) { error = socket_error(); return InvalidSocket; }
    int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (address.empty() || address == "0.0.0.0") endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        error = "invalid bind address: " + address;
        close_socket(socket);
        return InvalidSocket;
    }
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        error = "bind failed: " + socket_error();
        close_socket(socket);
        return InvalidSocket;
    }
    if (type == SOCK_STREAM && ::listen(socket, 32) != 0) {
        error = "listen failed: " + socket_error();
        close_socket(socket);
        return InvalidSocket;
    }
    sockaddr_in actual{};
#if defined(_WIN32)
    int actualSize = sizeof(actual);
#else
    socklen_t actualSize = sizeof(actual);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &actualSize) == 0) boundPort = ntohs(actual.sin_port);
    return socket;
}

std::string peer_address(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    return ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) != nullptr ? buffer : "unknown";
}

std::string status_text(int status) {
    switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 400: return "Bad Request"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large"; case 426: return "Upgrade Required"; case 500: return "Internal Server Error";
    default: return "Shinkou Response";
    }
}

std::string mime_type(const std::filesystem::path& path) {
    const auto ext = lower(path.extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::string url_decode(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const auto parsed = std::strtol(hex.c_str(), &end, 16);
            if (end != hex.c_str() && *end == '\0') { output.push_back(static_cast<char>(parsed)); i += 2; continue; }
        }
        output.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return output;
}

// Small self-contained SHA-1 implementation used only for the WebSocket handshake.
std::array<std::uint8_t, 20> sha1(std::string_view input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const auto bitLength = static_cast<std::uint64_t>(data.size()) * 8u;
    data.push_back(0x80);
    while ((data.size() % 64u) != 56u) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));
    std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::array<std::uint32_t, 80> words{};
        for (int i = 0; i < 16; ++i) words[i] = (static_cast<std::uint32_t>(data[block + i * 4]) << 24u) |
            (static_cast<std::uint32_t>(data[block + i * 4 + 1]) << 16u) |
            (static_cast<std::uint32_t>(data[block + i * 4 + 2]) << 8u) | data[block + i * 4 + 3];
        for (int i = 16; i < 80; ++i) words[i] = (words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16]);
        for (int i = 16; i < 80; ++i) words[i] = (words[i] << 1u) | (words[i] >> 31u);
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            const auto f = i < 20 ? ((b & c) | ((~b) & d)) : i < 40 ? (b ^ c ^ d) : i < 60 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            const auto k = i < 20 ? 0x5A827999u : i < 40 ? 0x6ED9EBA1u : i < 60 ? 0x8F1BBCDCu : 0xCA62C1D6u;
            const auto rotate = (a << 5u) | (a >> 27u);
            const auto next = rotate + f + e + k + words[i];
            e = d; d = c; c = (b << 30u) | (b >> 2u); b = a; a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<std::uint8_t, 20> output{};
    const std::array<std::uint32_t, 5> values{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < values.size(); ++i) for (int j = 0; j < 4; ++j) output[i * 4 + j] = static_cast<std::uint8_t>(values[i] >> (24 - j * 8));
    return output;
}

std::string base64(const std::uint8_t* data, std::size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[i]) << 16u) |
            (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8u : 0u) |
            (i + 2 < size ? data[i + 2] : 0u);
        output.push_back(alphabet[(value >> 18u) & 63u]); output.push_back(alphabet[(value >> 12u) & 63u]);
        output.push_back(i + 1 < size ? alphabet[(value >> 6u) & 63u] : '='); output.push_back(i + 2 < size ? alphabet[value & 63u] : '=');
    }
    return output;
}

std::string websocket_accept(std::string_view key) {
    const auto digest = sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(digest.data(), digest.size());
}

} // namespace

struct NetworkSystem::Impl {
    struct Route { std::string method; std::string path; HttpHandler handler; };
    NetworkConfig config;
    mutable std::mutex mutex;
    std::vector<Route> routes;
    HttpHandler fallback;
    WebSocketHandler websocketHandler;
    TcpHandler tcpHandler;
    UdpHandler udpHandler;
    std::atomic<bool> stop{false};
    std::atomic<bool> initialized{false};
    std::atomic<bool> httpRunning{false};
    std::atomic<bool> tcpRunning{false};
    std::atomic<bool> udpRunning{false};
    std::atomic<std::uint64_t> nextConnection{1};
    std::atomic<std::uint64_t> nextWebSocket{1};
    std::atomic<std::uint64_t> bytesReceived{0};
    std::atomic<std::uint64_t> bytesSent{0};
    std::atomic<std::size_t> httpConnections{0};
    std::atomic<std::size_t> tcpConnections{0};
    std::uint16_t httpPort{0}, tcpPort{0}, udpPort{0};
    Socket httpListener{InvalidSocket}, tcpListener{InvalidSocket}, udpSocket{InvalidSocket};
    std::mutex socketMutex;
    std::vector<Socket> clients;
    std::unordered_map<std::uint64_t, Socket> tcpClients;
    std::unordered_map<std::uint64_t, Socket> webSockets;
    std::thread httpThread;
    std::thread tcpThread;
    std::thread udpThread;
    std::mutex threadMutex;
    std::vector<std::thread> clientThreads;
    std::string lastError;

    void remove_client(Socket socket) {
        std::lock_guard lock(socketMutex);
        clients.erase(std::remove(clients.begin(), clients.end(), socket), clients.end());
    }
};

namespace {

HttpResponse error_response(int status, std::string body) { return HttpResponse{status, "text/plain; charset=utf-8", {}, std::move(body), false}; }

bool parse_http_request(std::string_view raw, HttpRequest& request) {
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) return false;
    const auto firstEnd = raw.find("\r\n");
    if (firstEnd == std::string_view::npos || firstEnd >= headerEnd) return false;
    std::istringstream line{std::string(raw.substr(0, firstEnd))};
    line >> request.method >> request.target;
    if (request.method.empty() || request.target.empty()) return false;
    const auto queryAt = request.target.find('?');
    request.path = url_decode(request.target.substr(0, queryAt));
    request.query = queryAt == std::string::npos ? std::string{} : request.target.substr(queryAt + 1);
    std::size_t cursor = firstEnd + 2;
    while (cursor < headerEnd) {
        const auto end = raw.find("\r\n", cursor);
        if (end == std::string_view::npos || end > headerEnd) return false;
        const auto colon = raw.find(':', cursor);
        if (colon == std::string_view::npos || colon > end) return false;
        request.headers[lower(std::string(raw.substr(cursor, colon - cursor)))] = trim(std::string(raw.substr(colon + 1, end - colon - 1)));
        cursor = end + 2;
    }
    request.body = std::string(raw.substr(headerEnd + 4));
    return true;
}

bool receive_http(Socket socket, std::size_t maxBytes, std::atomic<std::uint64_t>& counter, std::string& raw) {
    std::array<char, 4096> buffer{};
    raw.clear();
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const auto received = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) return false;
        counter.fetch_add(static_cast<std::uint64_t>(received), std::memory_order_relaxed);
        raw.append(buffer.data(), static_cast<std::size_t>(received));
        if (raw.size() > maxBytes) return false;
    }
    const auto headerEnd = raw.find("\r\n\r\n");
    std::size_t contentLength = 0;
    const auto header = lower(raw.substr(0, headerEnd));
    const auto marker = header.find("content-length:");
    if (marker != std::string::npos) contentLength = static_cast<std::size_t>(std::strtoull(header.c_str() + marker + 15, nullptr, 10));
    while (raw.size() < headerEnd + 4 + contentLength) {
        const auto received = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) return false;
        counter.fetch_add(static_cast<std::uint64_t>(received), std::memory_order_relaxed);
        raw.append(buffer.data(), static_cast<std::size_t>(received));
        if (raw.size() > maxBytes) return false;
    }
    return true;
}

bool send_websocket_frame(Socket socket, std::string_view payload, std::uint8_t opcode, std::atomic<std::uint64_t>& counter) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80u | opcode));
    if (payload.size() < 126) frame.push_back(static_cast<char>(payload.size()));
    else if (payload.size() <= 65535) { frame.push_back(126); frame.push_back(static_cast<char>(payload.size() >> 8u)); frame.push_back(static_cast<char>(payload.size())); }
    else return false;
    frame.append(payload.data(), payload.size());
    return send_all(socket, frame.data(), frame.size(), counter);
}

} // namespace

NetworkSystem::NetworkSystem(NetworkConfig config) : config_(std::move(config)), impl_(std::make_unique<Impl>()) { impl_->config = config_; }
NetworkSystem::~NetworkSystem() { shutdown(); }

bool NetworkSystem::route(std::string method, std::string path, HttpHandler handler) {
    if (method.empty() || path.empty() || !handler) return false;
    std::lock_guard lock(impl_->mutex);
    impl_->routes.push_back({lower(std::move(method)), std::move(path), std::move(handler)});
    return true;
}

bool NetworkSystem::serve_static(std::filesystem::path root, std::string prefix) {
    if (root.empty() || prefix.empty() || prefix.front() != '/') return false;
    config_.staticRoot = std::move(root); config_.staticPrefix = std::move(prefix); impl_->config = config_;
    return true;
}

void NetworkSystem::set_websocket_handler(WebSocketHandler handler) { std::lock_guard lock(impl_->mutex); impl_->websocketHandler = std::move(handler); }
void NetworkSystem::set_tcp_handler(TcpHandler handler) { std::lock_guard lock(impl_->mutex); impl_->tcpHandler = std::move(handler); }
void NetworkSystem::set_udp_handler(UdpHandler handler) { std::lock_guard lock(impl_->mutex); impl_->udpHandler = std::move(handler); }

bool NetworkSystem::initialize() {
    if (impl_->initialized.load()) return true;
    if (!config_.enabled) { impl_->initialized.store(true); return true; }
    if (!sockets_startup()) { impl_->lastError = "socket startup failed"; return false; }
    impl_->stop.store(false);
    auto fail = [this](std::string message) { impl_->lastError = std::move(message); shutdown(); return false; };
    if (config_.enableHttp) {
        impl_->httpListener = make_listener(config_.bindAddress, config_.httpPort, SOCK_STREAM, impl_->httpPort, impl_->lastError);
        if (impl_->httpListener == InvalidSocket) return fail(impl_->lastError);
        impl_->httpRunning.store(true);
        impl_->httpThread = std::thread([this] {
            while (!impl_->stop.load()) {
                sockaddr_in peer{};
#if defined(_WIN32)
                int size = sizeof(peer);
#else
                socklen_t size = sizeof(peer);
#endif
                const auto client = ::accept(impl_->httpListener, reinterpret_cast<sockaddr*>(&peer), &size);
                if (client == InvalidSocket) { if (!impl_->stop.load()) impl_->lastError = socket_error(); break; }
                if (impl_->httpConnections.load() >= config_.maxConnections) { close_socket(client); continue; }
                { std::lock_guard lock(impl_->socketMutex); impl_->clients.push_back(client); }
                impl_->httpConnections.fetch_add(1);
                std::lock_guard threadLock(impl_->threadMutex);
                impl_->clientThreads.emplace_back([this, client, address = peer_address(peer)] {
                    std::string raw;
                    if (receive_http(client, config_.maxRequestBytes, impl_->bytesReceived, raw)) {
                        HttpRequest request;
                        request.remoteAddress = address;
                        if (parse_http_request(raw, request)) {
                            HttpResponse response = error_response(404, "route not found");
                            const auto websocket = lower(request.headers["upgrade"]) == "websocket" && request.path == config_.webSocketPath;
                            if (websocket) {
                                const auto key = request.headers["sec-websocket-key"];
                                if (key.empty()) response = error_response(400, "missing WebSocket key");
                                else {
                                    const auto handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + websocket_accept(key) + "\r\n\r\n";
                                    if (send_all(client, handshake.data(), handshake.size(), impl_->bytesSent)) {
                                        const auto id = impl_->nextWebSocket.fetch_add(1);
                                        { std::lock_guard lock(impl_->socketMutex); impl_->webSockets[id] = client; }
                                        while (!impl_->stop.load()) {
                                            std::array<unsigned char, 2> header{};
                                            if (!recv_exact(client, reinterpret_cast<char*>(header.data()), 2, impl_->bytesReceived)) break;
                                            std::uint64_t length = header[1] & 0x7fu;
                                            if (length == 126) { std::array<unsigned char, 2> extended{}; if (!recv_exact(client, reinterpret_cast<char*>(extended.data()), 2, impl_->bytesReceived)) break; length = (static_cast<std::uint64_t>(extended[0]) << 8u) | extended[1]; }
                                            else if (length == 127) break;
                                            if (length > config_.maxRequestBytes) break;
                                            std::array<unsigned char, 4> mask{};
                                            if ((header[1] & 0x80u) != 0 && !recv_exact(client, reinterpret_cast<char*>(mask.data()), 4, impl_->bytesReceived)) break;
                                            std::string payload(static_cast<std::size_t>(length), '\0');
                                            if (length != 0 && !recv_exact(client, payload.data(), payload.size(), impl_->bytesReceived)) break;
                                            if ((header[1] & 0x80u) != 0) for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
                                            const auto opcode = header[0] & 0x0fu;
                                            if (opcode == 0x8) break;
                                            if (opcode == 0x9) { if (!send_websocket_frame(client, payload, 0xA, impl_->bytesSent)) break; }
                                            else if (opcode == 0x1) { WebSocketHandler callback; { std::lock_guard lock(impl_->mutex); callback = impl_->websocketHandler; } if (callback) callback(id, payload); }
                                        }
                                        { std::lock_guard lock(impl_->socketMutex); impl_->webSockets.erase(id); }
                                        impl_->remove_client(client);
                                        shutdown_socket(client);
                                        close_socket(client);
                                        impl_->httpConnections.fetch_sub(1);
                                        return;
                                    }
                                }
                            } else {
                                HttpHandler handler;
                                { std::lock_guard lock(impl_->mutex); for (const auto& route : impl_->routes) if (route.method == lower(request.method) && route.path == request.path) { handler = route.handler; break; } }
                                if (handler) { try { response = handler(request); } catch (...) { response = error_response(500, "handler failed"); } }
                                else if (!config_.staticRoot.empty() && request.method == "GET" && request.path.rfind(config_.staticPrefix, 0) == 0) {
                                    auto relative = request.path.substr(config_.staticPrefix.size()); if (relative.empty() || relative == "/") relative = "/index.html";
                                    std::error_code pathError;
                                    const auto root = std::filesystem::weakly_canonical(config_.staticRoot, pathError);
                                    const auto file = pathError ? std::filesystem::path{} : std::filesystem::weakly_canonical(root / url_decode(relative.substr(relative.front() == '/' ? 1 : 0)), pathError);
                                    const auto rootText = root.generic_string(); const auto fileText = file.generic_string();
                                    const bool insideRoot = !pathError && (fileText == rootText || fileText.rfind(rootText + '/', 0) == 0);
                                    if (insideRoot && std::filesystem::is_regular_file(file, pathError)) { std::ifstream input(file, std::ios::binary); response.body.assign(std::istreambuf_iterator<char>(input), {}); response.status = 200; response.contentType = mime_type(file); }
                                }
                            }
                            std::ostringstream output; output << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\nContent-Length: " << response.body.size() << "\r\nContent-Type: " << response.contentType << "\r\nConnection: close\r\nServer: ShinkouEngine\r\n";
                            if (config_.enableCors) output << "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type\r\n";
                            for (const auto& [name, value] : response.headers) output << name << ": " << value << "\r\n";
                            output << "\r\n" << response.body;
                            const auto text = output.str(); send_all(client, text.data(), text.size(), impl_->bytesSent);
                        }
                    }
                    impl_->remove_client(client); shutdown_socket(client); close_socket(client); impl_->httpConnections.fetch_sub(1);
                });
            }
        });
    }
    if (config_.enableTcp) {
        impl_->tcpListener = make_listener(config_.bindAddress, config_.tcpPort, SOCK_STREAM, impl_->tcpPort, impl_->lastError);
        if (impl_->tcpListener == InvalidSocket) return fail(impl_->lastError);
        impl_->tcpRunning.store(true);
        impl_->tcpThread = std::thread([this] {
            while (!impl_->stop.load()) {
                sockaddr_in peer{};
#if defined(_WIN32)
                int size = sizeof(peer);
#else
                socklen_t size = sizeof(peer);
#endif
                const auto client = ::accept(impl_->tcpListener, reinterpret_cast<sockaddr*>(&peer), &size);
                if (client == InvalidSocket) { if (!impl_->stop.load()) impl_->lastError = socket_error(); break; }
                const auto id = impl_->nextConnection.fetch_add(1); { std::lock_guard lock(impl_->socketMutex); impl_->tcpClients[id] = client; } impl_->tcpConnections.fetch_add(1);
                std::lock_guard threadLock(impl_->threadMutex);
                impl_->clientThreads.emplace_back([this, client, id, address = peer_address(peer)] {
                    std::array<char, 4096> buffer{};
                    while (!impl_->stop.load()) { const auto received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0); if (received <= 0) break; impl_->bytesReceived.fetch_add(static_cast<std::uint64_t>(received)); TcpHandler callback; { std::lock_guard lock(impl_->mutex); callback = impl_->tcpHandler; } if (callback) callback(id, std::string_view(buffer.data(), static_cast<std::size_t>(received)), address); }
                    { std::lock_guard lock(impl_->socketMutex); impl_->tcpClients.erase(id); } shutdown_socket(client); close_socket(client); impl_->tcpConnections.fetch_sub(1);
                });
            }
        });
    }
    if (config_.enableUdp) {
        impl_->udpSocket = make_listener(config_.bindAddress, config_.udpPort, SOCK_DGRAM, impl_->udpPort, impl_->lastError);
        if (impl_->udpSocket == InvalidSocket) return fail(impl_->lastError);
        impl_->udpRunning.store(true);
        impl_->udpThread = std::thread([this] {
            std::array<char, 65536> buffer{};
            while (!impl_->stop.load()) { sockaddr_in peer{}; socklen_t size = sizeof(peer); const auto received = ::recvfrom(impl_->udpSocket, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&peer), &size); if (received <= 0) { if (!impl_->stop.load()) impl_->lastError = socket_error(); break; } impl_->bytesReceived.fetch_add(static_cast<std::uint64_t>(received)); UdpHandler callback; { std::lock_guard lock(impl_->mutex); callback = impl_->udpHandler; } if (callback) callback(peer_address(peer), std::string_view(buffer.data(), static_cast<std::size_t>(received))); }
        });
    }
    impl_->initialized.store(true);
    SHINKOU_LOG_INFO("network initialized; http={}, tcp={}, udp={}", impl_->httpPort, impl_->tcpPort, impl_->udpPort);
    return true;
}

void NetworkSystem::poll() {}

void NetworkSystem::shutdown() noexcept {
    if (!impl_ || (!impl_->initialized.load() && !impl_->httpThread.joinable() && !impl_->tcpThread.joinable() && !impl_->udpThread.joinable())) return;
    impl_->stop.store(true);
    shutdown_socket(impl_->httpListener); shutdown_socket(impl_->tcpListener); shutdown_socket(impl_->udpSocket);
    close_socket(impl_->httpListener); close_socket(impl_->tcpListener); close_socket(impl_->udpSocket);
    { std::lock_guard lock(impl_->socketMutex); for (const auto socket : impl_->clients) shutdown_socket(socket); for (const auto& [id, socket] : impl_->tcpClients) { (void)id; shutdown_socket(socket); } for (const auto& [id, socket] : impl_->webSockets) { (void)id; shutdown_socket(socket); } }
    if (impl_->httpThread.joinable() && impl_->httpThread.get_id() != std::this_thread::get_id()) impl_->httpThread.join();
    if (impl_->tcpThread.joinable() && impl_->tcpThread.get_id() != std::this_thread::get_id()) impl_->tcpThread.join();
    if (impl_->udpThread.joinable() && impl_->udpThread.get_id() != std::this_thread::get_id()) impl_->udpThread.join();
    { std::lock_guard threadLock(impl_->threadMutex); for (auto& thread : impl_->clientThreads) if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join(); impl_->clientThreads.clear(); }
    impl_->clients.clear(); impl_->tcpClients.clear(); impl_->webSockets.clear();
    impl_->httpListener = impl_->tcpListener = impl_->udpSocket = InvalidSocket;
    impl_->httpRunning.store(false); impl_->tcpRunning.store(false); impl_->udpRunning.store(false); impl_->initialized.store(false);
}

bool NetworkSystem::broadcast_websocket(std::string_view text) {
    bool result = true; std::lock_guard lock(impl_->socketMutex); for (const auto& [id, socket] : impl_->webSockets) { (void)id; if (!send_websocket_frame(socket, text, 0x1, impl_->bytesSent)) result = false; } return result;
}

bool NetworkSystem::send_tcp(std::uint64_t id, std::string_view bytes) {
    std::lock_guard lock(impl_->socketMutex); const auto found = impl_->tcpClients.find(id); return found != impl_->tcpClients.end() && send_all(found->second, bytes.data(), bytes.size(), impl_->bytesSent);
}

bool NetworkSystem::send_udp(std::string_view host, std::uint16_t port, std::string_view bytes) {
    if (impl_->udpSocket == InvalidSocket) return false;
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(port); if (::inet_pton(AF_INET, std::string(host).c_str(), &endpoint.sin_addr) != 1) return false;
    const auto sent = ::sendto(impl_->udpSocket, bytes.data(), static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)); if (sent < 0) return false; impl_->bytesSent.fetch_add(static_cast<std::uint64_t>(sent)); return true;
}

bool NetworkSystem::initialized() const noexcept { return impl_->initialized.load(); }
NetworkStats NetworkSystem::stats() const {
    NetworkStats result;
    result.initialized = impl_->initialized.load(); result.httpRunning = impl_->httpRunning.load(); result.tcpRunning = impl_->tcpRunning.load(); result.udpRunning = impl_->udpRunning.load();
    result.httpPort = impl_->httpPort; result.tcpPort = impl_->tcpPort; result.udpPort = impl_->udpPort; result.httpConnections = impl_->httpConnections.load(); result.tcpConnections = impl_->tcpConnections.load();
    { std::lock_guard lock(impl_->socketMutex); result.webSocketConnections = impl_->webSockets.size(); }
    result.bytesReceived = impl_->bytesReceived.load(); result.bytesSent = impl_->bytesSent.load(); result.lastError = impl_->lastError;
    return result;
}

} // namespace shinkou::network
