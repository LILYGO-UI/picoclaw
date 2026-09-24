#include "domain/picoclaw_client.hpp"
#include "domain/picoclaw_protocol.hpp"
#include "domain/picoclaw_websocket.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <spawn.h>
#include <string_view>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

#ifndef LILYGO_PICOCLAW_CONTROL_SCRIPT
#define LILYGO_PICOCLAW_CONTROL_SCRIPT "/usr/lib/lilygo-ui-picoclaw/picoclaw-control"
#endif

#ifndef LILYGO_PICOCLAW_SOURCE_CONTROL_SCRIPT
#define LILYGO_PICOCLAW_SOURCE_CONTROL_SCRIPT "scripts/picoclaw-control.sh"
#endif

namespace {

constexpr std::size_t maximum_output = 16U * 1024U;
constexpr std::size_t maximum_update_output = 1024U * 1024U;
std::mutex command_spawn_mutex;

bool create_command_pipe(int descriptors[2])
{
    if (::pipe(descriptors) != 0) return false;
    if (::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) == 0 &&
        ::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == 0) return true;
    const int error = errno;
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    errno = error;
    return false;
}

const char *command_name(PicoClawRuntimeAction action)
{
    switch (action) {
        case PicoClawRuntimeAction::Status:
            return "status";
        case PicoClawRuntimeAction::Install:
            return "install";
        case PicoClawRuntimeAction::CheckUpdates:
            return "check-updates";
        case PicoClawRuntimeAction::InstallRelease:
            return "install-release";
        case PicoClawRuntimeAction::SaveConfig:
            return "save-config";
        case PicoClawRuntimeAction::Ask:
            return "ask";
        case PicoClawRuntimeAction::GatewayStart:
            return "gateway-start";
        case PicoClawRuntimeAction::GatewayStop:
            return "gateway-stop";
        case PicoClawRuntimeAction::Logs:
            return "log";
        case PicoClawRuntimeAction::TestConnection:
            return "test-connection";
        case PicoClawRuntimeAction::FetchModels:
            return "fetch-models";
        case PicoClawRuntimeAction::ImportModels:
            return "export-models";
        case PicoClawRuntimeAction::LoadChannels:
            return "load-channels";
        case PicoClawRuntimeAction::SaveChannel:
            return "save-channel";
        case PicoClawRuntimeAction::WeixinBindStart:
            return "weixin-bind-start";
        case PicoClawRuntimeAction::WeixinBindPoll:
            return "weixin-bind-poll";
        case PicoClawRuntimeAction::ProtocolAsk:
            return "protocol-ask";
    }
    return "status";
}

std::string control_script_path()
{
    if (const char *override_path = std::getenv("LILYGO_PICOCLAW_CONTROL")) {
        if (*override_path) return override_path;
    }
    if (::access(LILYGO_PICOCLAW_CONTROL_SCRIPT, R_OK) == 0) return LILYGO_PICOCLAW_CONTROL_SCRIPT;
    return LILYGO_PICOCLAW_SOURCE_CONTROL_SCRIPT;
}

int normalized_exit_code(int status)
{
    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

}  // namespace

PicoClawClient::PicoClawClient(Runner runner)
{
    runner_ = runner ? std::move(runner)
                     : Runner([this](const PicoClawRuntimeRequest &request) { return run_control_script(request); });
}

PicoClawClient::~PicoClawClient()
{
    stopping_.store(true);
    const pid_t child = child_pid_.load();
    if (child > 0) (void)::kill(-child, SIGTERM);
    if (worker_.joinable()) worker_.join();
}

bool PicoClawClient::submit(PicoClawRuntimeAction action, std::vector<std::string> arguments)
{
    std::unique_lock<std::mutex> lock(mutex_);
    // A completed worker still owns a result until the UI polls it. Starting
    // another command here would otherwise discard that result (and could
    // leave a chat response pending when the user checks for updates).
    if (busy_ || result_) return false;
    if (worker_.joinable()) {
        lock.unlock();
        worker_.join();
        lock.lock();
    }
    busy_ = true;
    result_.reset();
    PicoClawRuntimeRequest request{action, std::move(arguments)};
    worker_ = std::thread([this, request = std::move(request)]() {
        PicoClawRuntimeResult result;
        try {
            result = runner_(request);
        } catch (const std::exception &error) {
            result.exit_code = -1;
            result.output    = std::string("PicoClaw command failed: ") + error.what();
        } catch (...) {
            result.exit_code = -1;
            result.output    = "PicoClaw command failed unexpectedly.";
        }
        result.action = request.action;
        std::lock_guard<std::mutex> result_lock(mutex_);
        result_ = std::move(result);
        busy_   = false;
    });
    return true;
}

