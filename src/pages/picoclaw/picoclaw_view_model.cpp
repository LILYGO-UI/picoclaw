#include "pages/picoclaw/picoclaw_view_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <utility>

namespace {

bool websocket_transport()
{
    const char *transport = std::getenv("PICOCLAW_TRANSPORT");
    return transport && std::string_view(transport) == "websocket";
}

std::string runtime_field(std::string_view output, std::string_view name)
{
    std::istringstream input{std::string(output)};
    std::string line;
    while (std::getline(input, line))
        if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == '=')
            return line.substr(name.size() + 1U);
    return {};
}

bool matching_profile(const PicoClawProfile &left, const PicoClawProfile &right)
{
    return left.model_name == right.model_name && left.model_id == right.model_id && left.api_base == right.api_base;
}

bool status_matches_profile(std::string_view output, const PicoClawProfile &profile)
{
    return runtime_field(output, "model_name") == profile.model_name &&
           runtime_field(output, "model_id") == profile.model_id && runtime_field(output, "api_base") == profile.api_base;
}

}  // namespace

PicoClawViewModel::PicoClawViewModel(PicoClawClient &client, PicoClawClient::Runner provider_runner,
                                     PicoClawClient::Runner binding_runner, BindingClock binding_clock)
    : client_(client),
      provider_client_(std::move(provider_runner)),
      binding_client_(std::move(binding_runner)),
      binding_clock_(binding_clock ? std::move(binding_clock) : BindingClock([] { return std::chrono::steady_clock::now(); })),
      chat_profile_(model_.active_profile()),
      screen_(static_cast<std::int32_t>(model_.screen()), static_cast<std::int32_t>(PicoClawScreen::Chat),
              static_cast<std::int32_t>(PicoClawScreen::Logs)),
      busy_(0, 0, 1),
      status_(status_text_),
      profile_(model_.active_profile().title)
{
    protocol_.reset_session(model_.active_conversation().id);
    if (!model_.profiles_error().empty()) status_text_ = model_.profiles_error();
    if (!model_.history_error().empty()) status_text_ = model_.history_error();
    publish();
}

PicoClawScreen PicoClawViewModel::screen() const noexcept
{
    return model_.screen();
}
const std::vector<PicoClawProfile> &PicoClawViewModel::profiles() const noexcept
{
    return model_.profiles();
}
const PicoClawProfile &PicoClawViewModel::active_profile() const noexcept
{
    return model_.active_profile();
}
const std::string &PicoClawViewModel::profiles_error() const noexcept
{
    return model_.profiles_error();
}
std::size_t PicoClawViewModel::active_profile_index() const noexcept
{
    return model_.active_profile_index();
}
const PicoClawProfile &PicoClawViewModel::chat_profile() const noexcept
{
    return chat_profile_;
}
std::size_t PicoClawViewModel::chat_profile_index() const noexcept
{
    const auto &profiles = model_.profiles();
    for (std::size_t index = 0; index < profiles.size(); ++index)
        if (matching_profile(profiles[index], chat_profile_)) return index;
    return profiles.size();
}
bool PicoClawViewModel::chat_model_ready() const noexcept
{
    return chat_model_ready_ && chat_model_switch_ == ChatModelSwitch::None && chat_profile_index() < model_.profiles().size();
}
const std::vector<PicoClawMessage> &PicoClawViewModel::messages() const noexcept
{
    return model_.messages();
}
const std::vector<PicoClawConversation> &PicoClawViewModel::conversations() const noexcept
{
    return model_.conversations();
}
std::size_t PicoClawViewModel::active_conversation_index() const noexcept
{
    return model_.active_conversation_index();
}
const PicoClawRuntimeStatus &PicoClawViewModel::runtime() const noexcept
{
    return model_.runtime();
}
const PicoClawSettings &PicoClawViewModel::settings() const noexcept
{
    return model_.settings();
}
const std::vector<PicoClawRelease> &PicoClawViewModel::releases() const noexcept
{
    return model_.releases();
}
std::size_t PicoClawViewModel::selected_release_index() const noexcept
{
    return model_.selected_release_index();
}
const PicoClawRelease *PicoClawViewModel::selected_release() const noexcept
{
    return model_.selected_release();
}
bool PicoClawViewModel::updates_loaded() const noexcept
{
    return model_.updates_loaded();
}
const std::string &PicoClawViewModel::update_message() const noexcept
{
    return model_.update_message();
}
const std::string &PicoClawViewModel::update_status() const noexcept
{
    return model_.update_status();
}
const std::vector<PicoClawChannel> &PicoClawViewModel::channels() const noexcept { return model_.channels(); }
bool PicoClawViewModel::channels_loaded() const noexcept { return model_.channels_loaded(); }
const std::string &PicoClawViewModel::channels_error() const noexcept { return model_.channels_error(); }
const std::string &PicoClawViewModel::channels_status() const noexcept { return model_.channels_status(); }
const PicoClawChannel *PicoClawViewModel::channel_draft() const noexcept { return model_.channel_draft(); }
bool PicoClawViewModel::channel_dirty() const noexcept { return model_.channel_dirty(); }
bool PicoClawViewModel::channels_restart_required() const noexcept { return model_.channels_restart_required(); }
std::uint64_t PicoClawViewModel::channels_revision() const noexcept { return model_.channels_revision(); }
const PicoClawWeixinBinding &PicoClawViewModel::weixin_binding() const noexcept { return model_.weixin_binding(); }

