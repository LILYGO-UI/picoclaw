#ifndef LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_MODEL_HPP
#define LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_MODEL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class PicoClawScreen {
    Chat = 0,
    Models,
    Settings,
    Channels,
    Logs,
};

enum class PicoClawMessageRole {
    User,
    Assistant,
};

// Settings owned by the FUI configuration page. The LVGL client does not
// embed a browser, but retaining these values makes the same configuration
// portable between the Android UI and the CM0 UI. Runtime settings are kept
// separate from the model registry, which stores provider API keys privately.
struct PicoClawSettings {
    std::string host = "127.0.0.1";
    int port = 18800;
    std::string binary_path;
    std::string arguments;
    bool public_mode = false;
    std::string language = "en";
};

struct PicoClawProfile {
    // Editable provider display name shown as the model card title.
    std::string title;
    // Stable runtime alias; generated on add and retained on edit when omitted.
    std::string model_name;
    std::string model_id;
    std::string api_base;
    std::string api_key;
    // UI grouping is independent of the exact model ID returned by an API.
    std::string provider{};
};

struct PicoClawMessage {
    PicoClawMessageRole role = PicoClawMessageRole::Assistant;
    std::string text;
    bool pending = false;
};

struct PicoClawConversation {
    std::string id;
    std::string title;
    std::string cli_session;
    std::vector<PicoClawMessage> messages;
};

struct PicoClawRuntimeStatus {
    bool installed       = false;
    bool gateway_running = false;
    bool network_ready   = false;
    bool config_ready    = false;
    bool model_key_ready = false;
#if defined(__linux__)
    bool update_supported = true;
#else
    bool update_supported = false;
#endif
    std::string version  = "Not installed";
    std::string url      = "http://device:18790";
    // Web admin endpoint used by picoclaw_fui's Dashboard/Web pages.
    std::string web_url = "http://device:18800";
    std::string output;
};

struct PicoClawRelease {
    std::string tag;
    std::string name;
    std::string channel;
    std::string published_at;
    bool installable = false;
};

enum class PicoClawChannelFieldKind { Text, Secret, Integer, Boolean, List };

struct PicoClawChannelField {
    std::string key;
    std::string label;
    std::string help;
    std::string value;
    PicoClawChannelFieldKind kind = PicoClawChannelFieldKind::Text;
    bool required = false;
    // Groups mirror the sections used by the PicoClaw web configuration page.
    // The field key remains the persistence identity; this is presentation
    // metadata only and does not change the helper wire format.
    std::string section = "Configuration";
};

struct PicoClawChannel {
    std::string id;
    std::string title;
    std::string description;
    bool enabled = false;
    std::vector<PicoClawChannelField> fields;
};

enum class PicoClawWeixinBindingState { Idle, Starting, Waiting, Scanned, Confirmed, Expired, Failed, Cancelled };

struct PicoClawWeixinBinding {
    PicoClawWeixinBindingState state = PicoClawWeixinBindingState::Idle;
    std::string qr_url;
    std::string message = "Scan a QR code in Weixin to connect your account.";
    std::string account_id;

    [[nodiscard]] bool active() const noexcept
    {
        return state == PicoClawWeixinBindingState::Starting || state == PicoClawWeixinBindingState::Waiting ||
               state == PicoClawWeixinBindingState::Scanned;
    }
};

class PicoClawModel {
public:
    // Initial render batch; older messages remain stored and can be expanded.
    static constexpr std::size_t maximum_messages      = 32;
    static constexpr std::size_t maximum_prompt_length = 900;

    PicoClawModel();

    [[nodiscard]] static std::string_view profile_provider(const PicoClawProfile &profile) noexcept;

    [[nodiscard]] PicoClawScreen screen() const noexcept;
    [[nodiscard]] const std::vector<PicoClawProfile> &profiles() const noexcept;
    [[nodiscard]] std::size_t active_profile_index() const noexcept;
    [[nodiscard]] const PicoClawProfile &active_profile() const noexcept;
    [[nodiscard]] const std::string &profiles_error() const noexcept;
    [[nodiscard]] const std::vector<PicoClawMessage> &messages() const noexcept;
    [[nodiscard]] const std::vector<PicoClawConversation> &conversations() const noexcept;
    [[nodiscard]] std::size_t active_conversation_index() const noexcept;
    [[nodiscard]] const PicoClawConversation &active_conversation() const noexcept;
    [[nodiscard]] const std::string &history_error() const noexcept;
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
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t message_revision() const noexcept;

