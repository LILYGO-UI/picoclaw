#include "test_environment.hpp"
#include "pages/picoclaw/picoclaw_view_model.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

PicoClawRuntimeResult unused_provider_runner(const PicoClawRuntimeRequest &request)
{
    return {request.action, 1, "connection=failed\n"};
}

template <typename Predicate>
bool wait_for_state(PicoClawViewModel &model, Predicate ready)
{
    for (int attempt = 0; attempt < 500; ++attempt) {
        (void)model.poll();
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool wait_for_poll(PicoClawViewModel &view_model)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (view_model.poll()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void test_update_selection_and_install_failure_recovery()
{
    TestEnvironment environment;
    std::vector<PicoClawRuntimeRequest> requests;
    bool incomplete = false;
    bool install_success = false;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        PicoClawRuntimeResult result{request.action, 0, {}};
        if (request.action == PicoClawRuntimeAction::Status) {
            result.output = "installed=yes\nversion=picoclaw 0.3.0\ngateway=running\nnetwork=ready\n"
                            "config=ready\nmodel_key=ready\nupdate_supported=yes\n";
        } else if (request.action == PicoClawRuntimeAction::CheckUpdates) {
            result.output = "updates=ready\nlatest_stable=v0.3.1\nupdate_status=available\n"
                            "update_message=A new version is available.\n"
                            "release=nightly\tNightly\tnightly\t2026-09-14\tyes\n"
                            "release=v0.3.1\tStable\tstable\t2026-09-12\tyes\n"
                            "release=v0.1.0\tOld release\tstable\t2026-01-01\tno\n";
            if (!incomplete) result.output += "end_releases=yes\n";
        } else if (request.action == PicoClawRuntimeAction::InstallRelease) {
            result.exit_code = install_success ? 0 : 4;
            result.output = install_success ? "Installed nightly successfully.\n"
                                            : "ERROR: new gateway failed to start.\nRollback failed: manual recovery required.\n";
        }
        return result;
    });
    PicoClawViewModel model(client);
    model.request_status();
    assert(wait_for_poll(model));
    model.check_updates();
    assert(wait_for_poll(model));
    assert(model.releases().size() == 3 && model.selected_release()->tag == "v0.3.1");
    model.select_release(2);
    const auto request_count = requests.size();
    model.install_selected_release();
    assert(requests.size() == request_count && !model.busy());
    model.select_release(0);
    incomplete = true;
    model.check_updates();
    assert(wait_for_poll(model));
    assert(model.releases().size() == 3 && model.selected_release()->tag == "nightly");
    assert(std::string(lv_subject_get_string(model.status_subject())).find("incomplete") != std::string::npos);
    model.install_selected_release();
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(requests[requests.size() - 2].action == PicoClawRuntimeAction::InstallRelease);
    assert(requests[requests.size() - 2].arguments == std::vector<std::string>{"nightly"});
    assert(requests.back().action == PicoClawRuntimeAction::Status);
    assert(model.runtime().output.find("Rollback failed") != std::string::npos);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("Update failed") != std::string::npos);
    assert(model.update_status() == "available");
    install_success = true;
    model.install_selected_release();
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(model.update_status() == "unknown");
    assert(model.update_message().find("Runtime changed") != std::string::npos);
    assert(model.releases().size() == 3 && model.selected_release()->tag == "nightly");
}

void test_docker_retains_image_install()
{
    TestEnvironment environment;
    std::vector<PicoClawRuntimeAction> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request.action);
        return PicoClawRuntimeResult{request.action, 0, "installed=yes\nupdate_supported=no\n"};
    });
    PicoClawViewModel model(client);
    model.request_status();
    assert(wait_for_poll(model));
    model.check_updates();
    assert(requests.size() == 1 && !model.busy());
    model.install_runtime();
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(requests[1] == PicoClawRuntimeAction::Install);
}

void test_registry_operations_and_busy_protection()
{
    TestEnvironment environment;
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, "installed=yes\n"};
    });
    PicoClawViewModel model(client, unused_provider_runner);
    model.show_models();
    assert(model.screen() == PicoClawScreen::Models);
    assert(lv_subject_get_int(model.screen_subject()) == static_cast<int>(PicoClawScreen::Models));
    model.show_logs();
    assert(lv_subject_get_int(model.screen_subject()) == static_cast<int>(PicoClawScreen::Logs));
    assert(model.back());
    assert(model.screen() == PicoClawScreen::Settings);
    assert(lv_subject_get_int(model.screen_subject()) == static_cast<int>(PicoClawScreen::Settings));
    assert(model.back());
    assert(model.screen() == PicoClawScreen::Chat);
    assert(lv_subject_get_int(model.screen_subject()) == static_cast<int>(PicoClawScreen::Chat));
    assert(!model.back());
    const PicoClawProfile added{"Saved provider", "saved-provider", "openai/model", "https://example.test/v1", "secret"};
    const auto previous_active = model.active_profile_index();
    assert(model.add_profile(added));
    assert(model.profiles().size() == 8 && model.active_profile_index() == previous_active && !model.busy());
    PicoClawProfile changed = added;
    changed.api_key = "replacement";
    assert(model.update_profile(7, changed));
    assert(model.profiles()[7].api_key == "replacement" && !model.busy());
    model.request_status();
    assert(model.busy());
    assert(!model.add_profile(added));
    assert(!model.update_profile(7, added));
    assert(!model.remove_profile(7));
    assert(!model.save_profile(added));
    assert(model.profiles().size() == 8 && model.profiles()[7].api_key == "replacement");
    assert(wait_for_poll(model));
    assert(model.remove_profile(7));
    assert(model.profiles().size() == 7 && !model.busy());
}