bool PicoClawViewModel::busy() const noexcept
{
    return client_.busy();
}
std::uint64_t PicoClawViewModel::revision() const noexcept
{
    return model_.revision() + provider_revision_;
}
std::uint64_t PicoClawViewModel::message_revision() const noexcept
{
    return model_.message_revision();
}
const std::string &PicoClawViewModel::protocol_session_id() const noexcept
{
    return protocol_.session_id();
}
const std::string &PicoClawViewModel::last_protocol_frame() const noexcept
{
    return last_protocol_frame_;
}
const std::vector<std::string> &PicoClawViewModel::fetched_models() const noexcept
{
    return fetched_models_;
}
bool PicoClawViewModel::fetched_models_match(std::string_view api_base, std::string_view api_key) const noexcept
{
    return fetched_models_valid_ && api_base == fetched_api_base_ && api_key == fetched_api_key_;
}

const PicoClawProviderStatus &PicoClawViewModel::provider_status(const PicoClawProfile &profile) const noexcept
{
    for (const auto &check : provider_checks_)
        if (check.query.api_base == profile.api_base && check.query.api_key == profile.api_key) return check.status;
    static const PicoClawProviderStatus unknown;
    return unknown;
}

PicoClawViewModel::ProviderQuery PicoClawViewModel::begin_provider_check(const std::string &api_base,
                                                                      const std::string &api_key, bool background)
{
    // Keep the saved accounts and the current editor draft, without retaining
    // credentials from every previously tested draft for the whole session.
    if (!background) {
        provider_checks_.erase(std::remove_if(provider_checks_.begin(), provider_checks_.end(), [&](const ProviderCheck &check) {
            return std::none_of(model_.profiles().begin(), model_.profiles().end(), [&](const PicoClawProfile &profile) {
                return check.query.api_base == profile.api_base && check.query.api_key == profile.api_key;
            });
        }), provider_checks_.end());
    }
    auto found = std::find_if(provider_checks_.begin(), provider_checks_.end(), [&](const ProviderCheck &check) {
        return check.query.api_base == api_base && check.query.api_key == api_key;
    });
    if (found == provider_checks_.end()) {
        provider_checks_.push_back({{api_base, api_key}, {}});
        found = provider_checks_.end() - 1;
    }
    found->query.id = ++provider_revision_;
    found->status = {PicoClawConnectionState::Checking, "Available: --"};
    return found->query;
}

void PicoClawViewModel::complete_provider_check(const PicoClawRuntimeResult &result, const ProviderQuery &query)
{
    if (!current_provider_query(query)) return;
    for (auto &check : provider_checks_) {
        if (check.query.id != query.id) continue;
        const bool connected = result.exit_code == 0;
        check.status = {connected ? PicoClawConnectionState::Connected : PicoClawConnectionState::Failed,
                        "Available: --"};
        if (!connected) break;
        const auto quota = runtime_field(result.output, "quota");
        check.status.available = quota == "unsupported" ? "Available: Not provided"
                                 : quota == "unmetered" ? "Available: Not metered"
                                 : quota == "unlimited" ? "Available: Unlimited"
                                                       : "Available: Unavailable";
        if (quota == "available") {
            std::istringstream input(result.output);
            std::string line, balances;
            std::size_t count = 0;
            // Only accept numeric amounts and currency codes from the helper.
            // Raw provider errors must never reach the list or reveal a key.
            static const std::regex balance_pattern("balance=([A-Z]{3})\\t(-?[0-9]{1,16}(\\.[0-9]{1,8})?)");
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::smatch match;
                if (std::regex_match(line, match, balance_pattern) && count < 4) {
                    if (count++) balances += " / ";
                    balances += match[2].str() + " " + match[1].str();
                }
            }
            if (!balances.empty()) check.status.available = "Available: " + balances;
        }
        break;
    }
    ++provider_revision_;
}

bool PicoClawViewModel::current_provider_query(const ProviderQuery &query) const noexcept
{
    return std::any_of(provider_checks_.begin(), provider_checks_.end(), [&](const ProviderCheck &check) {
        return check.query.id == query.id;
    });
}

void PicoClawViewModel::refresh_provider_statuses()
{
    // Discard deleted accounts, while allowing an editor's in-flight test to
    // complete even if its draft has not been saved into the model registry.
    provider_checks_.erase(std::remove_if(provider_checks_.begin(), provider_checks_.end(), [&](const ProviderCheck &check) {
        if (pending_provider_check_ && check.query.id == pending_provider_check_->id) return false;
        return std::none_of(model_.profiles().begin(), model_.profiles().end(), [&](const PicoClawProfile &profile) {
            return check.query.api_base == profile.api_base && check.query.api_key == profile.api_key;
        });
    }), provider_checks_.end());

    std::vector<ProviderQuery> queue;
    for (const auto &profile : model_.profiles()) {
        if (profile.api_base.empty() ||
            (profile.api_key.empty() && PicoClawModel::profile_provider(profile) != "ollama")) continue;
        auto matches = [&](const ProviderQuery &query) {
            return query.api_base == profile.api_base && query.api_key == profile.api_key;
        };
        if (std::any_of(queue.begin(), queue.end(), matches)) continue;
        if (pending_provider_check_ && matches(*pending_provider_check_)) continue;
        if (background_provider_check_ && matches(*background_provider_check_) &&
            current_provider_query(*background_provider_check_)) continue;
        const auto queued = std::find_if(provider_check_queue_.begin(), provider_check_queue_.end(), matches);
        if (queued != provider_check_queue_.end() && current_provider_query(*queued)) queue.push_back(*queued);
        else queue.push_back(begin_provider_check(profile.api_base, profile.api_key, true));
    }
    provider_check_queue_ = std::move(queue);
    poll_provider_checks();
}

