#include "test_environment.hpp"
#include "domain/picoclaw_protocol.hpp"
#include "pages/picoclaw/picoclaw_view_model.hpp"

#include <cassert>
#include <regex>
#include <string>

namespace {

void test_wire_round_trip()
{
    PicoClawProtocol generated;
    assert(std::regex_match(generated.session_id(),
                            std::regex("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")));
    PicoClawProtocol protocol("session-1");
    const std::string frame = protocol.make_message_send("say \"hi\"\nplease", "request-1");
    assert(frame ==
           "{\"type\":\"message.send\",\"id\":\"request-1\",\"session_id\":\"session-1\","
           "\"payload\":{\"content\":\"say \\\"hi\\\"\\nplease\"}}");

    PicoClawProtocolEvent event;
    std::string error;
    assert(PicoClawProtocol::parse_event(
        R"({"type":"message.update","id":"r1","session_id":"session-1","payload":{"content":"partial"}})", event,
        error));
    assert(event.type == PicoClawProtocolEventType::MessageUpdate);
    assert(event.id == "r1");
    assert(event.session_id == "session-1");
    assert(event.content == "partial");
    assert(PicoClawProtocol::parse_event(R"({"type":"error","payload":{"message":"bad key"}})", event, error));
    assert(event.type == PicoClawProtocolEventType::Error);
    assert(event.error == "bad key");
    assert(!PicoClawProtocol::parse_event("oops", event, error));
    assert(!error.empty());
}

void test_view_model_consumes_events()
{
    PicoClawClient client([](const PicoClawRuntimeRequest &) { return PicoClawRuntimeResult{}; });
    PicoClawViewModel view_model(client);
    const std::string session = view_model.protocol_session_id();
    assert(view_model.send("hello"));
    assert(view_model.last_protocol_frame().find("message.send") != std::string::npos);
    assert(view_model.handle_protocol_frame("{\"type\":\"message.update\",\"session_id\":\"" + session +
                                            "\",\"payload\":{\"content\":\"stream\"}}"));
    assert(view_model.messages().back().pending);
    assert(view_model.messages().back().text == "stream");
    assert(view_model.handle_protocol_frame("{\"type\":\"message.create\",\"session_id\":\"" + session +
                                            "\",\"payload\":{\"content\":\"done\"}}"));
    assert(!view_model.messages().back().pending);
    assert(view_model.messages().back().text == "done");
}

}  // namespace

int main()
{
    TestEnvironment environment;
    test_wire_round_trip();
    test_view_model_consumes_events();
    return 0;
}