void test_model_import_keeps_keys_private_and_save_retains_restart_notice()
{
    TestEnvironment environment;
    bool valid_import = true;
    bool successful_import = true;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        if (request.action == PicoClawRuntimeAction::Status)
            return PicoClawRuntimeResult{request.action, 0,
                "installed=yes\ngateway=running\nnetwork=ready\nconfig=ready\nmodel_key=ready\n"};
        if (request.action == PicoClawRuntimeAction::ImportModels)
            return PicoClawRuntimeResult{request.action, successful_import ? 0 : 1,
                valid_import ? "PICOCLAW_MODELS 1 1\n\"Imported\" \"imported\" \"openai/imported\" "
                               "\"https://example.test/v1\" \"private-import-secret\"\n"
                             : "broken export containing private-import-secret"};
        return PicoClawRuntimeResult{request.action, 0, "Safe runtime output"};
    });
    PicoClawViewModel model(client);
    model.request_logs();
    assert(wait_for_poll(model));
    const auto previous_output = model.runtime().output;
    model.import_models();
    assert(wait_for_poll(model));
    assert(model.profiles().size() == 8 && model.profiles()[7].api_key == "private-import-secret");
    assert(model.runtime().output == previous_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("private-import-secret") == std::string::npos);
    valid_import = false;
    model.import_models();
    assert(wait_for_poll(model));
    assert(model.profiles().size() == 8 && model.runtime().output == previous_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("private-import-secret") == std::string::npos);
    successful_import = false;
    model.import_models();
    assert(wait_for_poll(model));
    assert(model.runtime().output == previous_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("private-import-secret") == std::string::npos);

    model.request_status();
    assert(wait_for_poll(model) && model.runtime().gateway_running);
    auto selected = model.active_profile();
    selected.api_key = "replacement";
    assert(model.save_profile(selected));
    assert(wait_for_poll(model));
    assert(std::string(lv_subject_get_string(model.status_subject())).find("Restart the gateway") != std::string::npos);
    assert(wait_for_poll(model));
    assert(std::string(lv_subject_get_string(model.status_subject())).find("Restart the gateway") != std::string::npos);
}

void test_chat_model_switch_uses_selected_alias_and_preserves_conversation()
{
    TestEnvironment environment;
    PicoClawProfile configured{"DeepSeek Chat", "deepseek-chat", "deepseek/deepseek-chat", "https://api.deepseek.com/v1", "old-key"};
    std::vector<PicoClawRuntimeRequest> requests;
    bool fail_save = false;
    bool mismatch_status = false;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        if (request.action == PicoClawRuntimeAction::SaveConfig) {
            if (fail_save) return PicoClawRuntimeResult{request.action, 1, "Save failed"};
            configured.model_name = request.arguments[0];
            configured.model_id = request.arguments[1];
            configured.api_base = request.arguments[2];
            configured.api_key = request.arguments[3];
        }
        if (request.action == PicoClawRuntimeAction::Status)
            return PicoClawRuntimeResult{request.action, 0,
                "installed=yes\ngateway=running\nconfig=ready\nmodel_key=ready\nnetwork=ready\nmodel_name=" +
                (mismatch_status ? "unexpected" : configured.model_name) + "\nmodel_id=" + configured.model_id +
                "\napi_base=" + configured.api_base + "\n"};
        return PicoClawRuntimeResult{request.action, 0, "Completed"};
    });
    PicoClawViewModel model(client);
    model.request_status();
    assert(wait_for_poll(model));
    const auto original_index = model.chat_profile_index();
    assert(model.send("First question"));
    assert(!model.select_chat_profile(2));
    assert(wait_for_poll(model));
    const auto session = model.protocol_session_id();
    const auto transcript_size = model.messages().size();
    const PicoClawProfile added{"Second model", "second-model", "openai/second", "https://second.test/v1", "new-key"};
    assert(model.add_profile(added));
    model.select_profile(3); // Editing settings must not select a chat model.
    assert(model.chat_profile_index() == original_index);
    assert(model.select_chat_profile(7));
    assert(model.busy() && !model.chat_model_ready() && model.chat_profile_index() == original_index);
    auto interrupted_settings = model.settings();
    interrupted_settings.host = "other-device.local";
    assert(!model.save_settings(interrupted_settings));
    assert(model.settings().host == "127.0.0.1");
    assert(!model.send("Draft must stay unsent"));
    assert(model.messages().size() == transcript_size && model.protocol_session_id() == session);
    assert(wait_for_poll(model));
    assert(!model.chat_model_ready() && model.busy());
    assert(wait_for_poll(model));
    assert(model.chat_model_ready() && model.chat_profile_index() == 7);
    assert(std::string(lv_subject_get_string(model.status_subject())) == "Ready to chat");
    assert(model.protocol_session_id() == session && model.messages().size() == transcript_size);
    assert(model.send("Continue with second model"));
    assert(wait_for_poll(model));
    assert(requests.back().action == PicoClawRuntimeAction::Ask && requests.back().arguments.size() == 3);
    assert(requests.back().arguments[2] == "second-model");
    for (const auto &request : requests)
        assert(request.action != PicoClawRuntimeAction::GatewayStop && request.action != PicoClawRuntimeAction::GatewayStart);

    fail_save = true;
    assert(model.select_chat_profile(0));
    assert(wait_for_poll(model));
    assert(model.chat_profile_index() == 7 && !model.chat_model_ready());
    const auto messages_before_failure = model.messages().size();
    assert(!model.send("Must not use the previous/default model"));
    assert(model.messages().size() == messages_before_failure);
    fail_save = false;
    mismatch_status = true;
    assert(model.select_chat_profile(0));
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(!model.chat_model_ready() && model.chat_profile_index() == 7);
    mismatch_status = false;
    assert(model.select_chat_profile(7));
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(model.chat_model_ready() && model.chat_profile_index() == 7);
    auto renamed = model.profiles()[7];
    renamed.title = "Renamed selected model";
    assert(model.update_profile(7, renamed));
    assert(model.chat_profile().title == renamed.title && model.chat_model_ready());
    renamed.api_key = "edited-key";
    assert(model.update_profile(7, renamed));
    assert(!model.chat_model_ready() && !model.send("Edited keys need to be applied"));
    assert(model.select_chat_profile(7));
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    assert(model.chat_model_ready());
    assert(model.remove_profile(7));
    assert(!model.chat_model_ready() && model.chat_profile_index() == model.profiles().size());
    assert(model.chat_profile().title.empty());
}

