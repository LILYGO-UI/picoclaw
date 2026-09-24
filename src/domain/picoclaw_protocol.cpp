#include "domain/picoclaw_protocol.hpp"

#include <atomic>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <unistd.h>

namespace {

std::string trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U]))) --last;
    return std::string(value.substr(first, last - first));
}

void skip_space(std::string_view text, std::size_t &at)
{
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
}

bool decode_string(std::string_view text, std::size_t at, std::string &value, std::size_t *end = nullptr)
{
    if (at >= text.size() || text[at] != '"') return false;
    ++at;
    value.clear();
    while (at < text.size()) {
        const char ch = text[at++];
        if (ch == '"') {
            if (end) *end = at;
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (at >= text.size()) return false;
        switch (text[at++]) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            // Pico messages are UTF-8 and normally do not escape unicode. A
            // unicode escape is retained as UTF-8-compatible source text when
            // encountered, rather than rejecting the whole gateway frame.
            case 'u':
                if (at + 4U > text.size()) return false;
                value += "\\u";
                value.append(text.substr(at, 4U));
                at += 4U;
                break;
            default:
                return false;
        }
    }
    return false;
}

bool find_string(std::string_view object, std::string_view key, std::string &value)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t at           = 0;
    while ((at = object.find(needle, at)) != std::string_view::npos) {
        std::size_t cursor = at + needle.size();
        skip_space(object, cursor);
        if (cursor >= object.size() || object[cursor++] != ':') {
            at += needle.size();
            continue;
        }
        skip_space(object, cursor);
        if (decode_string(object, cursor, value)) return true;
        at += needle.size();
    }
    return false;
}

std::string json_escape(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8U);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (ch < 0x20U) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch);
                    result += hex.str();
                } else {
                    result.push_back(static_cast<char>(ch));
                }
        }
    }
    return result;
}

std::string payload_object(std::string_view frame)
{
    const std::size_t key = frame.find("\"payload\"");
    if (key == std::string_view::npos) return {};
    std::size_t at = frame.find('{', key);
    if (at == std::string_view::npos) return {};
    const std::size_t begin = at++;
    int depth               = 1;
    bool quoted             = false;
    bool escaped            = false;
    for (; at < frame.size(); ++at) {
        const char ch = frame[at];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"')
            quoted = true;
        else if (ch == '{')
            ++depth;
        else if (ch == '}' && --depth == 0)
            return std::string(frame.substr(begin, at - begin + 1U));
    }
    return {};
}

}  // namespace

PicoClawProtocol::PicoClawProtocol(std::string session_id)
    : session_id_(session_id.empty() ? new_id() : std::move(session_id))
{
}

const std::string &PicoClawProtocol::session_id() const noexcept
{
    return session_id_;
}

void PicoClawProtocol::reset_session(std::string session_id)
{
    session_id_ = session_id.empty() ? new_id() : std::move(session_id);
    sequence_   = 0;
}

std::string PicoClawProtocol::make_message_send(std::string_view content, std::string_view request_id)
{
    const std::string id =
        request_id.empty() ? (new_id() + "-" + std::to_string(++sequence_)) : std::string(request_id);
    return "{\"type\":\"message.send\",\"id\":\"" + json_escape(id) + "\",\"session_id\":\"" +
           json_escape(session_id_) + "\",\"payload\":{\"content\":\"" + json_escape(content) + "\"}}";
}

bool PicoClawProtocol::parse_event(std::string_view frame, PicoClawProtocolEvent &event, std::string &error)
{
    event                    = {};
    const std::string object = trim(frame);
    if (object.size() < 2U || object.front() != '{' || object.back() != '}') {
        error = "Pico Protocol frame must be a JSON object.";
        return false;
    }
    std::string type;
    if (!find_string(object, "type", type) || type.empty()) {
        error = "Pico Protocol frame has no type.";
        return false;
    }
    find_string(object, "id", event.id);
    find_string(object, "session_id", event.session_id);
    const std::string payload     = payload_object(object);
    const std::string_view values = payload.empty() ? std::string_view(object) : std::string_view(payload);
    find_string(values, "content", event.content);
    find_string(values, "message", event.error);

    if (type == "message.create")
        event.type = PicoClawProtocolEventType::MessageCreate;
    else if (type == "message.update")
        event.type = PicoClawProtocolEventType::MessageUpdate;
    else if (type == "typing.start")
        event.type = PicoClawProtocolEventType::TypingStart;
    else if (type == "typing.stop")
        event.type = PicoClawProtocolEventType::TypingStop;
    else if (type == "error")
        event.type = PicoClawProtocolEventType::Error;
    else if (type == "pong")
        event.type = PicoClawProtocolEventType::Pong;
    else
        event.type = PicoClawProtocolEventType::Unknown;
    error.clear();
    return true;
}

const char *PicoClawProtocol::event_type_name(PicoClawProtocolEventType type) noexcept
{
    switch (type) {
        case PicoClawProtocolEventType::MessageCreate:
            return "message.create";
        case PicoClawProtocolEventType::MessageUpdate:
            return "message.update";
        case PicoClawProtocolEventType::TypingStart:
            return "typing.start";
        case PicoClawProtocolEventType::TypingStop:
            return "typing.stop";
        case PicoClawProtocolEventType::Error:
            return "error";
        case PicoClawProtocolEventType::Pong:
            return "pong";
        case PicoClawProtocolEventType::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::string PicoClawProtocol::new_id()
{
    // The Flutter client persists UUID v4 session IDs. Generate the same
    // shape here so a future native WebSocket transport can share sessions
    // with the Android gateway without another identifier conversion layer.
    static std::atomic<std::uint64_t> counter{0};
    std::random_device entropy;
    std::mt19937_64 generator(static_cast<std::uint64_t>(entropy()) ^ (static_cast<std::uint64_t>(::getpid()) << 32U) ^
                              ++counter);
    std::uint64_t words[2]{generator(), generator()};
    words[0] = (words[0] & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    words[1] = (words[1] & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(8) << static_cast<std::uint32_t>(words[0] >> 32U) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(words[0] >> 16U) << '-' << std::setw(4)
           << static_cast<std::uint16_t>(words[0]) << '-' << std::setw(4) << static_cast<std::uint16_t>(words[1] >> 48U)
           << '-' << std::setw(12) << (words[1] & 0x0000ffffffffffffULL);
    return output.str();
}
