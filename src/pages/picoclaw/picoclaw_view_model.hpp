#ifndef LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_VIEW_MODEL_HPP
#define LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_VIEW_MODEL_HPP

#include "components/lv_subject.hpp"
#include "domain/picoclaw_client.hpp"
#include "domain/picoclaw_protocol.hpp"
#include "pages/picoclaw/picoclaw_model.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

enum class PicoClawConnectionState { Unknown, Checking, Connected, Failed };

struct PicoClawProviderStatus {
    PicoClawConnectionState connection = PicoClawConnectionState::Unknown;
    std::string available = "Available: --";
};

class PicoClawViewModel {
public:
    using BindingClock = std::function<std::chrono::steady_clock::time_point()>;
    explicit PicoClawViewModel(PicoClawClient &client, PicoClawClient::Runner provider_runner = {},
                              PicoClawClient::Runner binding_runner = {}, BindingClock binding_clock = {});

    PicoClawViewModel(const PicoClawViewModel &)            = delete;
    PicoClawViewModel &operator=(const PicoClawViewModel &) = delete;

    [[nodiscard]] PicoClawScreen screen() const noexcept;
    [[nodiscard]] const std::vector<PicoClawProfile> &profiles() const noexcept;
    [[nodiscard]] const PicoClawProfile &active_profile() const noexcept;
    [[nodiscard]] const std::string &profiles_error() const noexcept;
    [[nodiscard]] std::size_t active_profile_index() const noexcept;
    [[nodiscard]] const PicoClawProfile &chat_profile() const noexcept;
    [[nodiscard]] std::size_t chat_profile_index() const noexcept;
    [[nodiscard]] bool chat_model_ready() const noexcept;
    [[nodiscard]] const std::vector<PicoClawMessage> &messages() const noexcept;
    [[nodiscard]] const std::vector<PicoClawConversation> &conversations() const noexcept;
    [[nodiscard]] std::size_t active_conversation_index() const noexcept;
    [[nodiscard]] const PicoClawRuntimeStatus &runtime() const noexcept;
    [[nodiscard]] const PicoClawSettings &settings() const noexcept;
    [[nodiscard]] const std::vector<PicoClawRelease> &releases() const noexcept;
    [[nodiscard]] std::size_t selected_release_index() const noexcept;
    [[nodiscard]] const PicoClawRelease *selected_release() const noexcept;
    [[nodiscard]] bool updates_loaded() const noexcept;
    [[nodiscard]] const std::string &update_message() const noexcept;
    [[nodiscard]] const std::string &update_status() const noexcept;
    [[nodiscard]] const std::vector<PicoClawChannel> &channels() const noexcept;
    [[nodiscard]] bool channels_loaded() const noexcept;
    [[nodiscard]] const std::string &channels_error() const noexcept;
    [[nodiscard]] const std::string &channels_status() const noexcept;
    [[nodiscard]] const PicoClawChannel *channel_draft() const noexcept;
    [[nodiscard]] bool channel_dirty() const noexcept;
    [[nodiscard]] bool channels_restart_required() const noexcept;
    [[nodiscard]] std::uint64_t channels_revision() const noexcept;
    [[nodiscard]] const PicoClawWeixinBinding &weixin_binding() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t message_revision() const noexcept;
    [[nodiscard]] const std::string &protocol_session_id() const noexcept;
    // The most recent Pico Protocol frame produced by send(). A transport
    // adapter can write this frame to its WebSocket without coupling LVGL to
    // a particular socket implementation.
    [[nodiscard]] const std::string &last_protocol_frame() const noexcept;
    [[nodiscard]] const std::vector<std::string> &fetched_models() const noexcept;
    [[nodiscard]] bool fetched_models_match(std::string_view api_base, std::string_view api_key) const noexcept;
    [[nodiscard]] const PicoClawProviderStatus &provider_status(const PicoClawProfile &profile) const noexcept;

    [[nodiscard]] lv_subject_t *screen_subject() noexcept;
    [[nodiscard]] lv_subject_t *busy_subject() noexcept;
    [[nodiscard]] lv_subject_t *status_subject() noexcept;
    [[nodiscard]] lv_subject_t *profile_subject() noexcept;