void test_websocket_model_switch_restarts_and_verifies_gateway()
{
    for (const auto failed_stage : {PicoClawRuntimeAction::Ask, PicoClawRuntimeAction::GatewayStop,
                                   PicoClawRuntimeAction::GatewayStart, PicoClawRuntimeAction::Status}) {
        TestEnvironment environment;
        environment.set("PICOCLAW_TRANSPORT", "websocket");
        std::vector<PicoClawRuntimeAction> requests;
        PicoClawProfile configured;
        bool running = true;
        PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
            requests.push_back(request.action);
            if (request.action == failed_stage && failed_stage != PicoClawRuntimeAction::Ask)
                return PicoClawRuntimeResult{request.action, 1, "Controlled failure"};
            if (request.action == PicoClawRuntimeAction::SaveConfig)
                configured = {"Selected", request.arguments[0], request.arguments[1], request.arguments[2], request.arguments[3]};
            if (request.action == PicoClawRuntimeAction::GatewayStop) running = false;
            if (request.action == PicoClawRuntimeAction::GatewayStart) running = true;
            if (request.action == PicoClawRuntimeAction::ProtocolAsk)
                return PicoClawRuntimeResult{request.action, 0,
                    "{\"type\":\"message.create\",\"payload\":{\"content\":\"Selected model replied\"}}"};
            return PicoClawRuntimeResult{request.action, 0,
                std::string("installed=yes\nconfig=ready\nmodel_key=ready\ngateway=") + (running ? "running" : "stopped") +
                "\nmodel_name=" + configured.model_name + "\nmodel_id=" + configured.model_id + "\napi_base=" + configured.api_base + "\n"};
        });
        PicoClawViewModel model(client);
        const auto previous_index = model.chat_profile_index();
        assert(model.select_chat_profile(2));
        while (model.busy()) {
            assert(!model.chat_model_ready());
            assert(!model.send("Not before the new gateway is ready"));
            assert(wait_for_poll(model));
        }
        if (failed_stage == PicoClawRuntimeAction::Ask) {
            assert((requests == std::vector<PicoClawRuntimeAction>{PicoClawRuntimeAction::SaveConfig,
                PicoClawRuntimeAction::GatewayStop, PicoClawRuntimeAction::GatewayStart, PicoClawRuntimeAction::Status}));
            assert(model.chat_model_ready() && model.chat_profile_index() == 2);
            assert(model.send("Use selected model through the new gateway"));
            assert(wait_for_poll(model));
            assert(requests.back() == PicoClawRuntimeAction::ProtocolAsk);
            assert(model.messages().back().text == "Selected model replied");
        } else {
            assert(!model.chat_model_ready() && model.chat_profile_index() == previous_index);
            assert(!model.send("Must remain blocked"));
            assert(model.messages().empty() && requests.back() == failed_stage);
        }
    }
}

void test_remote_websocket_switch_is_rejected_without_changing_local_configuration()
{
    TestEnvironment environment;
    environment.set("PICOCLAW_HOME", environment.root + "/runtime");
    environment.set("PICOCLAW_TRANSPORT", "websocket");
    std::vector<PicoClawRuntimeAction> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request.action);
        return PicoClawRuntimeResult{request.action, 0, "OK"};
    });
    PicoClawViewModel model(client);
    auto settings = model.settings();
    settings.host = "other-device.local";
    assert(model.save_settings(settings));
    assert(!model.select_chat_profile(2));
    assert(!model.busy() && !model.chat_model_ready() && requests.empty());
    assert(!model.send("Never claim to select a remote model by changing local files"));
    assert(model.messages().empty());
}

void test_fetched_models_are_scoped_to_successful_request_credentials()
{
    TestEnvironment environment;
    const std::string first_base = "https://first.test/v1";
    const std::string first_key = "first-private-key";
    const std::string second_base = "https://second.test/v1";
    const std::string second_key = "second-private-key";
    std::vector<PicoClawRuntimeRequest> requests;
    bool fetch_success = true;
    bool empty_response = false;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        if (request.action == PicoClawRuntimeAction::FetchModels)
            return PicoClawRuntimeResult{request.action, fetch_success ? 0 : 1,
                empty_response ? std::string{} : "model=provider-one\r\nmodel=provider-two\nprovider echoed " + request.arguments[1]};
        if (request.action == PicoClawRuntimeAction::TestConnection)
            return PicoClawRuntimeResult{request.action, 1, "Provider error contains " + request.arguments[1]};
        return PicoClawRuntimeResult{request.action, 0, "Safe runtime output"};
    });
    PicoClawViewModel model(client, unused_provider_runner);
    model.request_logs();
    assert(wait_for_poll(model));
    const auto safe_output = model.runtime().output;
    assert(!model.fetched_models_match(first_base, first_key));
    model.fetch_models(first_base, first_key);
    assert(model.busy() && !model.fetched_models_match(first_base, first_key));
    model.fetch_models(second_base, second_key); // A rejected fetch must not replace the in-flight origin.
    model.show_settings();
    model.select_profile(2);
    model.show_models();
    assert(wait_for_poll(model));
    assert(requests.size() == 2 && requests.back().arguments == std::vector<std::string>({first_base, first_key}));
    assert(model.fetched_models() == std::vector<std::string>({"provider-one", "provider-two"}));
    assert(model.fetched_models_match(first_base, first_key));
    assert(!model.fetched_models_match(second_base, first_key));
    assert(!model.fetched_models_match(first_base, second_key));
    assert(!model.fetched_models_match(second_base, second_key));
    assert(model.runtime().output == safe_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find(first_key) == std::string::npos);

    // Successful requests from another page can replace the cache only under
    // their own endpoint and credentials, including a valid empty response.
    empty_response = true;
    model.fetch_models(second_base, second_key);
    assert(!model.fetched_models_match(first_base, first_key));
    assert(wait_for_poll(model));
    assert(model.fetched_models().empty() && model.fetched_models_match(second_base, second_key));
    assert(!model.fetched_models_match(first_base, first_key));

    // An unrelated busy operation must leave the completed cache intact.
    model.test_connection(first_base, first_key);
    model.fetch_models(first_base, first_key);
    assert(wait_for_poll(model));
    assert(requests.back().action == PicoClawRuntimeAction::TestConnection);
    assert(model.fetched_models_match(second_base, second_key));
    assert(model.runtime().output == safe_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find(first_key) == std::string::npos);

    empty_response = false;
    fetch_success = false;
    model.fetch_models(first_base, first_key);
    assert(wait_for_poll(model));
    assert(model.fetched_models().empty());
    assert(!model.fetched_models_match(first_base, first_key));
    assert(!model.fetched_models_match(second_base, second_key));
    assert(model.runtime().output == safe_output);
    assert(std::string(lv_subject_get_string(model.status_subject())).find(first_key) == std::string::npos);
}

