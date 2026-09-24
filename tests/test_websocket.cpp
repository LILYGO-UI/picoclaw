#include "domain/picoclaw_websocket.hpp"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

void test_url_parser()
{
    PicoClawWebSocketEndpoint endpoint;
    std::string error;
    assert(PicoClawWebSocket::parse_url("ws://example.test:18790/pico/ws", endpoint, error));
    assert(endpoint.host == "example.test" && endpoint.port == 18790 && endpoint.path == "/pico/ws");
    assert(PicoClawWebSocket::parse_url("ws://[::1]", endpoint, error));
    assert(endpoint.host == "::1" && endpoint.port == 80 && endpoint.path == "/");
    assert(!PicoClawWebSocket::parse_url("http://example.test", endpoint, error));
    assert(!PicoClawWebSocket::parse_url("ws://2001:db8::1", endpoint, error));
}

// The test server uses a compact SHA-1 implementation only to validate the
// client's RFC 6455 upgrade; production code has its own implementation.
std::array<std::uint8_t, 20> sha1(std::string input)
{
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8U;
    input.push_back(static_cast<char>(0x80));
    while ((input.size() % 64U) != 56U) input.push_back('\0');
    for (int i = 7; i >= 0; --i) input.push_back(static_cast<char>(bits >> (i * 8)));
    for (std::size_t offset = 0; offset < input.size(); offset += 64U) {
        std::uint32_t w[80]{};
        for (unsigned i = 0; i < 16; ++i) {
            const auto *p = reinterpret_cast<const std::uint8_t *>(input.data() + offset + i * 4U);
            w[i] = (std::uint32_t(p[0]) << 24U) | (std::uint32_t(p[1]) << 16U) | (std::uint32_t(p[2]) << 8U) | p[3];
        }
        for (unsigned i = 16; i < 80; ++i) w[i] = ((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]) << 1U) | ((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]) >> 31U);
        auto rol = [](std::uint32_t value, unsigned n) { return (value << n) | (value >> (32U - n)); };
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; ++i) {
            const std::uint32_t f = i < 20 ? ((b & c) | (~b & d)) : i < 40 ? (b ^ c ^ d) : i < 60 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            const std::uint32_t k = i < 20 ? 0x5A827999U : i < 40 ? 0x6ED9EBA1U : i < 60 ? 0x8F1BBCDCU : 0xCA62C1D6U;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> result{};
    for (unsigned i = 0; i < 5; ++i) for (unsigned j = 0; j < 4; ++j) result[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24U - j * 8U));
    return result;
}

std::string b64(const std::uint8_t *data, std::size_t length)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < length; i += 3) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16U) | (i + 1 < length ? std::uint32_t(data[i + 1]) << 8U : 0U) | (i + 2 < length ? data[i + 2] : 0U);
        result += alphabet[(v >> 18U) & 63U]; result += alphabet[(v >> 12U) & 63U];
        result += i + 1 < length ? alphabet[(v >> 6U) & 63U] : '='; result += i + 2 < length ? alphabet[v & 63U] : '=';
    }
    return result;
}

bool read_until(int fd, std::string &result, std::string_view marker)
{
    std::array<char, 512> buffer{};
    while (result.find(marker) == std::string::npos && result.size() < 8192U) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) return false;
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return result.find(marker) != std::string::npos;
}

void test_loopback_transport()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    int reuse = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        // Some hermetic build sandboxes deny all local sockets. Keep URL and
        // compile coverage in that environment; the same test exercises the
        // full handshake whenever loopback sockets are available.
        if (errno == EPERM || errno == EACCES) {
            ::close(listener);
            return;
        }
        assert(false && "Could not bind loopback test socket");
    }
    assert(::listen(listener, 1) == 0);
    socklen_t address_length = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) == 0);
    const std::uint16_t port = ntohs(address.sin_port);
    bool server_ok = false;
    std::thread server([&]() {
        const int peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0) return;
        std::string request;
        if (!read_until(peer, request, "\r\n\r\n")) { ::close(peer); return; }
        const std::string key_header = "Sec-WebSocket-Key:";
        const std::size_t key_at = request.find(key_header);
        if (key_at == std::string::npos) { ::close(peer); return; }
        const std::size_t key_start = request.find_first_not_of(" \t", key_at + key_header.size());
        const std::size_t key_end = request.find("\r\n", key_start);
        const std::string key = request.substr(key_start, key_end - key_start);
        const std::string accept = b64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").data(), 20);
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        const std::string initial = response + "\x81\x05world";
        if (::send(peer, initial.data(), initial.size(), 0) != static_cast<ssize_t>(initial.size())) { ::close(peer); return; }
        std::array<std::uint8_t, 2> head{};
        if (::recv(peer, head.data(), 2, MSG_WAITALL) != 2 || !(head[1] & 0x80U)) { ::close(peer); return; }
        std::size_t length = head[1] & 0x7FU;
        if (length == 126) { std::array<std::uint8_t, 2> ext{}; if (::recv(peer, ext.data(), 2, MSG_WAITALL) != 2) { ::close(peer); return; } length = (ext[0] << 8U) | ext[1]; }
        std::array<std::uint8_t, 4> mask{};
        if (::recv(peer, mask.data(), 4, MSG_WAITALL) != 4) { ::close(peer); return; }
        std::string message(length, '\0');
        if (length && ::recv(peer, message.data(), length, MSG_WAITALL) != static_cast<ssize_t>(length)) { ::close(peer); return; }
        for (std::size_t i = 0; i < length; ++i) message[i] = static_cast<char>(static_cast<unsigned char>(message[i]) ^ mask[i & 3U]);
        if (message != "hello") { ::close(peer); return; }
        server_ok = true;
        ::close(peer);
    });
    PicoClawWebSocket client;
    std::string error;
    assert(client.connect("ws://127.0.0.1:" + std::to_string(port) + "/pico/ws", error, 2000));
    assert(client.send_text("hello", error));
    std::string payload;
    assert(client.receive_text(payload, error, 2000));
    assert(payload == "world");
    client.close();
    server.join();
    ::close(listener);
    assert(server_ok);
}

}  // namespace

int main()
{
    test_url_parser();
    test_loopback_transport();
    return 0;
}
