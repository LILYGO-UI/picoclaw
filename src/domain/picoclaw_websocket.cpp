#include "domain/picoclaw_websocket.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t maximum_frame = 16U * 1024U * 1024U;

std::string socket_error(const char *prefix)
{
    return std::string(prefix) + ": " + std::strerror(errno);
}

// Tiny SHA-1 implementation for Sec-WebSocket-Accept.  The handshake is the
// only place a digest is needed and keeping it here avoids an OpenSSL runtime
// dependency on the CM0 image.
struct Sha1 {
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    std::uint64_t bits = 0;
    std::array<std::uint8_t, 64> block{};
    std::size_t used = 0;

    static std::uint32_t rol(std::uint32_t value, unsigned amount)
    {
        return (value << amount) | (value >> (32U - amount));
    }
    void transform(const std::uint8_t *data)
    {
        std::uint32_t w[80]{};
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(data[i * 4]) << 24U) | (std::uint32_t(data[i * 4 + 1]) << 16U) |
                   (std::uint32_t(data[i * 4 + 2]) << 8U) | std::uint32_t(data[i * 4 + 3]);
        for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    void update(std::string_view input)
    {
        bits += static_cast<std::uint64_t>(input.size()) * 8U;
        for (const unsigned char byte : input) {
            block[used++] = byte;
            if (used == block.size()) {
                transform(block.data());
                used = 0;
            }
        }
    }
    std::array<std::uint8_t, 20> finish()
    {
        block[used++] = 0x80U;
        if (used > 56U) {
            while (used < 64U) block[used++] = 0;
            transform(block.data());
            used = 0;
        }
        while (used < 56U) block[used++] = 0;
        for (unsigned i = 0; i < 8; ++i) block[used++] = static_cast<std::uint8_t>(bits >> (56U - i * 8U));
        transform(block.data());
        std::array<std::uint8_t, 20> digest{};
        for (unsigned i = 0; i < 5; ++i)
            for (unsigned j = 0; j < 4; ++j) digest[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24U - j * 8U));
        return digest;
    }
};

std::string base64(const std::uint8_t *data, std::size_t length)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((length + 2U) / 3U * 4U);
    for (std::size_t i = 0; i < length; i += 3U) {
        const std::uint32_t value = (std::uint32_t(data[i]) << 16U) |
                                    (i + 1U < length ? std::uint32_t(data[i + 1U]) << 8U : 0U) |
                                    (i + 2U < length ? data[i + 2U] : 0U);
        result.push_back(alphabet[(value >> 18U) & 63U]);
        result.push_back(alphabet[(value >> 12U) & 63U]);
        result.push_back(i + 1U < length ? alphabet[(value >> 6U) & 63U] : '=');
        result.push_back(i + 2U < length ? alphabet[value & 63U] : '=');
    }
    return result;
}

std::string random_key()
{
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto &byte : bytes) byte = static_cast<std::uint8_t>(random());
    return base64(bytes.data(), bytes.size());
}

std::string lower(std::string value)
{
    for (char &ch : value)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

bool header_value(std::string_view headers, std::string_view wanted, std::string &value)
{
    std::size_t at = 0;
    while (at < headers.size()) {
        const std::size_t end = headers.find("\r\n", at);
        const std::string_view line = headers.substr(at, end == std::string_view::npos ? headers.size() - at : end - at);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && lower(std::string(line.substr(0, colon))) == lower(std::string(wanted))) {
            std::size_t first = colon + 1U;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
            std::size_t last = line.size();
            while (last > first && (line[last - 1U] == ' ' || line[last - 1U] == '\t')) --last;
            value = std::string(line.substr(first, last - first));
            return true;
        }
        if (end == std::string_view::npos) break;
        at = end + 2U;
    }
    return false;
}

}  // namespace

PicoClawWebSocket::~PicoClawWebSocket() { close(); }