void test_provider_status_tracks_credentials_and_balance_failures()
{
    TestEnvironment environment;
    int exit_code = 0;
    std::string output = "connection=ok\nquota=available\nbalance=USD\t12.50\n";
    std::vector<PicoClawRuntimeRequest> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        return PicoClawRuntimeResult{request.action, exit_code, output};
    });
    PicoClawViewModel model(client, unused_provider_runner);
    PicoClawProfile first{"First account", "first-account", "first-model", "https://provider.test/v1", "first-key"};
    PicoClawProfile second{"Second account", "second-account", "second-model", first.api_base, "second-key"};
    const auto first_index = model.profiles().size();
    assert(model.add_profile(first) && model.add_profile(second));
    assert(model.provider_status(first).connection == PicoClawConnectionState::Unknown);
    const auto before = model.revision();
    model.test_connection(first.api_base, first.api_key);
    assert(model.provider_status(first).connection == PicoClawConnectionState::Checking);
    assert(model.revision() > before);
    model.test_connection(second.api_base, second.api_key);
    model.select_profile(2);
    model.show_chat();
    assert(wait_for_poll(model));
    model.show_settings();
    assert(requests.size() == 1 && requests[0].arguments[1] == first.api_key);
    assert(model.provider_status(first).connection == PicoClawConnectionState::Connected);
    assert(model.provider_status(first).available == "Available: 12.50 USD");
    assert(model.provider_status(second).connection == PicoClawConnectionState::Unknown);

    // Rename/model changes share the account; changing credentials loses the old status.
    auto edited = first;
    edited.title = "Renamed";
    edited.model_id = "other-model";
    assert(model.update_profile(first_index, edited));
    assert(model.provider_status(model.profiles()[first_index]).available == "Available: 12.50 USD");
    edited.api_key = "replacement-key";
    assert(model.update_profile(first_index, edited));
    assert(model.provider_status(model.profiles()[first_index]).connection == PicoClawConnectionState::Unknown);
    output = "connection=ok\nquota=available\nbalance=CNY\t0.00\nmodel=other-model\n";
    model.fetch_models(edited.api_base, edited.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(edited).available == "Available: 0.00 CNY");
    assert(model.fetched_models() == std::vector<std::string>{"other-model"});

    output = "connection=ok\nquota=unlimited\n";
    model.test_connection(edited.api_base, edited.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(edited).connection == PicoClawConnectionState::Connected);
    assert(model.provider_status(edited).available == "Available: Unlimited");

    output = "connection=ok\nquota=unavailable\nprovider echoed replacement-key\n";
    model.test_connection(edited.api_base, edited.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(edited).connection == PicoClawConnectionState::Connected);
    assert(model.provider_status(edited).available == "Available: Unavailable");
    output = "connection=ok\nquota=available\nbalance=USD\treplacement-key\nbalance=USD\tNaN\n";
    model.test_connection(edited.api_base, edited.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(edited).available == "Available: Unavailable");
    exit_code = 1;
    model.test_connection(edited.api_base, edited.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(edited).connection == PicoClawConnectionState::Failed);
    assert(model.provider_status(edited).available == "Available: --");
    assert(model.runtime().output.find(edited.api_key) == std::string::npos);
    assert(std::string(lv_subject_get_string(model.status_subject())).find(edited.api_key) == std::string::npos);
}

void save_test_profiles(const std::vector<PicoClawProfile> &profiles)
{
    PicoClawModel saved;
    std::string error;
    while (saved.profiles().size() > 1) assert(saved.remove_profile(saved.profiles().size() - 1, error));
    assert(saved.update_profile(0, profiles.front(), error));
    for (std::size_t i = 1; i < profiles.size(); ++i) assert(saved.add_profile(profiles[i], error));
}

void test_models_entry_refreshes_accounts_without_blocking_foreground_work()
{
    TestEnvironment environment;
    const PicoClawProfile first{"First", "first", "model-a", "https://provider.test/v1", "first-private-key"};
    const PicoClawProfile shared{"Shared", "shared", "model-b", first.api_base, first.api_key};
    const PicoClawProfile second{"Second", "second", "model-c", first.api_base, "second-private-key"};
    const PicoClawProfile local{"Local", "local", "ollama/model", "http://127.0.0.1:11434/v1", "", "ollama"};
    const PicoClawProfile empty{"No key", "no-key", "model", "https://unconfigured.test/v1", ""};
    save_test_profiles({first, shared, second, local, empty});
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> started{0};
    std::mutex mutex;
    std::vector<PicoClawRuntimeRequest> background_requests;
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, "Safe foreground output"};
    });
    PicoClawViewModel model(client, [&](const PicoClawRuntimeRequest &request) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            background_requests.push_back(request);
        }
        ++started;
        gate.wait();
        if (request.arguments[1] == second.api_key)
            return PicoClawRuntimeResult{request.action, 1, "Provider echoed " + second.api_key};
        return PicoClawRuntimeResult{request.action, 0, request.arguments[1].empty() ? "quota=unmetered\n"
            : "connection=ok\nquota=available\nbalance=USD\t23.45\n"};
    });
    model.request_logs();
    model.show_models();
    assert(wait_for_poll(model));
    assert(wait_for_state(model, [&] { return started == 1; }));
    assert(!model.busy());
    assert(lv_subject_get_int(model.busy_subject()) == 0);
    const auto status = std::string(lv_subject_get_string(model.status_subject()));
    assert(status == "Logs updated.");
    for (const auto &profile : {first, shared, second, local})
        assert(model.provider_status(profile).connection == PicoClawConnectionState::Checking);
    assert(model.provider_status(empty).connection == PicoClawConnectionState::Unknown);
    auto edited = first;
    edited.title = "Renamed while checking";
    assert(model.update_profile(0, edited));
    model.show_chat();
    assert(model.send("Chat while the provider query is still pending."));
    assert(wait_for_poll(model));
    assert(!model.messages().back().pending);
    assert(model.new_chat());
    model.show_models();
    model.show_models();
    assert(started == 1 && !model.busy());
    release.set_value();
    assert(wait_for_state(model, [&] { return model.provider_status(local).connection == PicoClawConnectionState::Connected; }));
    assert(model.provider_status(first).available == "Available: 23.45 USD");
    assert(model.provider_status(shared).available == "Available: 23.45 USD");
    assert(model.provider_status(second).connection == PicoClawConnectionState::Failed);
    assert(model.provider_status(local).available == "Available: Not metered");
    assert(model.runtime().output == "Safe foreground output");
    assert(std::string(lv_subject_get_string(model.status_subject())).find("private-key") == std::string::npos);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(background_requests.size() == 3);
        assert(background_requests[0].arguments == std::vector<std::string>({first.api_base, first.api_key}));
        assert(background_requests[1].arguments[1] == second.api_key);
        assert(background_requests[2].arguments[0] == local.api_base);
    }
    const auto refreshed_status = std::string(lv_subject_get_string(model.status_subject()));
    model.show_settings();
    model.show_models();
    assert(wait_for_state(model, [&] { return model.provider_status(local).connection == PicoClawConnectionState::Connected; }));
    assert(started == 6);
    assert(std::string(lv_subject_get_string(model.status_subject())) == refreshed_status);
    assert(!model.busy());
}

