#ifndef LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_WEBSOCKET_HPP
#define LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_WEBSOCKET_HPP

#include <cstdint>
#include <string>
#include <string_view>

// Small POSIX WebSocket client used by the CM0 gateway connection.  It only
// depends on libc (getaddrinfo/socket/poll) so the UI can use it on the target
// image without pulling in a TLS or WebSocket framework.
struct PicoClawWebSocketEndpoint {
    std::string host;
    std::uint16_t port = 80;
    std::string path = "/pico/ws";
};

class PicoClawWebSocket {
public:
    PicoClawWebSocket() = default;
    ~PicoClawWebSocket();

    PicoClawWebSocket(const PicoClawWebSocket &) = delete;
    PicoClawWebSocket &operator=(const PicoClawWebSocket &) = delete;

    // Parses ws://host[:port][/path].  The transport deliberately accepts
    // ws:// only; TLS belongs to the platform networking layer.
    static bool parse_url(std::string_view url, PicoClawWebSocketEndpoint &endpoint,
                          std::string &error);

    // Performs TCP connect and RFC 6455 HTTP upgrade. timeout_ms applies to
    // both connect and each handshake read. A negative value means blocking.
    bool connect(std::string_view url, std::string &error, int timeout_ms = 5000,
                 std::string_view bearer_token = {});
    // Convenience ordering used by PicoClawClient: URL, token, error, timeout.
    bool connect(std::string_view url, std::string_view bearer_token, std::string &error,
                 int timeout_ms = 5000);
    bool connect(const PicoClawWebSocketEndpoint &endpoint, std::string &error, int timeout_ms = 5000,
                 std::string_view bearer_token = {});

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int native_handle() const noexcept { return fd_; }

    bool send_text(std::string_view payload, std::string &error);
    // Reads one complete text message. Ping frames are answered automatically;
    // close frames are acknowledged and make this return false.
    bool receive_text(std::string &payload, std::string &error, int timeout_ms = -1);
    void close() noexcept;

private:
    bool wait_readable(int timeout_ms, std::string &error) const;
    bool read_exact(void *buffer, std::size_t length, int timeout_ms, std::string &error);
    bool write_all(const void *buffer, std::size_t length, std::string &error);
    bool receive_frame(std::uint8_t &opcode, bool &fin, std::string &payload,
                       std::string &error, int timeout_ms);
    bool send_frame(std::uint8_t opcode, std::string_view payload, std::string &error);

    int fd_ = -1;
    // Bytes read together with the HTTP response (a peer may send the first
    // WebSocket frame in the same TCP packet as the upgrade response).
    std::string pending_;
};

#endif
