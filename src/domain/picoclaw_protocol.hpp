#ifndef LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_PROTOCOL_HPP
#define LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <string_view>

// Wire-level events used by picoclaw_fui's Pico Protocol WebSocket endpoint.
// Keeping this type independent from any socket library lets the LVGL target
// use whichever transport is available on the device.
enum class PicoClawProtocolEventType {
    Unknown,
    MessageCreate,
    MessageUpdate,
    TypingStart,
    TypingStop,
    Error,
    Pong,
};

struct PicoClawProtocolEvent {
    PicoClawProtocolEventType type = PicoClawProtocolEventType::Unknown;
    std::string id;
    std::string session_id;
    std::string content;
    std::string error;
};

class PicoClawProtocol {
public:
    static constexpr std::uint16_t gateway_port    = 18790;
    static constexpr std::string_view gateway_path = "/pico/ws";

    explicit PicoClawProtocol(std::string session_id = {});

    [[nodiscard]] const std::string &session_id() const noexcept;
    // Starts a new conversation. An empty id creates a fresh local id.
    void reset_session(std::string session_id = {});

    // Serializes the exact message.send envelope expected by /pico/ws.
    // request_id is optional; when omitted a unique local id is generated.
    [[nodiscard]] std::string make_message_send(std::string_view content, std::string_view request_id = {});

    // Parses one complete text WebSocket frame. No third-party JSON library is
    // required; the parser accepts the object/string subset used by Pico.
    static bool parse_event(std::string_view frame, PicoClawProtocolEvent &event, std::string &error);
    [[nodiscard]] static const char *event_type_name(PicoClawProtocolEventType type) noexcept;

private:
    static std::string new_id();
    std::string session_id_;
    std::uint64_t sequence_ = 0;
};

#endif