void test_background_checks_ignore_deleted_credentials_and_newer_manual_results()
{
    TestEnvironment environment;
    const PicoClawProfile first{"First", "first", "model", "https://first.test/v1", "first-key"};
    const PicoClawProfile changed{"Changed", "changed", "model", "https://changed.test/v1", "old-key"};
    const PicoClawProfile removed{"Removed", "removed", "model", "https://removed.test/v1", "removed-key"};
    const PicoClawProfile last{"Last", "last", "model", "https://last.test/v1", "last-key"};
    save_test_profiles({first, changed, removed, last});
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> started{0};
    std::mutex mutex;
    std::vector<PicoClawRuntimeRequest> requests;
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, "quota=available\nbalance=USD\t99.00\n"};
    });
    PicoClawViewModel model(client, [&](const PicoClawRuntimeRequest &request) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(request);
        }
        ++started;
        gate.wait();
        return PicoClawRuntimeResult{request.action, 0, "quota=available\nbalance=USD\t1.00\n"};
    });
    model.show_models();
    assert(wait_for_state(model, [&] { return started == 1; }));
    model.test_connection(first.api_base, first.api_key);
    assert(wait_for_poll(model));
    assert(model.provider_status(first).available == "Available: 99.00 USD");
    auto edited = changed;
    edited.api_key = "new-key";
    assert(model.update_profile(1, edited));
    assert(model.remove_profile(2));
    assert(model.provider_status(edited).connection == PicoClawConnectionState::Unknown);
    release.set_value();
    assert(wait_for_state(model, [&] { return model.provider_status(last).connection == PicoClawConnectionState::Connected; }));
    assert(model.provider_status(first).available == "Available: 99.00 USD");
    assert(model.provider_status(edited).connection == PicoClawConnectionState::Unknown);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(requests.size() == 2 && requests.back().arguments[0] == last.api_base);
    }
    model.show_chat();
    model.show_models();
    assert(wait_for_state(model, [&] { return model.provider_status(last).connection == PicoClawConnectionState::Connected; }));
    assert(model.provider_status(edited).available == "Available: 1.00 USD");
    assert(started == 5);
    std::lock_guard<std::mutex> lock(mutex);
    assert(requests[3].arguments[1] == "new-key");
}

void test_raw_fetched_model_id_is_saved_without_a_provider_prefix()
{
    TestEnvironment environment;
    const std::string raw_id = "claude-sonnet-4-20250514";
    const std::string api_base = "https://custom-gateway.test/v1";
    const std::string api_key = "private-provider-key";
    std::vector<PicoClawRuntimeRequest> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        if (request.action == PicoClawRuntimeAction::FetchModels)
            return PicoClawRuntimeResult{request.action, 0, "model=" + raw_id + "\nmodel=namespace/model:revision\n"};
        if (request.action == PicoClawRuntimeAction::Status)
            return PicoClawRuntimeResult{request.action, 0,
                "installed=yes\nconfig=ready\nmodel_key=ready\nmodel_name=raw-provider\nmodel_id=" + raw_id +
                "\napi_base=" + api_base + "\n"};
        return PicoClawRuntimeResult{request.action, 0, "Configuration saved"};
    });
    PicoClawViewModel model(client);
    model.fetch_models(api_base, api_key);
    assert(wait_for_poll(model));
    assert(model.fetched_models_match(api_base, api_key));
    assert(model.fetched_models() == std::vector<std::string>({raw_id, "namespace/model:revision"}));
    const PicoClawProfile raw{"Custom Anthropic", "raw-provider", model.fetched_models()[0], api_base, api_key, "anthropic"};
    assert(model.add_profile(raw));
    model.select_profile(7);
    assert(model.save_profile(raw));
    assert(wait_for_poll(model));
    assert(wait_for_poll(model));
    const auto saved = std::find_if(requests.begin(), requests.end(), [](const PicoClawRuntimeRequest &request) {
        return request.action == PicoClawRuntimeAction::SaveConfig;
    });
    assert(saved != requests.end() && saved->arguments[1] == raw_id);
    assert(model.chat_profile().model_id == raw_id && model.chat_profile().provider == "anthropic");
    assert(model.active_profile().provider == "anthropic");
    auto renamed = model.profiles()[7];
    renamed.title = "Renamed custom Anthropic";
    renamed.provider.clear();
    assert(model.update_profile(7, renamed));
    assert(model.chat_profile().provider == "anthropic");
    const PicoClawModel reloaded;
    assert(reloaded.profiles()[7].model_id == raw_id && reloaded.profiles()[7].provider == "anthropic");
}


std::string channels_response()
{
    return "PICOCLAW_CHANNELS 1 1\n\"telegram\" 0 4\n"
           "\"token\" \"private-channel-token\"\n\"base_url\" \"\"\n\"proxy\" \"\"\n\"allow_from\" \"12345\"\n"
           "END_CHANNELS\n";
}

void test_channel_commands_preserve_drafts_and_keep_secrets_private()
{
    TestEnvironment environment;
    std::vector<PicoClawRuntimeRequest> requests;
    int load_exit = 0, save_exit = 1;
    std::string load_output = channels_response();
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        if (request.action == PicoClawRuntimeAction::LoadChannels)
            return PicoClawRuntimeResult{request.action, load_exit, load_output};
        if (request.action == PicoClawRuntimeAction::SaveChannel)
            return PicoClawRuntimeResult{request.action, save_exit, "Untrusted helper echoed private-channel-token"};
        return PicoClawRuntimeResult{request.action, 0, "Safe runtime output"};
    });
    PicoClawViewModel model(client, unused_provider_runner);
    model.request_logs();
    assert(wait_for_poll(model));
    model.show_channels();
    assert(model.screen() == PicoClawScreen::Channels && model.busy());
    assert(wait_for_poll(model));
    assert(model.channels_loaded() && model.channels().size() == 1);
    assert(requests.back().action == PicoClawRuntimeAction::LoadChannels && requests.back().arguments.empty());
    assert(model.edit_channel(0));
    assert(model.set_channel_enabled(true));
    assert(model.set_channel_field(3, "12345,67890"));
    assert(model.channel_dirty());
    assert(model.save_channel());
    assert(!model.save_channel());
    assert(wait_for_poll(model));
    assert(model.channels_error().find("Could not save channel") != std::string::npos);
    assert(model.channel_draft()->enabled && model.channel_dirty());
    assert(!model.channels()[0].enabled);
    assert(model.channel_draft()->fields[3].value == "12345,67890");
    assert(requests.back().action == PicoClawRuntimeAction::SaveChannel && requests.back().arguments[0] == "telegram");
    assert(requests.back().arguments[1].find("\"token\":\"private-channel-token\"") != std::string::npos);
    assert(requests.back().arguments[1].find("\"allow_from\":\"12345,67890\"") != std::string::npos);
    assert(model.runtime().output == "Safe runtime output");
    assert(model.channels_status().find("private-channel-token") == std::string::npos);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("private-channel-token") == std::string::npos);
    model.show_chat();
    model.show_channels();
    assert(!model.busy() && model.channel_dirty());
    save_exit = 0;
    assert(model.save_channel());
    assert(wait_for_poll(model));
    assert(model.channels_error().empty());
    assert(model.channel_draft() && !model.channel_dirty());
    assert(model.channels()[0].enabled && model.channels_restart_required());
    assert(model.channels_status() == "Channel saved. Restart the gateway to apply changes.");
    assert(model.runtime().output == "Safe runtime output");
    assert(model.set_channel_field(0, "unsaved-new-token"));
    load_exit = 1;
    load_output = "helper error echoed unsaved-new-token";
    model.load_channels();
    assert(wait_for_poll(model));
    assert(!model.channels_error().empty() && model.channels_error().find("unsaved-new-token") == std::string::npos);
    assert(model.channel_draft()->fields[0].value == "unsaved-new-token");
    assert(model.channels()[0].fields[0].value == "private-channel-token");
    load_exit = 0;
    load_output = channels_response().substr(0, channels_response().size() - 10);
    model.load_channels();
    assert(wait_for_poll(model));
    assert(model.channels_error().find("incomplete") != std::string::npos);
    assert(model.channel_dirty());
    model.close_channel();
    assert(!model.channel_draft());
    assert(model.edit_channel(0));
    assert(model.channel_draft()->fields[0].value == "private-channel-token");
    assert(model.set_channel_field(0, ""));
    const auto before = requests.size();
    assert(!model.save_channel());
    assert(requests.size() == before && !model.busy());
    assert(model.channels_error() == "Bot token is required to enable this channel.");
    assert(model.set_channel_enabled(false));
    assert(model.save_channel());
    assert(wait_for_poll(model));
    assert(!model.channels()[0].enabled);
}