bool PicoClawClient::poll(PicoClawRuntimeResult &result)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!result_) return false;
    result = std::move(*result_);
    result_.reset();
    lock.unlock();
    if (worker_.joinable()) worker_.join();
    return true;
}

bool PicoClawClient::busy() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_ || result_.has_value();
}

PicoClawRuntimeResult PicoClawClient::run_control_script(const PicoClawRuntimeRequest &request)
{
    if (request.action == PicoClawRuntimeAction::ProtocolAsk) return run_protocol_request(request);
    PicoClawRuntimeResult result;
    result.action = request.action;
    if (stopping_.load()) {
        result.output = "PicoClaw command was cancelled.";
        return result;
    }
    const std::string script = control_script_path();
    if (::access(script.c_str(), R_OK) != 0) {
        result.exit_code = 127;
        result.output    = "PicoClaw control helper was not found.";
        return result;
    }

    // Serialize pipe creation through spawn so another client's child cannot
    // inherit a descriptor before its close-on-exec flag has been set.
    std::unique_lock<std::mutex> spawn_lock(command_spawn_mutex);
    int output_pipe[2]{};
    if (!create_command_pipe(output_pipe)) {
        result.output = std::string("Could not create command pipe: ") + std::strerror(errno);
        return result;
    }
    int input_pipe[2]{};
    if (!create_command_pipe(input_pipe)) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        result.output = std::string("Could not create input pipe: ") + std::strerror(errno);
        return result;
    }

    std::string standard_input;
    std::vector<std::string> command_arguments;
    if (request.action == PicoClawRuntimeAction::SaveChannel) {
        if (request.arguments.size() != 2U) {
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
            ::close(input_pipe[0]);
            ::close(input_pipe[1]);
            result.output = "Channel configuration is missing or invalid.";
            return result;
        }
        command_arguments = {request.arguments.front(), "--json-stdin"};
        standard_input = request.arguments.back();
    } else if (request.action == PicoClawRuntimeAction::WeixinBindStart ||
               request.action == PicoClawRuntimeAction::WeixinBindPoll) {
        if (request.arguments.size() != 1U) {
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
            ::close(input_pipe[0]);
            ::close(input_pipe[1]);
            result.output = "Weixin binding request is missing its JSON payload.";
            return result;
        }
        // The helper takes one JSON object on stdin. Keeping the payload out
        // of argv avoids exposing qrcode and bot_token values to ps(1).
        standard_input = request.arguments.front();
    } else if (request.action == PicoClawRuntimeAction::SaveConfig && request.arguments.size() == 4U) {
        command_arguments.assign(request.arguments.begin(), request.arguments.begin() + 3);
        command_arguments.emplace_back("--key-stdin");
        standard_input = request.arguments.back();
    } else if ((request.action == PicoClawRuntimeAction::TestConnection ||
                request.action == PicoClawRuntimeAction::FetchModels) && request.arguments.size() == 2U) {
        command_arguments.emplace_back(request.arguments.front());
        command_arguments.emplace_back("--key-stdin");
        standard_input = request.arguments.back();
    } else if (request.action == PicoClawRuntimeAction::Ask &&
               (request.arguments.size() >= 1U && request.arguments.size() <= 3U)) {
        command_arguments.emplace_back("--prompt-stdin");
        command_arguments.insert(command_arguments.end(), request.arguments.begin() + 1, request.arguments.end());
        standard_input = request.arguments.front();
    } else {
        command_arguments = request.arguments;
    }

    std::vector<std::string> values{"/bin/sh", script, command_name(request.action)};
    values.insert(values.end(), command_arguments.begin(), command_arguments.end());
    std::vector<char *> arguments;
    arguments.reserve(values.size() + 1U);
    for (auto &value : values) arguments.push_back(value.data());
    arguments.push_back(nullptr);

    posix_spawn_file_actions_t file_actions;
    posix_spawnattr_t attributes;
    (void)::posix_spawn_file_actions_init(&file_actions);
    (void)::posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&file_actions, input_pipe[0], STDIN_FILENO);
    (void)::posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);
    (void)::posix_spawn_file_actions_addclose(&file_actions, output_pipe[1]);
    (void)::posix_spawn_file_actions_addclose(&file_actions, input_pipe[0]);
    (void)::posix_spawn_file_actions_addclose(&file_actions, input_pipe[1]);
    (void)::posix_spawnattr_init(&attributes);
    (void)::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    (void)::posix_spawnattr_setpgroup(&attributes, 0);
    pid_t process         = -1;
    const int spawn_error = ::posix_spawn(&process, "/bin/sh", &file_actions, &attributes, arguments.data(), environ);
    (void)::posix_spawn_file_actions_destroy(&file_actions);
    (void)::posix_spawnattr_destroy(&attributes);

    ::close(output_pipe[1]);
    ::close(input_pipe[0]);
    if (spawn_error != 0) {
        ::close(output_pipe[0]);
        ::close(input_pipe[1]);
        result.output = std::string("Could not start PicoClaw helper: ") + std::strerror(spawn_error);
        return result;
    }
    child_pid_.store(process);
    if (stopping_.load()) (void)::kill(-process, SIGTERM);
    spawn_lock.unlock();

    std::size_t written = 0;
    while (written < standard_input.size()) {
        const ssize_t count = ::write(input_pipe[1], standard_input.data() + written, standard_input.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    ::close(input_pipe[1]);

    const std::size_t output_limit = request.action == PicoClawRuntimeAction::CheckUpdates ||
                                             request.action == PicoClawRuntimeAction::ImportModels ||
                                             request.action == PicoClawRuntimeAction::LoadChannels
                                         ? maximum_update_output : maximum_output;
    bool output_overflow = false;
    std::array<char, 1024> buffer{};
    for (;;) {
        const ssize_t count = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (request.action == PicoClawRuntimeAction::InstallRelease) {
            // Installation can print download progress before the final
            // restart/rollback result. Keep that result within the same limit.
            result.output.append(buffer.data(), static_cast<std::size_t>(count));
            if (result.output.size() > output_limit) result.output.erase(0, result.output.size() - output_limit);
            continue;
        }
        const std::size_t available = output_limit > result.output.size() ? output_limit - result.output.size() : 0;
        if (static_cast<std::size_t>(count) > available) output_overflow = true;
        if (available != 0U) {
            result.output.append(buffer.data(), std::min<std::size_t>(static_cast<std::size_t>(count), available));
        }
    }
    ::close(output_pipe[0]);
    int status = -1;
    while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    child_pid_.store(-1);
    result.exit_code = normalized_exit_code(status);
    if (output_overflow && request.action == PicoClawRuntimeAction::CheckUpdates) {
        result.exit_code = 1;
        result.output = "The release list exceeded the 1 MiB limit. No versions were updated.";
    }
    if (request.action == PicoClawRuntimeAction::ImportModels && (output_overflow || result.exit_code != 0)) {
        result.exit_code = 1;
        result.output = "Could not import runtime models. Check the existing configuration.";
    }
    if (request.action == PicoClawRuntimeAction::LoadChannels && (output_overflow || result.exit_code != 0)) {
        result.exit_code = 1;
        result.output = "Could not load channels. Check the existing configuration.";
    }
    return result;
}

PicoClawRuntimeResult PicoClawClient::run_protocol_request(const PicoClawRuntimeRequest &request)
{
    PicoClawRuntimeResult result;
    result.action = request.action;
    if (request.arguments.size() < 3U) {
        result.output = "WebSocket request is missing endpoint, token, or frame.";
        return result;
    }
    PicoClawWebSocket socket;
    std::string error;
    if (!socket.connect(request.arguments[0], error, 5000, request.arguments[1]) ||
        !socket.send_text(request.arguments[2], error)) {
        result.output = error;
        return result;
    }
    bool terminal_event = false;
    // Keep each protocol event on its own line so the LVGL polling layer can
    // feed the frames to PicoClawViewModel::handle_protocol_frame().
    for (;;) {
        std::string frame;
        if (!socket.receive_text(frame, error, 500)) {
            if (error == "WebSocket read timed out") {
                if (stopping_.load()) break;
                continue;
            }
            if (result.output.empty()) result.output = error;
            break;
        }
        result.output += frame;
        result.output.push_back('\n');
        PicoClawProtocolEvent event;
        std::string parse_error;
        if (PicoClawProtocol::parse_event(frame, event, parse_error) &&
            (event.type == PicoClawProtocolEventType::MessageCreate ||
             event.type == PicoClawProtocolEventType::Error)) {
            terminal_event = true;
            break;
        }
        if (stopping_.load()) break;
    }
    socket.close();
    result.exit_code = terminal_event ? 0 : -1;
    return result;
}