bool PicoClawWebSocket::parse_url(std::string_view url, PicoClawWebSocketEndpoint &endpoint, std::string &error)
{
    if (url.substr(0, 5) != "ws://") {
        error = "WebSocket URL must start with ws://";
        return false;
    }
    url.remove_prefix(5);
    const std::size_t slash = url.find('/');
    const std::string_view authority = url.substr(0, slash == std::string_view::npos ? url.size() : slash);
    if (authority.empty()) {
        error = "WebSocket URL has no host";
        return false;
    }
    endpoint.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    if (endpoint.path.empty() || endpoint.path.front() != '/') endpoint.path.insert(endpoint.path.begin(), '/');
    if (endpoint.path.find('\r') != std::string::npos || endpoint.path.find('\n') != std::string::npos) {
        error = "Invalid WebSocket path";
        return false;
    }
    std::string_view host = authority;
    std::uint32_t port = 80;
    if (authority.front() == '[') {
        const std::size_t closing = authority.find(']');
        if (closing == std::string_view::npos) {
            error = "Invalid IPv6 WebSocket host";
            return false;
        }
        host = authority.substr(1, closing - 1U);
        if (closing + 1U < authority.size()) {
            if (authority[closing + 1U] != ':' || authority.size() == closing + 2U) {
                error = "Invalid WebSocket port";
                return false;
            }
            try { port = std::stoul(std::string(authority.substr(closing + 2U))); }
            catch (...) { error = "Invalid WebSocket port"; return false; }
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos && authority.find(':') == colon) {
            host = authority.substr(0, colon);
            if (host.empty() || colon + 1U == authority.size()) {
                error = "Invalid WebSocket host or port";
                return false;
            }
            try { port = std::stoul(std::string(authority.substr(colon + 1U))); }
            catch (...) { error = "Invalid WebSocket port"; return false; }
        } else if (authority.find(':') != std::string_view::npos) {
            error = "IPv6 WebSocket hosts must use brackets";
            return false;
        }
    }
    if (port == 0 || port > 65535U || host.empty()) {
        error = "Invalid WebSocket host or port";
        return false;
    }
    endpoint.host = std::string(host);
    endpoint.port = static_cast<std::uint16_t>(port);
    return true;
}

bool PicoClawWebSocket::connect(std::string_view url, std::string &error, int timeout_ms,
                                std::string_view bearer_token)
{
    PicoClawWebSocketEndpoint endpoint;
    if (!parse_url(url, endpoint, error)) return false;
    return connect(endpoint, error, timeout_ms, bearer_token);
}

bool PicoClawWebSocket::connect(std::string_view url, std::string_view bearer_token, std::string &error,
                                int timeout_ms)
{
    return connect(url, error, timeout_ms, bearer_token);
}

bool PicoClawWebSocket::connect(const PicoClawWebSocketEndpoint &endpoint, std::string &error, int timeout_ms,
                                std::string_view bearer_token)
{
    close();
    struct addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    const std::string service = std::to_string(endpoint.port);
    struct addrinfo *addresses = nullptr;
    const int lookup = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        error = std::string("Could not resolve WebSocket host: ") + gai_strerror(lookup);
        return false;
    }
    for (auto *address = addresses; address != nullptr; address = address->ai_next) {
        fd_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd_ < 0) continue;
        if (timeout_ms >= 0) {
            const int flags = ::fcntl(fd_, F_GETFL, 0);
            (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
            const int result = ::connect(fd_, address->ai_addr, address->ai_addrlen);
            if (result < 0 && errno != EINPROGRESS) { close(); continue; }
            if (result < 0) {
                struct pollfd descriptor{fd_, POLLOUT, 0};
                if (::poll(&descriptor, 1, timeout_ms) <= 0 || !(descriptor.revents & POLLOUT)) { close(); continue; }
                int connect_error = 0;
                socklen_t length = sizeof(connect_error);
                if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &connect_error, &length) != 0 || connect_error != 0) { close(); continue; }
            }
            (void)::fcntl(fd_, F_SETFL, flags);
        } else if (::connect(fd_, address->ai_addr, address->ai_addrlen) != 0) {
            close();
            continue;
        }
        break;
    }
    ::freeaddrinfo(addresses);
    if (fd_ < 0) {
        error = socket_error("Could not connect WebSocket");
        return false;
    }

    const std::string key = random_key();
    const bool ipv6_host = endpoint.host.find(':') != std::string::npos;
    const std::string host_header = (ipv6_host ? "[" : "") + endpoint.host + (ipv6_host ? "]" : "") +
                                    ":" + std::to_string(endpoint.port);
    std::string request = "GET " + (endpoint.path.empty() ? "/" : endpoint.path) + " HTTP/1.1\r\nHost: " +
                          host_header +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n";
    if (!bearer_token.empty()) request += "Authorization: Bearer " + std::string(bearer_token) + "\r\n";
    request += "\r\n";
    if (!write_all(request.data(), request.size(), error)) { close(); return false; }
    std::string response;
    std::array<char, 1024> buffer{};
    while (response.find("\r\n\r\n") == std::string::npos && response.size() < 16U * 1024U) {
        if (!wait_readable(timeout_ms, error)) { close(); return false; }
        const ssize_t count = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (count <= 0) { error = count == 0 ? "WebSocket peer closed during handshake" : socket_error("WebSocket handshake read failed"); close(); return false; }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) { error = "Incomplete WebSocket handshake response"; close(); return false; }
    const std::size_t first_line_end = response.find("\r\n");
    if (first_line_end == std::string::npos || response.substr(0, first_line_end).find(" 101 ") == std::string::npos) {
        error = "WebSocket server did not return HTTP 101"; close(); return false;
    }
    std::string accept;
    if (!header_value(std::string_view(response).substr(first_line_end + 2U, header_end - first_line_end - 2U), "Sec-WebSocket-Accept", accept)) {
        error = "WebSocket handshake omitted Sec-WebSocket-Accept"; close(); return false;
    }
    Sha1 sha;
    sha.update(key + std::string(websocket_guid));
    if (accept != base64(sha.finish().data(), 20U)) { error = "Invalid WebSocket Sec-WebSocket-Accept"; close(); return false; }
    pending_ = response.substr(header_end + 4U);
    return true;
}