void test_channels_wait_for_foreground_work_and_preserve_newer_edits()
{
    TestEnvironment environment;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<PicoClawRuntimeRequest> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        requests.push_back(request);
        if (request.action == PicoClawRuntimeAction::Logs) {
            gate.wait();
            return PicoClawRuntimeResult{request.action, 0, "Safe log"};
        }
        if (request.action == PicoClawRuntimeAction::LoadChannels)
            return PicoClawRuntimeResult{request.action, 0, channels_response()};
        return PicoClawRuntimeResult{request.action, 0, "Saved"};
    });
    PicoClawViewModel model(client, unused_provider_runner);
    model.request_logs();
    model.show_channels();
    assert(model.busy() && !model.channels_loaded());
    assert(model.channels_status() == "Waiting to load channels...");
    release.set_value();
    assert(wait_for_state(model, [&] { return model.channels_loaded() && !model.busy(); }));
    assert(requests.size() == 2 && requests.back().action == PicoClawRuntimeAction::LoadChannels);
    assert(model.edit_channel(0));
    assert(model.set_channel_enabled(true));
    assert(model.save_channel());
    assert(model.set_channel_field(0, "newer-editor-token"));
    assert(wait_for_poll(model));
    assert(model.channels()[0].fields[0].value == "private-channel-token");
    assert(model.channel_draft()->fields[0].value == "newer-editor-token" && model.channel_dirty());
    assert(model.channels_status().find("Newer edits are unsaved") != std::string::npos);
    assert(model.runtime().output == "Safe log");
}


std::string weixin_channels_response()
{
    return "PICOCLAW_CHANNELS 1 1\n\"weixin\" 0 6\n"
        "\"token\" \"old-weixin-secret\"\n\"account_id\" \"old-account\"\n"
        "\"base_url\" \"https://ilinkai.weixin.qq.com/\"\n\"cdn_base_url\" \"https://novac2c.cdn.weixin.qq.com/c2c\"\n"
        "\"proxy\" \"http://127.0.0.1:7890\"\n\"allow_from\" \"12345\"\nEND_CHANNELS\n";
}

std::string weixin_start_response(std::string session = "first-session")
{
    return "PICOCLAW_WEIXIN_BINDING 1\nstatus \"wait\"\nqrcode \"" + session +
        "\"\nqr_url \"https://weixin.qq.com/qr/" + session +
        "\"\nbase_url \"https://ilinkai.weixin.qq.com/\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
}

std::string weixin_status_response(std::string status)
{
    return "PICOCLAW_WEIXIN_BINDING 1\nstatus \"" + status + "\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
}

std::string weixin_confirmed_response()
{
    return "PICOCLAW_WEIXIN_BINDING 1\nstatus \"confirmed\"\ntoken \"new-weixin-private-secret\"\n"
        "account_id \"new-weixin-account\"\nbase_url \"https://ilinkai.weixin.qq.com/\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
}

void test_weixin_binding_is_independent_paced_and_saves_only_confirmed_draft()
{
    TestEnvironment environment;
    using State = PicoClawWeixinBindingState;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<PicoClawRuntimeRequest> foreground, binding;
    int status_requests = 0;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        foreground.push_back(request);
        if (request.action == PicoClawRuntimeAction::LoadChannels)
            return PicoClawRuntimeResult{request.action, 0, weixin_channels_response()};
        return PicoClawRuntimeResult{request.action, 0, "Safe activity output"};
    });
    PicoClawViewModel model(client, unused_provider_runner, [&](const PicoClawRuntimeRequest &request) {
        binding.push_back(request);
        if (request.action == PicoClawRuntimeAction::WeixinBindStart)
            return PicoClawRuntimeResult{request.action, 0, weixin_start_response()};
        return PicoClawRuntimeResult{request.action, 0, ++status_requests == 1
            ? weixin_status_response("scaned") : weixin_confirmed_response()};
    }, [&] { return now; });
    assert(!model.start_weixin_binding());
    model.show_channels();
    assert(wait_for_poll(model) && model.edit_channel(0));
    assert(model.start_weixin_binding());
    assert(model.weixin_binding().state == State::Starting && !model.busy());
    assert(!model.save_channel());
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Waiting; }));
    assert(binding.size() == 1 && binding[0].arguments.size() == 1);
    assert(binding[0].arguments[0].find("http://127.0.0.1:7890") != std::string::npos);
    model.request_logs();
    assert(wait_for_poll(model));
    assert(model.runtime().output == "Safe activity output");
    for (int i = 0; i < 20; ++i) (void)model.poll();
    assert(binding.size() == 1);
    now += std::chrono::milliseconds(1999);
    (void)model.poll();
    assert(binding.size() == 1);
    now += std::chrono::milliseconds(1);
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Scanned; }));
    assert(binding.size() == 2 && binding.back().action == PicoClawRuntimeAction::WeixinBindPoll);
    assert(binding.back().arguments[0].find("\"qrcode\":\"first-session\"") != std::string::npos);
    assert(model.set_channel_field(5, "new-allowed-user"));
    now += std::chrono::seconds(2);
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Confirmed; }));
    assert(!model.weixin_binding().active() && model.weixin_binding().qr_url.empty());
    assert(model.weixin_binding().account_id == "new-weixin-account");
    assert(model.channel_draft()->fields[0].value == "new-weixin-private-secret");
    assert(model.channel_draft()->fields[5].value == "new-allowed-user");
    assert(model.channels()[0].fields[0].value == "old-weixin-secret" && model.channel_dirty());
    assert(model.runtime().output == "Safe activity output");
    assert(model.channels_status().find("new-weixin-private-secret") == std::string::npos);
    assert(std::string(lv_subject_get_string(model.status_subject())).find("new-weixin-private-secret") == std::string::npos);
    model.show_chat();
    model.show_channels();
    assert(model.weixin_binding().state == State::Confirmed && model.channel_dirty());
    assert(model.save_channel());
    assert(wait_for_poll(model));
    assert(foreground.back().action == PicoClawRuntimeAction::SaveChannel);
    assert(foreground.back().arguments[1].find("new-weixin-private-secret") != std::string::npos);
    assert(model.channels()[0].fields[0].value == "new-weixin-private-secret");
    assert(model.weixin_binding().message == "Weixin account saved. Restart the gateway to apply changes.");
    model.close_channel();
    assert(model.edit_channel(0));
    assert(model.weixin_binding().state == State::Idle && model.weixin_binding().account_id == "new-weixin-account");
}