void PicoClawViewModel::poll_provider_checks()
{
    PicoClawRuntimeResult result;
    if (provider_client_.poll(result)) {
        if (background_provider_check_) complete_provider_check(result, *background_provider_check_);
        background_provider_check_.reset();
    }
    if (provider_client_.busy()) return;
    while (!provider_check_queue_.empty()) {
        const auto query = provider_check_queue_.front();
        // Edits/deletions and newer manual requests may invalidate a queued check.
        const bool saved = std::any_of(model_.profiles().begin(), model_.profiles().end(), [&](const PicoClawProfile &profile) {
            return query.api_base == profile.api_base && query.api_key == profile.api_key;
        });
        if (!saved || !current_provider_query(query)) {
            provider_check_queue_.erase(provider_check_queue_.begin());
            continue;
        }
        if (!provider_client_.submit(PicoClawRuntimeAction::TestConnection, {query.api_base, query.api_key})) return;
        background_provider_check_ = query;
        provider_check_queue_.erase(provider_check_queue_.begin());
        return;
    }
}

void PicoClawViewModel::clear_fetched_models()
{
    fetched_models_.clear();
    fetched_api_base_.clear();
    fetched_api_key_.clear();
    fetched_models_valid_ = false;
}
lv_subject_t *PicoClawViewModel::screen_subject() noexcept
{
    return screen_.get();
}
lv_subject_t *PicoClawViewModel::busy_subject() noexcept
{
    return busy_.get();
}
lv_subject_t *PicoClawViewModel::status_subject() noexcept
{
    return status_.get();
}
lv_subject_t *PicoClawViewModel::profile_subject() noexcept
{
    return profile_.get();
}

void PicoClawViewModel::show_chat()
{
    cancel_weixin_binding();
    model_.show(PicoClawScreen::Chat);
    publish();
}

void PicoClawViewModel::show_models()
{
    cancel_weixin_binding();
    if (model_.show(PicoClawScreen::Models)) refresh_provider_statuses();
    if (!model_.profiles_error().empty()) status_text_ = model_.profiles_error();
    publish();
}

void PicoClawViewModel::show_logs()
{
    cancel_weixin_binding();
    model_.show(PicoClawScreen::Logs);
    publish();
}

void PicoClawViewModel::show_settings()
{
    cancel_weixin_binding();
    model_.show(PicoClawScreen::Settings);
    publish();
}

void PicoClawViewModel::show_channels()
{
    model_.show(PicoClawScreen::Channels);
    if (!model_.channels_loaded()) {
        if (client_.busy()) {
            channel_load_pending_ = true;
            model_.set_channels_status("Waiting to load channels...");
        } else load_channels();
    }
    publish();
}

void PicoClawViewModel::load_channels()
{
    if (client_.busy()) {
        model_.set_channels_status("Wait for the current task to finish.", true);
        publish();
        return;
    }
    channel_load_pending_ = false;
    model_.set_channels_status("Loading channels...");
    if (!submit(PicoClawRuntimeAction::LoadChannels, {}, model_.channels_status()))
        model_.set_channels_status("Could not load channels. Try again.", true);
    publish();
}

bool PicoClawViewModel::edit_channel(std::size_t index)
{
    if (index < model_.channels().size() && model_.channel_draft() &&
        model_.channel_draft()->id != model_.channels()[index].id) cancel_weixin_binding();
    const bool changed = model_.edit_channel(index);
    publish();
    return changed;
}

void PicoClawViewModel::close_channel()
{
    cancel_weixin_binding();
    model_.close_channel();
    publish();
}

bool PicoClawViewModel::set_channel_enabled(bool enabled)
{
    const bool changed = model_.set_channel_enabled(enabled);
    publish();
    return changed;
}

bool PicoClawViewModel::set_channel_field(std::size_t index, std::string value)
{
    const bool changed = model_.set_channel_field(index, std::move(value));
    publish();
    return changed;
}

bool PicoClawViewModel::save_channel()
{
    if (model_.weixin_binding().active()) {
        model_.set_channels_status("Finish or cancel Weixin binding before saving.", true);
        publish();
        return false;
    }
    if (client_.busy()) {
        model_.set_channels_status("Wait for the current task to finish.", true);
        publish();
        return false;
    }
    std::string payload, error;
    if (!model_.channel_save_payload(payload, error)) {
        model_.set_channels_status(std::move(error), true);
        publish();
        return false;
    }
    saving_channel_ = *model_.channel_draft();
    model_.set_channels_status("Saving channel...");
    if (!submit(PicoClawRuntimeAction::SaveChannel, {saving_channel_->id, std::move(payload)}, model_.channels_status())) {
        saving_channel_.reset();
        model_.set_channels_status("Could not save channel. Your changes are still available.", true);
        publish();
        return false;
    }
    return true;
}

bool PicoClawViewModel::start_weixin_binding()
{
    if (model_.screen() != PicoClawScreen::Channels || !model_.channel_draft() ||
        model_.channel_draft()->id != "weixin" || saving_channel_) return false;
    ++binding_generation_;
    binding_deadline_ = binding_clock_() + std::chrono::minutes(5);
    binding_start_pending_ = true;
    if (!model_.begin_weixin_binding()) {
        binding_start_pending_ = false;
        return false;
    }
    submit_weixin_binding_request();
    publish();
    return true;
}

void PicoClawViewModel::cancel_weixin_binding()
{
    if (!model_.weixin_binding().active() && !binding_start_pending_) return;
    ++binding_generation_;
    binding_start_pending_ = false;
    // An in-flight bounded HTTP request may finish in the worker. Generation
    // checks discard it without blocking the UI to join the worker thread.
    model_.cancel_weixin_binding();
    publish();
}