    void show_chat();
    void show_models();
    void show_logs();
    void show_settings();
    void show_channels();
    void load_channels();
    bool edit_channel(std::size_t index);
    void close_channel();
    bool set_channel_enabled(bool enabled);
    bool set_channel_field(std::size_t index, std::string value);
    bool save_channel();
    bool start_weixin_binding();
    void cancel_weixin_binding();
    bool back();
    void select_profile(std::size_t index);
    bool select_chat_profile(std::size_t index);
    bool save_profile(PicoClawProfile profile);
    // Registry operations for the dedicated Models page. These update local
    // persisted state and publish immediately; save_profile additionally
    // applies the selected profile to the running PicoClaw configuration.
    bool add_profile(PicoClawProfile profile);
    bool update_profile(std::size_t index, PicoClawProfile profile);
    bool remove_profile(std::size_t index);
    void import_models();
    bool save_settings(PicoClawSettings settings);
    bool send(std::string_view prompt);
    bool new_chat();
    bool select_conversation(std::size_t index);
    void request_status();
    void install_runtime();
    void check_updates();
    void select_release(std::size_t index);
    void install_selected_release();
    void start_gateway();
    void stop_gateway();
    void request_logs();
    void test_connection(std::string api_base, std::string api_key);
    void fetch_models(std::string api_base, std::string api_key);
    bool poll();
    // Feeds one text frame received from /pico/ws into the conversation model.
    // Returns false for malformed JSON; protocol errors are published through
    // status_subject() so the view can show them consistently.
    bool handle_protocol_frame(std::string_view frame);

private:
    struct ProviderQuery {
        std::string api_base;
        std::string api_key;
        std::uint64_t id = 0;
    };
    struct ProviderCheck {
        ProviderQuery query;
        PicoClawProviderStatus status;
    };
    enum class ChatModelSwitch { None, Saving, StoppingGateway, StartingGateway, Verifying };
    bool poll_chat_model_switch(const PicoClawRuntimeResult &result);
    void fail_chat_model_switch(std::string_view message);
    void clear_fetched_models();
    ProviderQuery begin_provider_check(const std::string &api_base, const std::string &api_key, bool background = false);
    void complete_provider_check(const PicoClawRuntimeResult &result, const ProviderQuery &query);
    [[nodiscard]] bool current_provider_query(const ProviderQuery &query) const noexcept;
    void refresh_provider_statuses();
    void poll_provider_checks();
    bool poll_weixin_binding();
    void submit_weixin_binding_request();
    bool submit(PicoClawRuntimeAction action, std::vector<std::string> arguments, std::string_view status);
    static std::string clean_reply(std::string_view output, bool success);
    void publish();

    PicoClawClient &client_;
    PicoClawClient provider_client_;
    PicoClawClient binding_client_;
    BindingClock binding_clock_;
    PicoClawModel model_;
    PicoClawProtocol protocol_;
    PicoClawProfile chat_profile_;
    PicoClawProfile pending_chat_profile_;
    PicoClawProfile saved_configuration_profile_;
    ChatModelSwitch chat_model_switch_ = ChatModelSwitch::None;
    bool chat_model_ready_ = true;
    bool chat_profile_confirmed_ = false;
    bool switch_websocket_gateway_ = false;
    std::string last_protocol_frame_;
    std::string status_text_ = "Ready";
    std::string operation_result_status_;
    bool preserve_operation_result_ = false;
    std::vector<std::string> fetched_models_;
    std::string fetched_api_base_;
    std::string fetched_api_key_;
    std::string pending_fetch_api_base_;
    std::string pending_fetch_api_key_;
    bool fetched_models_valid_ = false;
    bool model_fetch_pending_ = false;
    std::vector<ProviderCheck> provider_checks_;
    std::optional<ProviderQuery> pending_provider_check_;
    std::optional<ProviderQuery> background_provider_check_;
    std::vector<ProviderQuery> provider_check_queue_;
    std::uint64_t provider_revision_ = 0;
    bool channel_load_pending_ = false;
    std::optional<PicoClawChannel> saving_channel_;
    std::uint64_t binding_generation_ = 0;
    std::uint64_t binding_request_generation_ = 0;
    std::chrono::steady_clock::time_point next_binding_poll_;
    std::chrono::steady_clock::time_point binding_deadline_;
    bool binding_start_pending_ = false;
    bool restart_after_weixin_save_ = false;
    IntSubject screen_;
    IntSubject busy_;
    StringSubject<192> status_;
    StringSubject<96> profile_;
};

#endif
