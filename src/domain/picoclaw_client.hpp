#ifndef LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_CLIENT_HPP
#define LILYGO_UI_PICOCLAW_DOMAIN_PICOCLAW_CLIENT_HPP

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

enum class PicoClawRuntimeAction {
    Status,
    Install,
    CheckUpdates,
    InstallRelease,
    SaveConfig,
    Ask,
    GatewayStart,
    GatewayStop,
    Logs,
    TestConnection,
    FetchModels,
    ImportModels,
    LoadChannels,
    SaveChannel,
    // Direct iLink QR binding. The complete JSON request is sent on stdin so
    // qrcode values and the confirmed bot token never appear in argv.
    WeixinBindStart,
    WeixinBindPoll,
    // FUI-compatible WebSocket request. This is opt-in through
    // PICOCLAW_TRANSPORT=websocket; the default remains the Linux helper.
    ProtocolAsk,
};

struct PicoClawRuntimeRequest {
    PicoClawRuntimeAction action = PicoClawRuntimeAction::Status;
    std::vector<std::string> arguments;
};

struct PicoClawRuntimeResult {
    PicoClawRuntimeAction action = PicoClawRuntimeAction::Status;
    int exit_code                = -1;
    std::string output;
};

class PicoClawClient {
public:
    using Runner = std::function<PicoClawRuntimeResult(const PicoClawRuntimeRequest &)>;

    explicit PicoClawClient(Runner runner = {});
    ~PicoClawClient();

    PicoClawClient(const PicoClawClient &)            = delete;
    PicoClawClient &operator=(const PicoClawClient &) = delete;
    PicoClawClient(PicoClawClient &&)                 = delete;
    PicoClawClient &operator=(PicoClawClient &&)      = delete;

    bool submit(PicoClawRuntimeAction action, std::vector<std::string> arguments = {});
    bool poll(PicoClawRuntimeResult &result);
    [[nodiscard]] bool busy() const noexcept;

private:
    PicoClawRuntimeResult run_control_script(const PicoClawRuntimeRequest &request);
    PicoClawRuntimeResult run_protocol_request(const PicoClawRuntimeRequest &request);

    Runner runner_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::optional<PicoClawRuntimeResult> result_;
    std::atomic<pid_t> child_pid_{-1};
    std::atomic_bool stopping_{false};
    bool busy_ = false;
};

#endif