void test_weixin_refresh_cancel_and_navigation_ignore_late_responses()
{
    TestEnvironment environment;
    using State = PicoClawWeixinBindingState;
    auto now = std::chrono::steady_clock::time_point{};
    std::promise<void> first_release, poll_release;
    const auto first_gate = first_release.get_future().share();
    const auto poll_gate = poll_release.get_future().share();
    std::atomic<int> starts{0}, polls{0};
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, weixin_channels_response()};
    });
    PicoClawViewModel model(client, unused_provider_runner, [&](const PicoClawRuntimeRequest &request) {
        if (request.action == PicoClawRuntimeAction::WeixinBindStart) {
            const int sequence = ++starts;
            if (sequence == 1) first_gate.wait();
            return PicoClawRuntimeResult{request.action, 0, weixin_start_response("session-" + std::to_string(sequence))};
        }
        ++polls;
        poll_gate.wait();
        return PicoClawRuntimeResult{request.action, 0, weixin_confirmed_response()};
    }, [&] { return now; });
    model.show_channels();
    assert(wait_for_poll(model) && model.edit_channel(0));
    assert(model.start_weixin_binding());
    assert(wait_for_state(model, [&] { return starts == 1; }));
    assert(model.start_weixin_binding()); // Refresh while the first start is still in flight.
    assert(model.weixin_binding().state == State::Starting && model.weixin_binding().qr_url.empty());
    first_release.set_value();
    assert(wait_for_state(model, [&] { return starts == 2 && model.weixin_binding().state == State::Waiting; }));
    assert(model.weixin_binding().qr_url == "https://weixin.qq.com/qr/session-2");
    now += std::chrono::seconds(2);
    assert(wait_for_state(model, [&] { return polls == 1; }));
    model.cancel_weixin_binding();
    assert(model.weixin_binding().state == State::Cancelled);
    assert(model.weixin_binding().qr_url.empty());
    poll_release.set_value();
    for (int i = 0; i < 30; ++i) {
        (void)model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(model.channel_draft()->fields[0].value == "old-weixin-secret");
    assert(model.weixin_binding().state == State::Cancelled);
    assert(model.start_weixin_binding());
    model.show_chat();
    assert(model.weixin_binding().state == State::Cancelled);
    for (int i = 0; i < 30; ++i) {
        (void)model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    model.show_channels();
    assert(model.channel_draft()->fields[0].value == "old-weixin-secret");
    assert(model.start_weixin_binding());
    model.close_channel();
    assert(!model.channel_draft() && !model.weixin_binding().active());
}

void test_weixin_failures_expiry_and_deadline_preserve_account()
{
    TestEnvironment environment;
    using State = PicoClawWeixinBindingState;
    auto now = std::chrono::steady_clock::time_point{};
    int exit = 1;
    std::string response = "Error echoing old-weixin-secret";
    std::atomic<int> requests{0};
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, weixin_channels_response()};
    });
    PicoClawViewModel model(client, unused_provider_runner, [&](const PicoClawRuntimeRequest &request) {
        ++requests;
        return PicoClawRuntimeResult{request.action, exit, response};
    }, [&] { return now; });
    model.show_channels();
    assert(wait_for_poll(model) && model.edit_channel(0));
    assert(model.start_weixin_binding());
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Failed; }));
    assert(model.weixin_binding().message.find("old-weixin-secret") == std::string::npos);
    assert(model.runtime().output.find("old-weixin-secret") == std::string::npos);
    assert(model.channel_draft()->fields[0].value == "old-weixin-secret");
    exit = 0;
    response = weixin_start_response();
    assert(model.start_weixin_binding());
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Waiting; }));
    response = weixin_status_response("expired");
    now += std::chrono::seconds(2);
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Expired; }));
    response = weixin_start_response("fresh-session");
    assert(model.start_weixin_binding());
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Waiting; }));
    const int before = requests;
    now += std::chrono::minutes(5);
    assert(model.poll());
    assert(model.weixin_binding().state == State::Expired && model.weixin_binding().qr_url.empty());
    assert(requests == before);
    assert(model.channel_draft()->fields[0].value == "old-weixin-secret");
    response = "PICOCLAW_WEIXIN_BINDING 1\nstatus \"wait\"\n";
    assert(model.start_weixin_binding());
    assert(wait_for_state(model, [&] { return model.weixin_binding().state == State::Failed; }));
    assert(model.channel_draft()->fields[0].value == "old-weixin-secret");
}

}  // namespace