void PicoClawViewModel::submit_weixin_binding_request()
{
    if (binding_client_.busy() || !model_.weixin_binding().active()) return;
    const auto now = binding_clock_();
    if (!binding_start_pending_ && now < next_binding_poll_) return;
    const auto action = binding_start_pending_ ? PicoClawRuntimeAction::WeixinBindStart
                                               : PicoClawRuntimeAction::WeixinBindPoll;
    auto payload = binding_start_pending_ ? model_.weixin_binding_start_payload() : model_.weixin_binding_poll_payload();
    if (!binding_client_.submit(action, {std::move(payload)})) return;
    binding_request_generation_ = binding_generation_;
    binding_start_pending_ = false;
}

bool PicoClawViewModel::poll_weixin_binding()
{
    bool changed = false;
    const auto *draft = model_.channel_draft();
    if (model_.weixin_binding().active() &&
        (model_.screen() != PicoClawScreen::Channels || !draft || draft->id != "weixin")) {
        cancel_weixin_binding();
        changed = true;
    }
    if (model_.weixin_binding().active() && binding_clock_() >= binding_deadline_) {
        ++binding_generation_;
        binding_start_pending_ = false;
        model_.expire_weixin_binding();
        changed = true;
    }
    PicoClawRuntimeResult result;
    if (binding_client_.poll(result)) {
        if (binding_request_generation_ == binding_generation_ && model_.weixin_binding().active()) {
            std::string error;
            if (result.exit_code != 0 || !model_.apply_weixin_binding_result(result.output, error))
                model_.fail_weixin_binding();
            next_binding_poll_ = binding_clock_() + std::chrono::seconds(2);
            changed = true;
        }
    }
    // A refreshed session starts after the stale request has drained. Normal
    // polls wait at least two seconds after each completed status response.
    submit_weixin_binding_request();
    if (changed) publish();
    return changed;
}

bool PicoClawViewModel::back()
{
    cancel_weixin_binding();
    const bool changed = model_.back();
    publish();
    return changed;
}

void PicoClawViewModel::select_profile(std::size_t index)
{
    model_.select_profile(index);
    clear_fetched_models();
    status_text_ = "Review the profile, then save configuration.";
    publish();
}

bool PicoClawViewModel::select_chat_profile(std::size_t index)
{
    if (client_.busy() || chat_model_switch_ != ChatModelSwitch::None) {
        status_text_ = "Wait for the current task to finish before switching models.";
        publish();
        return false;
    }
    if (index >= model_.profiles().size()) {
        status_text_ = "Choose a saved model first.";
        publish();
        return false;
    }
    const auto &settings = model_.settings();
    if (websocket_transport() && !settings.public_mode && settings.host != "127.0.0.1" && settings.host != "localhost" &&
        settings.host != "::1" && settings.host != "[::1]" && settings.host != "0.0.0.0") {
        chat_model_ready_ = false;
        status_text_ = "Remote gateway model switching is unavailable. Use a local host in Settings.";
        publish();
        return false;
    }
    pending_chat_profile_ = model_.profiles()[index];
    switch_websocket_gateway_ = websocket_transport();
    chat_model_switch_ = ChatModelSwitch::Saving;
    chat_model_ready_ = false;
    if (!submit(PicoClawRuntimeAction::SaveConfig,
                {pending_chat_profile_.model_name, pending_chat_profile_.model_id, pending_chat_profile_.api_base,
                 pending_chat_profile_.api_key}, "Switching chat model...")) {
        fail_chat_model_switch("Could not switch models. Select a model again before sending.");
        return false;
    }
    return true;
}