    bool show(PicoClawScreen screen) noexcept;
    bool back() noexcept;
    bool select_profile(std::size_t index) noexcept;
    bool update_active_profile(PicoClawProfile profile, std::string &error);
    // Manage the persisted model registry used by the Models page. Profiles
    // contain provider metadata and the API key required to connect to it.
    bool add_profile(PicoClawProfile profile, std::string &error);
    bool update_profile(std::size_t index, PicoClawProfile profile, std::string &error);
    bool remove_profile(std::size_t index, std::string &error);
    // Import a complete private helper response without replacing existing
    // aliases or exposing the payload through runtime output.
    bool merge_profiles(std::string_view output, std::string &error);
    // Validate and persist FUI-compatible runtime/UI settings.
    bool update_settings(PicoClawSettings settings, std::string &error);
    bool begin_prompt(std::string_view prompt, std::string &error);
    // Preserve the current transcript before starting or selecting a session.
    bool new_conversation();
    bool select_conversation(std::size_t index);
    // Updates the current assistant placeholder while a Pico Protocol stream
    // is in flight. The placeholder remains pending until complete_prompt().
    void update_prompt(std::string text);
    void complete_prompt(std::string reply, bool success);
    void apply_runtime_status(std::string_view output);
    // Accept a complete helper response atomically, preserving the last list
    // and selected tag if the response is malformed or incomplete.
    bool apply_update_result(std::string_view output, std::string &error);
    bool select_release(std::size_t index) noexcept;
    void invalidate_update_status();
    bool apply_channels_result(std::string_view output, std::string &error);
    bool edit_channel(std::size_t index);
    void close_channel();
    bool set_channel_enabled(bool enabled);
    bool set_channel_field(std::size_t index, std::string value);
    bool channel_save_payload(std::string &payload, std::string &error) const;
    void complete_channel_save(const PicoClawChannel &saved);
    void set_channels_status(std::string message, bool error = false);
    bool begin_weixin_binding();
    void cancel_weixin_binding();
    void fail_weixin_binding();
    void expire_weixin_binding();
    bool apply_weixin_binding_result(std::string_view output, std::string &error);
    [[nodiscard]] std::string weixin_binding_start_payload() const;
    [[nodiscard]] std::string weixin_binding_poll_payload() const;
    void set_output(std::string output);

private:
    void touch(bool messages = false) noexcept;
    void touch_channels() noexcept;
    void load_history();
    bool save_history();
    void load_profiles();
    bool save_profiles();

    PicoClawScreen screen_ = PicoClawScreen::Chat;
    std::vector<PicoClawProfile> profiles_;
    std::size_t active_profile_index_ = 0;
    std::string profiles_error_;
    bool profiles_readable_ = true;
    std::vector<PicoClawConversation> conversations_;
    std::size_t active_conversation_index_ = 0;
    std::string history_error_;
    bool history_readable_ = true;
    PicoClawSettings settings_;
    PicoClawRuntimeStatus runtime_;
    std::vector<PicoClawRelease> releases_;
    std::size_t selected_release_index_ = 0;
    bool updates_loaded_ = false;
    std::string update_message_ = "Check GitHub Releases to choose a version.";
    std::string update_status_ = "unknown";
    std::vector<PicoClawChannel> channels_;
    std::optional<PicoClawChannel> channel_draft_;
    bool channels_loaded_ = false;
    bool channels_restart_required_ = false;
    std::string channels_error_;
    std::string channels_status_ = "Load your chat channel configuration.";
    std::uint64_t channels_revision_ = 1;
    PicoClawWeixinBinding weixin_binding_;
    std::string weixin_binding_session_;
    std::string weixin_binding_base_url_;
    std::string weixin_binding_proxy_;
    std::uint64_t revision_         = 1;
    std::uint64_t message_revision_ = 1;
};

#endif