int main()
{
    TestEnvironment environment;
    environment.set("PICOCLAW_HOME", environment.root + "/runtime");
    lv_init();
    test_channel_commands_preserve_drafts_and_keep_secrets_private();
    test_channels_wait_for_foreground_work_and_preserve_newer_edits();
    test_weixin_binding_is_independent_paced_and_saves_only_confirmed_draft();
    test_weixin_refresh_cancel_and_navigation_ignore_late_responses();
    test_weixin_failures_expiry_and_deadline_preserve_account();
    test_update_selection_and_install_failure_recovery();
    test_docker_retains_image_install();
    test_registry_operations_and_busy_protection();
    test_model_import_keeps_keys_private_and_save_retains_restart_notice();
    test_chat_model_switch_uses_selected_alias_and_preserves_conversation();
    test_websocket_model_switch_restarts_and_verifies_gateway();
    test_remote_websocket_switch_is_rejected_without_changing_local_configuration();
    test_fetched_models_are_scoped_to_successful_request_credentials();
    test_provider_status_tracks_credentials_and_balance_failures();
    test_models_entry_refreshes_accounts_without_blocking_foreground_work();
    test_background_checks_ignore_deleted_credentials_and_newer_manual_results();
    test_raw_fetched_model_id_is_saved_without_a_provider_prefix();
    std::mutex requests_mutex;
    std::vector<PicoClawRuntimeRequest> requests;
    PicoClawClient client([&](const PicoClawRuntimeRequest &request) {
        {
            std::lock_guard<std::mutex> lock(requests_mutex);
            requests.push_back(request);
        }
        PicoClawRuntimeResult result;
        result.action    = request.action;
        result.exit_code = 0;
        if (request.action == PicoClawRuntimeAction::Status) {
            result.output =
                "installed=yes\nversion=picoclaw 0.3.1\ngateway=stopped\nnetwork=ready\n"
                "config=ready\nmodel_key=ready\nurl=http://device:18790\n"
                "model_name=deepseek-chat\nmodel_id=deepseek/deepseek-chat\n"
                "api_base=https://api.deepseek.com/v1\n";
        } else if (request.action == PicoClawRuntimeAction::Ask) {
            result.output = "time=ignored\nHello from PicoClaw";
        } else {
            result.output = "OK";
        }
        return result;
    });

    {
        PicoClawViewModel view_model(client);
        assert(view_model.screen() == PicoClawScreen::Chat);
        view_model.request_status();
        assert(wait_for_poll(view_model));
        assert(view_model.runtime().installed);
        assert(view_model.runtime().network_ready);
        assert(std::string(lv_subject_get_string(view_model.status_subject())) == "Ready to chat");

        assert(view_model.send("Hello"));
        assert(view_model.messages().size() == 2);
        assert(view_model.messages().back().pending);
        assert(wait_for_poll(view_model));
        assert(!view_model.messages().back().pending);
        assert(view_model.messages().back().text == "Hello from PicoClaw");
        const auto first_session = view_model.protocol_session_id();
        assert(view_model.new_chat());
        assert(view_model.messages().empty());
        assert(std::string(lv_subject_get_string(view_model.status_subject())) == "New conversation ready");
        assert(view_model.protocol_session_id() != first_session);
        assert(view_model.conversations().size() == 2);
        assert(view_model.select_conversation(0));
        assert(view_model.protocol_session_id() == first_session);
        assert(view_model.messages().back().text == "Hello from PicoClaw");
        assert(view_model.send("Continue first conversation"));
        assert(!view_model.new_chat());
        assert(!view_model.select_conversation(1));
        assert(wait_for_poll(view_model));

        view_model.show_chat();
        assert(view_model.screen() == PicoClawScreen::Chat);
        assert(!view_model.back());
        assert(view_model.screen() == PicoClawScreen::Chat);

        view_model.show_settings();
        assert(view_model.screen() == PicoClawScreen::Settings);
        view_model.select_profile(6);
        PicoClawProfile profile{"My provider", "my-agent", "openai/my-model", "https://example.test/v1", "secret"};
        assert(view_model.save_profile(profile));
        assert(wait_for_poll(view_model));
        assert(view_model.runtime().output == "OK");
        assert(wait_for_poll(view_model));
        assert(view_model.back());
        assert(view_model.screen() == PicoClawScreen::Chat);
    }

    {
        std::lock_guard<std::mutex> lock(requests_mutex);
        assert(requests.size() >= 4);
        bool saw_save = false;
        std::string first_cli_session;
        for (const auto &request : requests) {
            if (request.action == PicoClawRuntimeAction::Ask) {
                assert(request.arguments.size() == 3);
                assert(request.arguments[2] == "deepseek-chat");
                if (first_cli_session.empty()) first_cli_session = request.arguments[1];
                assert(request.arguments[1] == first_cli_session);
            }
            if (request.action == PicoClawRuntimeAction::SaveConfig) {
                saw_save = true;
                assert(request.arguments.size() == 4);
                assert(request.arguments[0] == "my-agent");
                assert(request.arguments[3] == "secret");
            }
        }
        assert(saw_save);
    }
    const std::string banner =
        "\r\n\x1b[1;38;2;62;93;185m██████╗ ██╗ ██████╗ ██████╗  ██████╗██╗      █████╗ ██╗    ██╗\n"
        "██╔══██╗██║██╔════╝██╔═══██╗██╔════╝██║     ██╔══██╗██║    ██║\n"
        "██████╔╝██║██║     ██║   ██║██║     ██║     ███████║██║ █╗ ██║\n"
        "██╔═══╝ ██║██║     ██║   ██║██║     ██║     ██╔══██║██║███╗██║\n"
        "██║     ██║╚██████╗╚██████╔╝╚██████╗███████╗██║  ██║╚███╔███╔╝\n"
        "╚═╝     ╚═╝ ╚═════╝ ╚═════╝  ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝\n \x1b[0m\r\n";
    const std::string markdown = "## 命令示例\n\n```sh\nprintf 'hello'\n```\n\n**说明**\n[INFO] example output\n┌───┐\n│ OK│\n└───┘";
    struct ReplyCase {
        std::string output;
        int exit_code;
        std::string expected;
    };
    for (const auto &fixture : std::vector<ReplyCase>{
             {banner + "11:42:00 INF agent > Starting\n\n🦞 " + markdown + "\n", 0, markdown},
             {banner + "11:42:00 ERR agent > LLM failed\nError: API request failed:\n  Status: 401\n"
                       "11:42:00 ERR events > turn ended\n", 1, "Error: API request failed:\n  Status: 401"},
             {banner + "\n🦞 \x1b[32mOK\x1b[0m\r\n", 0, "OK"},
             {banner, 0, "PicoClaw returned an empty response."},
             {"Connection refused\n", 1, "Error: Connection refused"},
             {markdown, 0, markdown},
         }) {
        PicoClawClient fixture_client([fixture](const PicoClawRuntimeRequest &request) {
            PicoClawRuntimeResult result;
            result.action = request.action;
            result.exit_code = fixture.exit_code;
            result.output = fixture.output;
            return result;
        });
        PicoClawViewModel view_model(fixture_client);
        assert(view_model.send("Check response formatting"));
        assert(wait_for_poll(view_model));
        assert(view_model.messages().back().text == fixture.expected);
    }
    lv_deinit();
    return 0;
}