bool PicoClawWebSocket::is_open() const noexcept { return fd_ >= 0; }

bool PicoClawWebSocket::wait_readable(int timeout_ms, std::string &error) const
{
    struct pollfd descriptor{fd_, POLLIN, 0};
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result == 0) { error = "WebSocket read timed out"; return false; }
    if (result < 0) { if (errno == EINTR) return wait_readable(timeout_ms, error); error = socket_error("WebSocket poll failed"); return false; }
    // POLLIN may be reported together with POLLHUP when a peer half-closes;
    // consume the buffered frame first so the final assistant message is not
    // discarded.
    if (descriptor.revents & POLLIN) return true;
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) { error = "WebSocket connection closed"; return false; }
    return true;
}

bool PicoClawWebSocket::read_exact(void *buffer, std::size_t length, int timeout_ms, std::string &error)
{
    auto *output = static_cast<std::uint8_t *>(buffer);
    while (length != 0U) {
        if (!pending_.empty()) {
            const std::size_t count = pending_.size() < length ? pending_.size() : length;
            std::memcpy(output, pending_.data(), count);
            pending_.erase(0, count);
            output += count;
            length -= count;
            continue;
        }
        if (!wait_readable(timeout_ms, error)) return false;
        const ssize_t count = ::recv(fd_, output, length, 0);
        if (count == 0) { error = "WebSocket peer closed"; return false; }
        if (count < 0) { if (errno == EINTR) continue; error = socket_error("WebSocket read failed"); return false; }
        output += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

bool PicoClawWebSocket::write_all(const void *buffer, std::size_t length, std::string &error)
{
    const auto *input = static_cast<const std::uint8_t *>(buffer);
    while (length != 0U) {
        const ssize_t count = ::send(fd_, input, length, MSG_NOSIGNAL);
        if (count < 0) { if (errno == EINTR) continue; error = socket_error("WebSocket write failed"); return false; }
        input += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

bool PicoClawWebSocket::send_frame(std::uint8_t opcode, std::string_view payload, std::string &error)
{
    if (!is_open() || payload.size() > maximum_frame || ((opcode & 0x08U) != 0U && payload.size() > 125U)) {
        error = !is_open() ? "WebSocket is not connected" : "WebSocket payload is too large";
        return false;
    }
    std::array<std::uint8_t, 14> header{};
    std::size_t header_size = 2U;
    header[0] = static_cast<std::uint8_t>(0x80U | (opcode & 0x0FU));
    if (payload.size() < 126U) header[1] = static_cast<std::uint8_t>(0x80U | payload.size());
    else if (payload.size() <= 65535U) { header[1] = 0xFEU; header[2] = static_cast<std::uint8_t>(payload.size() >> 8U); header[3] = static_cast<std::uint8_t>(payload.size()); header_size = 4U; }
    else { header[1] = 0xFFU; for (unsigned i = 0; i < 8; ++i) header[2 + i] = static_cast<std::uint8_t>(payload.size() >> (56U - i * 8U)); header_size = 10U; }
    std::array<std::uint8_t, 4> mask{};
    std::random_device random;
    for (auto &byte : mask) byte = static_cast<std::uint8_t>(random());
    std::memcpy(header.data() + header_size, mask.data(), mask.size());
    header_size += mask.size();
    if (!write_all(header.data(), header_size, error)) return false;
    std::string masked(payload);
    for (std::size_t i = 0; i < masked.size(); ++i) masked[i] = static_cast<char>(static_cast<unsigned char>(masked[i]) ^ mask[i & 3U]);
    return masked.empty() || write_all(masked.data(), masked.size(), error);
}

bool PicoClawWebSocket::send_text(std::string_view payload, std::string &error) { return send_frame(0x1U, payload, error); }

bool PicoClawWebSocket::receive_frame(std::uint8_t &opcode, bool &fin, std::string &payload, std::string &error, int timeout_ms)
{
    std::array<std::uint8_t, 2> first{};
    if (!read_exact(first.data(), first.size(), timeout_ms, error)) return false;
    fin = (first[0] & 0x80U) != 0;
    opcode = first[0] & 0x0FU;
    const bool masked = (first[1] & 0x80U) != 0;
    std::uint64_t length = first[1] & 0x7FU;
    if (length == 126U) { std::array<std::uint8_t, 2> bytes{}; if (!read_exact(bytes.data(), 2, timeout_ms, error)) return false; length = (std::uint64_t(bytes[0]) << 8U) | bytes[1]; }
    else if (length == 127U) { std::array<std::uint8_t, 8> bytes{}; if (!read_exact(bytes.data(), 8, timeout_ms, error)) return false; length = 0; for (auto byte : bytes) length = (length << 8U) | byte; if (length > maximum_frame) { error = "WebSocket frame is too large"; return false; } }
    if (length > maximum_frame) { error = "WebSocket frame is too large"; return false; }
    if ((opcode & 0x08U) != 0U && (!fin || length > 125U)) { error = "Invalid WebSocket control frame"; return false; }
    std::array<std::uint8_t, 4> mask{};
    if (masked && !read_exact(mask.data(), mask.size(), timeout_ms, error)) return false;
    payload.resize(static_cast<std::size_t>(length));
    if (length != 0U && !read_exact(payload.data(), payload.size(), timeout_ms, error)) return false;
    if (masked) for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3U]);
    return true;
}

bool PicoClawWebSocket::receive_text(std::string &payload, std::string &error, int timeout_ms)
{
    payload.clear();
    bool started = false;
    for (;;) {
        std::uint8_t opcode = 0;
        bool fin = false;
        std::string frame;
        if (!receive_frame(opcode, fin, frame, error, timeout_ms)) return false;
        if (opcode == 0x9U) { if (!send_frame(0xAU, frame, error)) return false; continue; }
        if (opcode == 0xAU) continue;
        if (opcode == 0x8U) { (void)send_frame(0x8U, frame.substr(0, 125U), error); close(); error = "WebSocket peer closed"; return false; }
        if (opcode == 0x1U) { if (started) { error = "Unexpected WebSocket text frame"; return false; } started = true; }
        else if (opcode == 0x0U) { if (!started) { error = "Unexpected WebSocket continuation frame"; return false; } }
        else { error = "Unsupported WebSocket opcode"; return false; }
        payload += frame;
        if (payload.size() > maximum_frame) { error = "WebSocket message is too large"; return false; }
        if (fin) return true;
    }
}

void PicoClawWebSocket::close() noexcept
{
    if (fd_ >= 0) {
        (void)::shutdown(fd_, SHUT_RDWR);
        (void)::close(fd_);
        fd_ = -1;
    }
    pending_.clear();
}