bool PicoClawViewModel::save_profile(PicoClawProfile profile)
{
    if (client_.busy()) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    std::string error;
    if (!model_.update_profile(model_.active_profile_index(), std::move(profile), error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    const auto &active = model_.active_profile();
    saved_configuration_profile_ = active;
    return submit(PicoClawRuntimeAction::SaveConfig,
                  {active.model_name, active.model_id, active.api_base, active.api_key}, "Saving configuration...");
}

bool PicoClawViewModel::add_profile(PicoClawProfile profile)
{
    if (client_.busy()) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    std::string error;
    if (!model_.add_profile(std::move(profile), error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    clear_fetched_models();
    status_text_ = "Model profile added.";
    publish();
    return true;
}

bool PicoClawViewModel::update_profile(std::size_t index, PicoClawProfile profile)
{
    if (client_.busy()) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    const bool updates_chat = index < model_.profiles().size() && matching_profile(model_.profiles()[index], chat_profile_);
    std::string error;
    if (!model_.update_profile(index, std::move(profile), error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    if (updates_chat) {
        const auto &updated = model_.profiles()[index];
        if (!matching_profile(updated, chat_profile_) || updated.api_key != chat_profile_.api_key)
            chat_model_ready_ = false;
        chat_profile_.title = updated.title;
        chat_profile_.provider = updated.provider;
    }
    clear_fetched_models();
    status_text_ = "Model profile updated.";
    publish();
    return true;
}

bool PicoClawViewModel::remove_profile(std::size_t index)
{
    if (client_.busy()) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    const bool removes_chat = index < model_.profiles().size() && matching_profile(model_.profiles()[index], chat_profile_);
    std::string error;
    if (!model_.remove_profile(index, error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    if (removes_chat) {
        chat_model_ready_ = false;
        chat_profile_ = {};
    }
    clear_fetched_models();
    status_text_ = "Model profile removed.";
    publish();
    return true;
}

void PicoClawViewModel::import_models()
{
    (void)submit(PicoClawRuntimeAction::ImportModels, {}, "Importing configured models...");
}

bool PicoClawViewModel::save_settings(PicoClawSettings settings)
{
    if (chat_model_switch_ != ChatModelSwitch::None) {
        status_text_ = "Wait for the model switch to finish before changing settings.";
        publish();
        return false;
    }
    const bool changes_chat_endpoint = websocket_transport() &&
        (settings.host != model_.settings().host || settings.public_mode != model_.settings().public_mode);
    std::string error;
    if (!model_.update_settings(std::move(settings), error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    if (changes_chat_endpoint) chat_model_ready_ = false;
    status_text_ = "UI settings saved.";
    publish();
    return true;
}

bool PicoClawViewModel::send(std::string_view prompt)
{
    if (client_.busy()) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    if (!chat_model_ready()) {
        status_text_ = "Select a chat model successfully before sending.";
        publish();
        return false;
    }
    std::string error;
    if (!model_.begin_prompt(prompt, error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    const auto &messages      = model_.messages();
    const std::string message = messages.size() >= 2U ? messages[messages.size() - 2U].text : std::string(prompt);
    const std::string protocol_frame = protocol_.make_message_send(message);
    PicoClawRuntimeAction action     = PicoClawRuntimeAction::Ask;
    std::vector<std::string> arguments{message, model_.active_conversation().cli_session};
    if (chat_profile_confirmed_) arguments.push_back(chat_profile_.model_name);
    // The FUI Android client speaks Pico Protocol over a local WebSocket.
    // Keep CLI as the default CM0 transport, while allowing deployments with
    // a native gateway to opt into the same streaming protocol.
    if (websocket_transport()) {
        action               = PicoClawRuntimeAction::ProtocolAsk;
        const auto &settings = model_.settings();
        // 0.0.0.0 is a bind address, never a valid client destination.  FUI's
        // public mode binds the service broadly but still connects locally.
        std::string host = settings.public_mode || settings.host.empty() || settings.host == "0.0.0.0"
                               ? "127.0.0.1" : settings.host;
        if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";
        arguments = {
            "ws://" + host + ":" + std::to_string(PicoClawProtocol::gateway_port) +
                std::string(PicoClawProtocol::gateway_path) + "?session_id=" + protocol_.session_id(),
            std::getenv("PICOCLAW_GATEWAY_TOKEN") ? std::getenv("PICOCLAW_GATEWAY_TOKEN") : "picoclaw-android-local",
            protocol_frame};
    }
    if (!submit(action, std::move(arguments), "PicoClaw is thinking...")) {
        model_.complete_prompt("Could not start the PicoClaw request.", false);
        publish();
        return false;
    }
    last_protocol_frame_ = protocol_frame;
    return true;
}

bool PicoClawViewModel::new_chat()
{
    if (client_.busy()) return false;
    const bool changed = model_.new_conversation();
    protocol_.reset_session(model_.active_conversation().id);
    last_protocol_frame_.clear();
    if (changed) status_text_ = "New conversation ready";
    publish();
    return changed;
}

bool PicoClawViewModel::select_conversation(std::size_t index)
{
    if (client_.busy()) return false;
    const bool changed = model_.select_conversation(index);
    if (changed) {
        protocol_.reset_session(model_.active_conversation().id);
        last_protocol_frame_.clear();
        status_text_ = "Conversation restored";
    }
    publish();
    return changed;
}

bool PicoClawViewModel::handle_protocol_frame(std::string_view frame)
{
    PicoClawProtocolEvent event;
    std::string error;
    if (!PicoClawProtocol::parse_event(frame, event, error)) {
        status_text_ = std::move(error);
        publish();
        return false;
    }
    if (!event.session_id.empty() && event.session_id != protocol_.session_id()) {
        status_text_ = "PicoClaw session changed; start a new chat.";
        publish();
        return false;
    }
    switch (event.type) {
        case PicoClawProtocolEventType::MessageUpdate:
            model_.update_prompt(event.content);
            status_text_ = "PicoClaw is thinking...";
            break;
        case PicoClawProtocolEventType::MessageCreate:
            model_.complete_prompt(event.content, true);
            status_text_ = "Ready to chat";
            break;
        case PicoClawProtocolEventType::TypingStart:
            status_text_ = "PicoClaw is thinking...";
            break;
        case PicoClawProtocolEventType::TypingStop:
            break;
        case PicoClawProtocolEventType::Error:
            model_.complete_prompt(event.error.empty() ? event.content : event.error, false);
            status_text_ = event.error.empty() ? "PicoClaw request failed." : event.error;
            break;
        case PicoClawProtocolEventType::Pong:
        case PicoClawProtocolEventType::Unknown:
            break;
    }
    publish();
    return true;
}

void PicoClawViewModel::request_status()
{
    (void)submit(PicoClawRuntimeAction::Status, {}, "Checking runtime...");
}

void PicoClawViewModel::install_runtime()
{
    (void)submit(PicoClawRuntimeAction::Install, {}, "Installing PicoClaw...");
}

void PicoClawViewModel::check_updates()
{
    if (!model_.runtime().update_supported) {
        status_text_ = "Version selection is available on Linux. Use Install / update runtime.";
        publish();
        return;
    }
    (void)submit(PicoClawRuntimeAction::CheckUpdates, {}, "Checking GitHub Releases...");
}

void PicoClawViewModel::select_release(std::size_t index)
{
    if (client_.busy() || !model_.select_release(index)) return;
    const auto *release = model_.selected_release();
    status_text_ = release->installable ? "Selected " + release->tag + ". Tap Install selected to apply."
                                        : "This release has no compatible runtime for this device.";
    publish();
}

void PicoClawViewModel::install_selected_release()
{
    const auto *release = model_.selected_release();
    if (!model_.runtime().update_supported || !release || !release->installable) {
        status_text_ = "Choose a compatible release before installing.";
        publish();
        return;
    }
    (void)submit(PicoClawRuntimeAction::InstallRelease, {release->tag}, "Installing " + release->tag + "...");
}

void PicoClawViewModel::start_gateway()
{
    (void)submit(PicoClawRuntimeAction::GatewayStart, {}, "Starting gateway...");
}

void PicoClawViewModel::stop_gateway()
{
    (void)submit(PicoClawRuntimeAction::GatewayStop, {}, "Stopping gateway...");
}

void PicoClawViewModel::request_logs()
{
    (void)submit(PicoClawRuntimeAction::Logs, {}, "Loading logs...");
}

void PicoClawViewModel::test_connection(std::string api_base, std::string api_key)
{
    if (submit(PicoClawRuntimeAction::TestConnection, {api_base, api_key}, "Testing API connection..."))
        pending_provider_check_ = begin_provider_check(api_base, api_key);
}

void PicoClawViewModel::fetch_models(std::string api_base, std::string api_key)
{
    if (!submit(PicoClawRuntimeAction::FetchModels, {api_base, api_key}, "Fetching model list...")) return;
    pending_provider_check_ = begin_provider_check(api_base, api_key);
    clear_fetched_models();
    pending_fetch_api_base_ = std::move(api_base);
    pending_fetch_api_key_ = std::move(api_key);
    model_fetch_pending_ = true;
}

bool PicoClawViewModel::poll()
{
    poll_provider_checks();
    const bool binding_changed = poll_weixin_binding();
    PicoClawRuntimeResult result;
    if (!client_.poll(result)) {
        busy_.set(client_.busy() ? 1 : 0);
        if (channel_load_pending_ && !client_.busy()) {
            if (model_.screen() == PicoClawScreen::Channels) load_channels();
            else channel_load_pending_ = false;
        }
        return binding_changed;
    }

    if (chat_model_switch_ != ChatModelSwitch::None) return poll_chat_model_switch(result);

    const bool success  = result.exit_code == 0;
    bool refresh_status = false;
    switch (result.action) {
        case PicoClawRuntimeAction::Status:
            model_.apply_runtime_status(result.output);
            if (success && chat_model_ready_ && !chat_profile_confirmed_ &&
                status_matches_profile(result.output, model_.active_profile())) {
                chat_profile_ = model_.active_profile();
                chat_profile_confirmed_ = true;
            }
            if (!success) {
                status_text_ = "Runtime status is unavailable.";
            } else if (!model_.runtime().installed) {
                status_text_ = "Install PicoClaw to begin.";
            } else if (!model_.runtime().network_ready) {
                status_text_ = "Network connection required.";
            } else if (!model_.runtime().model_key_ready) {
                status_text_ = "Add an API key in Models.";
            } else {
                status_text_ = "Ready to chat";
            }
            if (preserve_operation_result_) {
                status_text_ = operation_result_status_;
                if (!success) status_text_ += " Runtime status could not be refreshed.";
                preserve_operation_result_ = false;
            }
            break;
        case PicoClawRuntimeAction::CheckUpdates: {
            std::string error;
            if (success && model_.apply_update_result(result.output, error)) {
                status_text_ = model_.update_message();
            } else {
                if (error.empty()) error = result.output.empty() ? "Could not check GitHub Releases." : result.output;
                model_.set_output(error);
                status_text_ = error;
            }
            break;
        }
        case PicoClawRuntimeAction::InstallRelease:
            model_.set_output(result.output);
            if (success) model_.invalidate_update_status();
            operation_result_status_ = success ? "Selected PicoClaw release installed."
                                             : "Update failed. See activity for the result and recovery details.";
            status_text_ = operation_result_status_;
            preserve_operation_result_ = true;
            refresh_status = true;
            break;
        case PicoClawRuntimeAction::Ask:
            model_.complete_prompt(clean_reply(result.output, success), success);
            status_text_ = success ? "Ready to chat" : "PicoClaw request failed.";
            break;
        case PicoClawRuntimeAction::ProtocolAsk: {
            bool terminal      = false;
            std::size_t offset = 0;
            while (offset < result.output.size()) {
                const auto end = result.output.find('\n', offset);
                const auto frame =
                    std::string_view(result.output)
                        .substr(offset, end == std::string::npos ? result.output.size() - offset : end - offset);
                if (!frame.empty()) {
                    (void)handle_protocol_frame(frame);
                    PicoClawProtocolEvent event;
                    std::string parse_error;
                    if (PicoClawProtocol::parse_event(frame, event, parse_error) &&
                        (event.type == PicoClawProtocolEventType::MessageCreate ||
                         event.type == PicoClawProtocolEventType::Error))
                        terminal = true;
                }
                if (end == std::string::npos) break;
                offset = end + 1U;
            }
            if (!success && !terminal) {
                model_.complete_prompt(result.output, false);
                status_text_ = "PicoClaw WebSocket request failed.";
            }
            break;
        }
        case PicoClawRuntimeAction::SaveConfig:
            model_.set_output(result.output);
            status_text_ = success ? (model_.runtime().gateway_running
                                        ? "Configuration saved. Restart the gateway to apply the selected model."
                                        : "Configuration saved.")
                                   : "Could not save configuration.";
            if (success) {
                // A running WebSocket gateway still owns its startup model.
                // Require the verified switching sequence before another send.
                if (websocket_transport()) chat_model_ready_ = false;
                else {
                    chat_profile_ = saved_configuration_profile_;
                    chat_profile_confirmed_ = true;
                    chat_model_ready_ = true;
                }
                operation_result_status_ = status_text_;
                preserve_operation_result_ = true;
            }
            saved_configuration_profile_ = {};
            refresh_status = success;
            break;
        case PicoClawRuntimeAction::ImportModels: {
            std::string error;
            if (success && model_.merge_profiles(result.output, error))
                status_text_ = "Configured models imported. Existing model settings were preserved.";
            else
                status_text_ = error.empty() ? "Could not import configured models. Existing API keys were preserved."
                                            : std::move(error);
            break;
        }
        case PicoClawRuntimeAction::LoadChannels: {
            channel_load_pending_ = false;
            std::string error;
            if (!success || !model_.apply_channels_result(result.output, error))
                model_.set_channels_status(error.empty() ? "Could not load channels. Your changes were preserved."
                                                        : std::move(error), true);
            status_text_ = model_.channels_status();
            // Channel responses contain private credentials. Never expose the
            // payload, including helper failures, through runtime output.
            break;
        }
        case PicoClawRuntimeAction::SaveChannel:
            if (success && saving_channel_) {
                const bool restart = saving_channel_->id == "weixin" && model_.runtime().gateway_running;
                model_.complete_channel_save(*saving_channel_);
                restart_after_weixin_save_ = restart;
                if (restart) {
                    model_.set_channels_status("Weixin saved. Restarting the gateway to apply the account...");
                    (void)submit(PicoClawRuntimeAction::GatewayStop, {}, model_.channels_status());
                }
            } else {
                model_.set_channels_status("Could not save channel. Your changes are still available.", true);
            }
            saving_channel_.reset();
            status_text_ = model_.channels_status();
            break;
        case PicoClawRuntimeAction::WeixinBindStart:
        case PicoClawRuntimeAction::WeixinBindPoll:
            // Binding uses a dedicated client. Never publish a misplaced
            // private result through foreground activity or status output.
            break;
        case PicoClawRuntimeAction::Install:
            model_.set_output(result.output);
            status_text_   = success ? "PicoClaw installed." : "PicoClaw installation failed.";
            refresh_status = success;
            break;
        case PicoClawRuntimeAction::GatewayStart:
            model_.set_output(result.output);
            if (restart_after_weixin_save_) {
                restart_after_weixin_save_ = false;
                status_text_ = success ? "Weixin saved and gateway restarted." : "Weixin saved, but the gateway could not restart.";
                model_.set_channels_status(status_text_, !success);
            } else {
                status_text_ = success ? "Gateway started." : "Gateway could not start.";
            }
            refresh_status = success;
            break;
        case PicoClawRuntimeAction::GatewayStop:
            model_.set_output(result.output);
            if (restart_after_weixin_save_ && success) {
                status_text_ = "Gateway stopped. Starting it with the new Weixin account...";
                (void)submit(PicoClawRuntimeAction::GatewayStart, {}, status_text_);
            } else {
                if (restart_after_weixin_save_) restart_after_weixin_save_ = false;
                status_text_ = success ? "Gateway stopped." : "Gateway could not stop.";
                if (!success) model_.set_channels_status("Weixin was saved, but the gateway could not stop for restart.", true);
                refresh_status = success;
            }
            break;
        case PicoClawRuntimeAction::Logs:
            model_.set_output(result.output);
            status_text_ = success ? "Logs updated." : "Logs are unavailable.";
            break;
        case PicoClawRuntimeAction::TestConnection:
            if (pending_provider_check_) complete_provider_check(result, *pending_provider_check_);
            pending_provider_check_.reset();
            // Provider errors can echo request credentials. Never publish the
            // raw connection response in status text or the runtime log.
            status_text_ = success ? "API connection successful." : "API connection failed. Check the API base and key.";
            break;
        case PicoClawRuntimeAction::FetchModels: {
            if (pending_provider_check_) complete_provider_check(result, *pending_provider_check_);
            pending_provider_check_.reset();
            clear_fetched_models();
            if (success && model_fetch_pending_) {
                std::size_t offset = 0;
                while (offset < result.output.size()) {
                    const auto end = result.output.find('\n', offset);
                    std::string line = result.output.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("model=", 0) == 0 && line.size() > 6U) fetched_models_.push_back(line.substr(6));
                    if (end == std::string::npos) break;
                    offset = end + 1U;
                }
                fetched_api_base_ = std::move(pending_fetch_api_base_);
                fetched_api_key_ = std::move(pending_fetch_api_key_);
                fetched_models_valid_ = true;
            }
            status_text_ = fetched_models_valid_ && !fetched_models_.empty()
                               ? "Fetched " + std::to_string(fetched_models_.size()) + " models."
                               : (fetched_models_valid_ ? "No models returned by the provider." : "Could not fetch models.");
            pending_fetch_api_base_.clear();
            pending_fetch_api_key_.clear();
            model_fetch_pending_ = false;
            break;
        }
    }
    publish();
    if (refresh_status) {
        if (preserve_operation_result_)
            (void)submit(PicoClawRuntimeAction::Status, {}, operation_result_status_);
        else
            request_status();
    }
    if (channel_load_pending_ && !client_.busy()) {
        if (model_.screen() == PicoClawScreen::Channels) load_channels();
        else channel_load_pending_ = false;
    }
    return true;
}

void PicoClawViewModel::fail_chat_model_switch(std::string_view message)
{
    chat_model_switch_ = ChatModelSwitch::None;
    chat_model_ready_ = false;
    pending_chat_profile_ = {};
    status_text_ = message;
    model_.set_output(status_text_);
    publish();
}

bool PicoClawViewModel::poll_chat_model_switch(const PicoClawRuntimeResult &result)
{
    if (result.exit_code != 0) {
        fail_chat_model_switch("Model switch failed. Check the configuration and select a model again before sending.");
        return true;
    }
    PicoClawRuntimeAction next_action = PicoClawRuntimeAction::Status;
    switch (chat_model_switch_) {
        case ChatModelSwitch::Saving:
            if (result.action != PicoClawRuntimeAction::SaveConfig) {
                fail_chat_model_switch("Model switch was interrupted. Select a model again before sending.");
                return true;
            }
            preserve_operation_result_ = false;
            chat_model_switch_ = switch_websocket_gateway_ ? ChatModelSwitch::StoppingGateway : ChatModelSwitch::Verifying;
            next_action = switch_websocket_gateway_ ? PicoClawRuntimeAction::GatewayStop : PicoClawRuntimeAction::Status;
            break;
        case ChatModelSwitch::StoppingGateway:
            if (result.action != PicoClawRuntimeAction::GatewayStop || runtime_field(result.output, "gateway") != "stopped") {
                fail_chat_model_switch("The gateway could not stop. Select a model again before sending.");
                return true;
            }
            model_.apply_runtime_status(result.output);
            chat_model_switch_ = ChatModelSwitch::StartingGateway;
            next_action = PicoClawRuntimeAction::GatewayStart;
            break;
        case ChatModelSwitch::StartingGateway:
            if (result.action != PicoClawRuntimeAction::GatewayStart || runtime_field(result.output, "gateway") != "running") {
                fail_chat_model_switch("The gateway could not start with the selected model. Select a model again before sending.");
                return true;
            }
            model_.apply_runtime_status(result.output);
            chat_model_switch_ = ChatModelSwitch::Verifying;
            break;
        case ChatModelSwitch::Verifying:
            model_.apply_runtime_status(result.output);
            if (result.action != PicoClawRuntimeAction::Status || !status_matches_profile(result.output, pending_chat_profile_) ||
                runtime_field(result.output, "installed") != "yes" || runtime_field(result.output, "config") != "ready" ||
                runtime_field(result.output, "model_key") != "ready" ||
                (switch_websocket_gateway_ && runtime_field(result.output, "gateway") != "running")) {
                fail_chat_model_switch("The selected model is not ready. Check its API key and select it again before sending.");
                return true;
            }
            chat_profile_ = pending_chat_profile_;
            pending_chat_profile_ = {};
            chat_profile_confirmed_ = true;
            chat_model_ready_ = true;
            chat_model_switch_ = ChatModelSwitch::None;
            status_text_ = "Chat model ready: " + chat_profile_.title;
            publish();
            return true;
        case ChatModelSwitch::None:
            return false;
    }
    if (!submit(next_action, {}, "Switching chat model..."))
        fail_chat_model_switch("Model switch was interrupted. Select a model again before sending.");
    return true;
}

bool PicoClawViewModel::submit(PicoClawRuntimeAction action, std::vector<std::string> arguments,
                               std::string_view status)
{
    if (!client_.submit(action, std::move(arguments))) {
        status_text_ = "Wait for the current task to finish.";
        publish();
        return false;
    }
    status_text_ = status;
    publish();
    return true;
}

std::string PicoClawViewModel::clean_reply(std::string_view output, bool success)
{
    // --no-color still prints PicoClaw's six-line logo and the CLI reply marker.
    // Strip terminal decoration before passing the text to an LVGL label.
    static const std::regex ansi_csi("\x1b\\[[0-?]*[ -/]*[@-~]");
    static const std::regex console_log("^[0-9]{2}:[0-9]{2}:[0-9]{2} (TRC|DBG|INF|WRN|ERR|FTL|PNC) ");
    std::istringstream input{std::regex_replace(std::string(output), ansi_csi, "")};
    std::string line;
    std::string reply;
    bool response_started = false;
    int banner_lines_left = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!response_started) {
            if (banner_lines_left > 0) {
                --banner_lines_left;
                continue;
            }
            if (reply.empty() && line.rfind("██████╗ ██╗ ██████╗ ██████╗", 0) == 0) {
                banner_lines_left = 5;
                continue;
            }
            if (line.rfind("🦞 ", 0) == 0) {
                response_started = true;
                reply.clear();
                line.erase(0, std::string_view("🦞 ").size());
            } else if (line.rfind("time=", 0) == 0 || line.rfind("level=", 0) == 0 ||
                       line.rfind("[INFO]", 0) == 0 || std::regex_search(line, console_log)) {
                continue;
            }
        }
        if (reply.empty() && line.find_first_not_of(" \t") == std::string::npos) continue;
        if (!reply.empty()) reply.push_back('\n');
        reply += line;
        // Plain-output runtimes have no lobster marker; once their answer
        // starts, retain log-like examples and box drawing inside the answer.
        if (success && !line.empty()) response_started = true;
    }
    while (!reply.empty() && std::isspace(static_cast<unsigned char>(reply.back()))) reply.pop_back();
    if (reply.empty()) reply = success ? "PicoClaw returned an empty response." : "Request failed.";
    if (!success && reply.rfind("Error:", 0) != 0) reply = "Error: " + reply;
    return reply;
}

void PicoClawViewModel::publish()
{
    if (!model_.history_error().empty()) status_text_ = model_.history_error();
    screen_.set(static_cast<std::int32_t>(model_.screen()));
    busy_.set(client_.busy() ? 1 : 0);
    status_.set(status_text_);
    profile_.set(chat_profile_.title);
}
