#include "test_environment.hpp"
#include "domain/picoclaw_client.hpp"
#include "pages/picoclaw/picoclaw_model.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

namespace {

PicoClawRuntimeResult wait_for_result(PicoClawClient &client)
{
    PicoClawRuntimeResult result;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (client.poll(result)) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(false && "PicoClaw client timed out");
    return result;
}

void test_model_profiles()
{
    PicoClawModel model;
    assert(model.screen() == PicoClawScreen::Chat);
    assert(model.profiles().size() == 7);
    assert(model.active_profile().title == "DeepSeek Chat");
    assert(model.profiles()[3].model_id == "anthropic/claude-3-5-sonnet-latest");
    assert(model.profiles()[4].model_id == "gemini/gemini-2.0-flash");
    assert(model.profiles()[5].model_id == "ollama/llama3.2");
    assert(model.profiles()[6].title == "OpenAI compatible");
    assert(model.select_profile(2));
    assert(model.active_profile().model_name == "gpt-4o-mini");
    assert(!model.select_profile(99));

    std::string error;
    PicoClawProfile invalid{"", "", "", "", ""};
    assert(!model.update_active_profile(invalid, error));
    assert(!error.empty());
    PicoClawProfile custom{"Local model", "local", "openai/local-model", "http://127.0.0.1:8080/v1", "token"};
    assert(model.update_active_profile(custom, error));
    assert(error.empty());
    assert(model.active_profile().api_key == "token");
}

void test_model_navigation()
{
    PicoClawModel model;
    assert(!model.back());
    for (const auto screen : {PicoClawScreen::Models, PicoClawScreen::Channels, PicoClawScreen::Settings}) {
        assert(model.show(screen));
        assert(model.back());
        assert(model.screen() == PicoClawScreen::Chat);
        assert(!model.back());
    }
    assert(model.show(PicoClawScreen::Settings));
    assert(model.show(PicoClawScreen::Logs));
    assert(model.back());
    assert(model.screen() == PicoClawScreen::Settings);
    assert(model.back());
    assert(model.screen() == PicoClawScreen::Chat);
    assert(!model.show(static_cast<PicoClawScreen>(-1)));
    assert(!model.show(static_cast<PicoClawScreen>(static_cast<int>(PicoClawScreen::Logs) + 1)));
    assert(model.screen() == PicoClawScreen::Chat);
}

void test_model_registry_persistence_and_failure_recovery()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    PicoClawProfile configured{"My model", "my-model", "openai/custom", "https://example.test/v1", "sk-test-secret"};
    const auto active = model.active_profile_index();
    assert(model.add_profile(configured, error));
    assert(model.profiles().size() == 8 && model.active_profile_index() == active);

    const auto path = environment.root + "/models.conf";
    struct stat info{};
    assert(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    PicoClawModel reloaded;
    assert(reloaded.profiles().size() == 8);
    assert(reloaded.profiles().back().api_key == "sk-test-secret");
    configured.api_key = "sk-updated";
    assert(reloaded.update_profile(7, configured, error));
    PicoClawModel updated;
    assert(updated.profiles().back().api_key == "sk-updated");

    updated.apply_runtime_status("model_name=external\nmodel_id=openai/external\napi_base=https://external.test/v1\n");
    assert(updated.profiles().size() == 9);
    assert(updated.profiles()[7].api_key == "sk-updated");
    assert(updated.active_profile().model_name == "external" && updated.active_profile().api_key.empty());
    assert(updated.remove_profile(7, error));
    assert(updated.active_profile().model_name == "external");
    PicoClawModel removed;
    assert(removed.profiles().size() == 8);
    assert(removed.profiles().back().model_name == "external");

    {
        TestEnvironment inaccessible;
        inaccessible.set("PICOCLAW_MODELS_PATH", inaccessible.root + "/not-a-directory/models.conf");
        std::ofstream(inaccessible.root + "/not-a-directory") << "occupied";
        const auto previous_size = removed.profiles().size();
        const auto previous_active = removed.active_profile_index();
        const auto previous_key = removed.active_profile().api_key;
        assert(!removed.add_profile(configured, error));
        assert(removed.profiles().size() == previous_size && removed.active_profile_index() == previous_active);
        assert(!removed.update_profile(previous_active, configured, error));
        assert(removed.active_profile().api_key == previous_key);
        assert(!removed.remove_profile(previous_active, error));
        assert(removed.profiles().size() == previous_size && removed.active_profile_index() == previous_active);
    }

    const std::string broken = "PICOCLAW_MODELS 1 2\n\"secret data preserved\"\n";
    std::ofstream(path) << broken;
    PicoClawModel corrupted;
    assert(!corrupted.profiles_error().empty());
    assert(!corrupted.add_profile(configured, error));
    assert(!corrupted.remove_profile(0, error));
    assert(!corrupted.update_profile(0, configured, error));
    std::ifstream preserved(path);
    assert(std::string((std::istreambuf_iterator<char>(preserved)), {}) == broken);
}

void test_model_registry_validation()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const PicoClawProfile valid{"Custom model", "custom-model", "openai/custom", "https://example.test/v1", "secret"};
    assert(model.add_profile(valid, error));
    assert(!model.add_profile(valid, error) && error.find("already in use") != std::string::npos);
    auto collision = valid;
    collision.model_name = model.profiles()[0].model_name;
    assert(!model.update_profile(7, collision, error));
    assert(model.update_profile(7, valid, error));
    for (std::size_t field_index = 0; field_index < 5; ++field_index) {
        for (const char forbidden : {'\r', '\n', '\0'}) {
            auto invalid = valid;
            std::string *fields[] = {&invalid.title, &invalid.model_name, &invalid.model_id, &invalid.api_base, &invalid.api_key};
            fields[field_index]->push_back(forbidden);
            assert(!model.update_profile(7, invalid, error));
            assert(error.find("line breaks or null") != std::string::npos);
        }
        auto oversized = valid;
        std::string *fields[] = {&oversized.title, &oversized.model_name, &oversized.model_id, &oversized.api_base,
                                &oversized.api_key};
        fields[field_index]->append(field_index == 4 ? 512U : 256U, 'x');
        assert(!model.update_profile(7, oversized, error) && error.find("limited") != std::string::npos);
    }
    auto boundary = valid;
    boundary.title = std::string(255, 't');
    boundary.model_name = std::string(255, 'm');
    boundary.model_id = std::string(255, 'i');
    boundary.api_base = "https://" + std::string(247, 'b');
    boundary.api_key = std::string(511, 'k');
    assert(model.update_profile(7, boundary, error));
    assert(PicoClawModel().profiles()[7].api_key == boundary.api_key);
    // Older or externally produced registries may exceed editor limits.
    // Load the complete value and reject an unchanged oversized submission.
    const auto path = environment.root + "/models.conf";
    auto legacy = valid;
    legacy.api_key = std::string(600, 'k');
    {
        std::ofstream saved(path);
        saved << "PICOCLAW_MODELS 1 1\n" << std::quoted(legacy.title) << ' ' << std::quoted(legacy.model_name) << ' '
              << std::quoted(legacy.model_id) << ' ' << std::quoted(legacy.api_base) << ' '
              << std::quoted(legacy.api_key) << '\n';
    }
    PicoClawModel legacy_loaded;
    assert(legacy_loaded.profiles().size() == 1 && legacy_loaded.active_profile().api_key == legacy.api_key);
    assert(!legacy_loaded.update_profile(0, legacy, error));
    assert(PicoClawModel().active_profile().api_key == legacy.api_key);
}

void test_generated_model_aliases_survive_provider_edits_and_persistence()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const auto first_index = model.profiles().size();
    PicoClawProfile profile{"Provider Alpha", "", "org/model:latest", "https://alpha.test/v1", "alpha-key", "openai"};
    assert(model.add_profile(profile, error));
    const auto first_alias = model.profiles().back().model_name;
    assert(first_alias == "org-model-latest");

    profile.title = "Provider Beta";
    profile.api_base = "https://beta.test/v1";
    assert(model.add_profile(profile, error));
    const auto second_alias = model.profiles().back().model_name;
    assert(second_alias != first_alias);
    assert(model.profiles()[first_index].model_id == model.profiles()[first_index + 1U].model_id);

    // Auto-generated aliases also avoid aliases chosen by existing callers.
    profile.model_name = first_alias + "-3";
    assert(model.add_profile(profile, error));
    profile.model_name.clear();
    profile.model_id = "org:model/latest";
    assert(model.add_profile(profile, error));
    assert(model.profiles().back().model_name == first_alias + "-4");

    profile.title = "Renamed provider";
    profile.model_id = "new/model";
    profile.api_key = "new-key";
    profile.provider.clear();
    assert(model.update_profile(first_index, profile, error));
    PicoClawModel reloaded;
    assert(reloaded.profiles()[first_index].title == "Renamed provider");
    assert(reloaded.profiles()[first_index].model_name == first_alias);
    assert(reloaded.profiles()[first_index].model_id == "new/model");
    assert(reloaded.profiles()[first_index].api_key == "new-key");
    assert(reloaded.profiles()[first_index].provider == "openai");
    assert(reloaded.profiles()[first_index + 1U].title == "Provider Beta");
    assert(reloaded.profiles()[first_index + 1U].model_name == second_alias);

    assert(reloaded.select_profile(first_index));
    profile.title = "Active provider";
    profile.model_id = "another/model";
    assert(reloaded.update_active_profile(profile, error));
    assert(reloaded.active_profile().model_name == first_alias);
    assert(reloaded.active_profile().title == "Active provider");
    assert(reloaded.active_profile().model_id == "another/model");
}

void test_generated_model_aliases_remain_valid_at_field_limits()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    PicoClawProfile profile{"Custom provider", "", std::string(255, 'x'), "https://example.test/v1", "", "openai"};
    std::set<std::string> aliases;
    for (int index = 0; index < 10; ++index) {
        assert(model.add_profile(profile, error));
        const auto &alias = model.profiles().back().model_name;
        assert(!alias.empty() && alias.size() <= 255U);
        assert(aliases.insert(alias).second);
        assert(model.profiles().back().model_id == profile.model_id);
    }
    PicoClawModel reloaded;
    for (const auto &saved : reloaded.profiles()) aliases.erase(saved.model_name);
    assert(aliases.empty());

    profile.model_id = "模型";
    assert(reloaded.add_profile(profile, error));
    assert(reloaded.profiles().back().model_name == "model");
    assert(reloaded.profiles().back().model_id == "模型");

    // Omitting the alias must not bypass field validation or change explicit aliases.
    profile.model_id = std::string(256, 'x');
    assert(!reloaded.add_profile(profile, error) && error.find("limited") != std::string::npos);
    profile.model_id = "invalid\nmodel";
    assert(!reloaded.add_profile(profile, error) && error.find("line breaks") != std::string::npos);
    profile.model_id = "valid-model";
    for (const auto &alias : {" ", "invalid/alias", "\n"}) {
        profile.model_name = alias;
        assert(!reloaded.add_profile(profile, error));
    }
    profile.model_name = "explicit-alias";
    assert(reloaded.add_profile(profile, error));
    assert(reloaded.profiles().back().model_name == "explicit-alias");
    assert(!reloaded.add_profile(profile, error) && error.find("already in use") != std::string::npos);
}

void test_runtime_status_preserves_conflicting_saved_model()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const PicoClawProfile saved{"Saved provider", "shared-model", "openai/saved", "https://saved.test/v1",
                                "saved-provider-secret", "openai"};
    const auto saved_index = model.profiles().size();
    assert(model.add_profile(saved, error));
    const auto previous_active = model.active_profile_index();
    const auto count = model.profiles().size();
    for (const auto &status : {
             "model_name=shared-model\nmodel_id=openai/runtime\napi_base=https://saved.test/v1\n",
             "model_name=shared-model\nmodel_id=openai/saved\napi_base=https://runtime.test/v1\n"}) {
        model.apply_runtime_status(status);
        model.apply_runtime_status(status);
        assert(model.profiles().size() == count);
        assert(model.active_profile_index() == previous_active);
        assert(model.profiles()[saved_index].title == saved.title);
        assert(model.profiles()[saved_index].model_name == saved.model_name);
        assert(model.profiles()[saved_index].model_id == saved.model_id);
        assert(model.profiles()[saved_index].api_base == saved.api_base);
        assert(model.profiles()[saved_index].api_key == saved.api_key);
    }
    auto edited = saved;
    edited.model_name.clear();
    edited.title = "Renamed provider";
    assert(model.update_profile(saved_index, edited, error));
    assert(model.profiles()[saved_index].model_name == saved.model_name);
    assert(PicoClawModel().profiles()[saved_index].title == edited.title);
    model.apply_runtime_status("model_name=shared-model\nmodel_id=openai/saved\napi_base=https://saved.test/v1\n");
    assert(model.active_profile_index() == saved_index);
}

void test_internal_alias_collisions_can_be_saved()
{
    for (const unsigned version : {1U, 2U}) {
        for (const bool edit_imported : {false, true}) {
            TestEnvironment environment;
            std::string error;
            const PicoClawProfile saved{"Saved provider", "shared-model", "openai/saved", "https://saved.test/v1",
                                        "saved-provider-secret", "openai"};
            const PicoClawProfile imported{"Runtime provider", "shared-model", "openai/runtime", "https://runtime.test/v1",
                                           "", "openai"};
            const std::size_t saved_index = 0;
            const std::size_t imported_index = 1;
            const std::string status = "model_name=shared-model\nmodel_id=openai/runtime\napi_base=https://runtime.test/v1\n";
            // Earlier status imports could persist duplicate internal aliases.
            {
                std::ofstream output(environment.root + "/models.conf");
                output << "PICOCLAW_MODELS " << version << " 2\n";
                for (const auto &profile : {saved, imported}) {
                    output << std::quoted(profile.title) << ' ' << std::quoted(profile.model_name) << ' '
                           << std::quoted(profile.model_id) << ' ' << std::quoted(profile.api_base) << ' '
                           << std::quoted(profile.api_key);
                    if (version == 2) output << ' ' << std::quoted(profile.provider);
                    output << '\n';
                }
            }
            PicoClawModel model;
            const auto edited_index = edit_imported ? imported_index : saved_index;
            auto edited = model.profiles()[edited_index];
            edited.title = "Renamed provider";
            model.select_profile(edited_index);
            assert(!model.update_active_profile(edited, error));
            assert(!model.update_profile(edited_index, edited, error));
            assert(error.find("already in use") != std::string::npos);

            edited.model_name.clear();
            PicoClawModel active_editor;
            active_editor.select_profile(edited_index);
            assert(active_editor.update_active_profile(edited, error));
            assert(active_editor.active_profile().model_name != saved.model_name);
            assert(model.update_profile(edited_index, edited, error));
            assert(error.empty());
            const auto recovered_alias = model.profiles()[edited_index].model_name;
            assert(recovered_alias != saved.model_name);
            assert(model.profiles()[saved_index].model_name != model.profiles()[imported_index].model_name);
            assert(model.profiles()[saved_index].model_id == saved.model_id);
            assert(model.profiles()[saved_index].api_base == saved.api_base);
            assert(model.profiles()[saved_index].api_key == saved.api_key);
            assert(model.profiles()[imported_index].model_id == "openai/runtime");
            assert(model.profiles()[imported_index].api_base == "https://runtime.test/v1");
            assert(model.profiles()[imported_index].api_key.empty());

            PicoClawModel reloaded;
            assert(reloaded.profiles().size() == model.profiles().size());
            assert(reloaded.profiles()[edited_index].model_name == recovered_alias);
            assert(reloaded.profiles()[edited_index].title == edited.title);
            edited.title = "Renamed again";
            assert(reloaded.update_profile(edited_index, edited, error));
            assert(reloaded.profiles()[edited_index].model_name == recovered_alias);
            assert(PicoClawModel().profiles()[edited_index].title == edited.title);
            const auto count = reloaded.profiles().size();
            reloaded.apply_runtime_status(status);
            reloaded.apply_runtime_status(status);
            assert(reloaded.profiles().size() == count);
            assert(reloaded.profiles()[edited_index].model_name == recovered_alias);
            assert(reloaded.profiles()[saved_index].model_name != reloaded.profiles()[imported_index].model_name);
            assert(reloaded.profiles()[saved_index].api_key == saved.api_key);
            assert(reloaded.profiles()[imported_index].api_key.empty());
            if (!edit_imported) assert(reloaded.active_profile_index() == imported_index);
        }
    }
}

void test_raw_model_ids_and_provider_metadata_survive_registry_round_trip()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const PicoClawProfile raw{"Custom Claude", "custom-claude", "claude-sonnet-4-20250514",
                              "https://gateway.example.test/api", "private-key", "anthropic"};
    assert(model.add_profile(raw, error));
    assert(model.profiles().back().model_id == raw.model_id);
    PicoClawModel loaded;
    assert(loaded.profiles().back().model_id == raw.model_id);
    assert(loaded.profiles().back().provider == "anthropic");
    assert(PicoClawModel::profile_provider(loaded.profiles().back()) == "anthropic");
    std::ifstream input(environment.root + "/models.conf");
    std::string magic;
    unsigned version = 0;
    input >> magic >> version;
    assert(magic == "PICOCLAW_MODELS" && version == 2);

    // Older callers may omit the new field when changing only the title.
    auto edited = raw;
    edited.title = "Renamed Claude";
    edited.provider.clear();
    assert(loaded.update_profile(7, edited, error));
    assert(loaded.profiles()[7].provider == "anthropic" && loaded.profiles()[7].model_id == raw.model_id);
    loaded.apply_runtime_status("model_name=custom-claude\nmodel_id=" + raw.model_id + "\napi_base=" + raw.api_base + "\n");
    assert(loaded.active_profile().provider == "anthropic");
    assert(loaded.active_profile().model_id == raw.model_id);
    auto invalid = raw;
    invalid.provider = "unsupported";
    assert(!loaded.update_profile(7, invalid, error));
    assert(error.find("supported model provider") != std::string::npos);
    assert(PicoClawModel().profiles()[7].provider == "anthropic");
}

void test_registry_v1_migration_and_import_infer_only_provider_metadata()
{
    TestEnvironment environment;
    const std::vector<PicoClawProfile> legacy{
        {"Prefix", "prefix", "anthropic/claude-original", "https://custom.test/v1", ""},
        {"Raw Gemini", "raw-gemini", "gemini-2.5-flash", "https://generativelanguage.googleapis.com/v1beta", ""},
        {"Raw DeepSeek", "raw-deepseek", "deepseek-chat", "https://api.deepseek.com/v1", ""},
        {"Raw Ollama", "raw-ollama", "llama3.2:latest", "http://localhost:11434/v1", ""},
        {"Unknown", "unknown", "org/model/original", "https://custom.test/v1", ""},
        {"Raw Anthropic", "raw-anthropic", "claude-custom", "https://api.anthropic.com/v1", ""},
    };
    const auto write_v1 = [](const std::vector<PicoClawProfile> &profiles) {
        std::ostringstream output;
        output << "PICOCLAW_MODELS 1 " << profiles.size() << '\n';
        for (const auto &profile : profiles)
            output << std::quoted(profile.title) << ' ' << std::quoted(profile.model_name) << ' '
                   << std::quoted(profile.model_id) << ' ' << std::quoted(profile.api_base) << ' '
                   << std::quoted(profile.api_key) << '\n';
        return output.str();
    };
    std::ofstream(environment.root + "/models.conf") << write_v1(legacy);
    PicoClawModel migrated;
    const std::vector<std::string> providers{"anthropic", "gemini", "deepseek", "ollama", "openai", "anthropic"};
    for (std::size_t index = 0; index < legacy.size(); ++index) {
        assert(migrated.profiles()[index].model_id == legacy[index].model_id);
        assert(migrated.profiles()[index].provider == providers[index]);
    }
    std::string error;
    auto custom = migrated.profiles()[4];
    custom.provider = "anthropic";
    assert(migrated.update_profile(4, custom, error));
    auto imported = custom;
    imported.provider.clear(); // Runtime export v1 has no supplier metadata.
    imported.api_key = "imported-private-key";
    const PicoClawProfile additional{"Imported raw", "imported-raw", "bare-model-id", "https://api.deepseek.com/v1", "key"};
    assert(migrated.merge_profiles(write_v1({imported, additional}), error));
    PicoClawModel reloaded;
    assert(reloaded.profiles()[4].provider == "anthropic");
    assert(reloaded.profiles()[4].model_id == "org/model/original");
    assert(reloaded.profiles()[4].api_key == "imported-private-key");
    assert(reloaded.profiles().back().model_id == "bare-model-id" && reloaded.profiles().back().provider == "deepseek");
}

void test_model_registry_import_is_atomic_and_preserves_existing_models()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const auto export_models = [](const std::vector<PicoClawProfile> &profiles) {
        std::ostringstream result;
        result << "PICOCLAW_MODELS 1 " << profiles.size() << '\n';
        for (const auto &profile : profiles)
            result << std::quoted(profile.title) << ' ' << std::quoted(profile.model_name) << ' '
                   << std::quoted(profile.model_id) << ' ' << std::quoted(profile.api_base) << ' '
                   << std::quoted(profile.api_key) << '\n';
        return result.str();
    };
    PicoClawProfile imported{"Runtime model", "runtime-model", "openai/runtime", "https://runtime.test/v1", "new-secret"};
    auto existing = model.profiles()[0];
    existing.api_key = "first-secret";
    assert(model.merge_profiles(export_models({imported, existing}), error));
    assert(model.profiles().size() == 8 && model.profiles()[0].api_key == "first-secret");
    assert(model.profiles()[7].api_key == "new-secret");
    const auto initial_alias = model.active_profile().model_name;
    existing.api_key = "replacement-must-not-win";
    imported.api_base = "https://unrelated.test/v1";
    imported.api_key = "wrong-provider-secret";
    assert(model.merge_profiles(export_models({imported, existing}), error));
    assert(model.active_profile().model_name == initial_alias);
    assert(model.profiles()[0].api_key == "first-secret");
    assert(model.profiles()[7].api_base == "https://runtime.test/v1" && model.profiles()[7].api_key == "new-secret");
    assert(PicoClawModel().profiles()[7].api_key == "new-secret");

    PicoClawProfile additional{"Later model", "later-model", "openai/later", "https://later.test/v1", "later-secret"};
    const auto complete = export_models({additional});
    for (const auto &invalid : {complete.substr(0, complete.size() - 3U), complete + "unexpected trailing output",
                               export_models({additional, additional}), "PICOCLAW_MODELS 1 2\n" + complete.substr(19)}) {
        assert(!model.merge_profiles(invalid, error));
        assert(model.profiles().size() == 8 && model.profiles()[7].api_key == "new-secret");
    }
    assert(!model.merge_profiles(std::string(1024U * 1024U + 1, 'x'), error));
    assert(model.merge_profiles("PICOCLAW_MODELS 1 0\n", error));
    {
        TestEnvironment inaccessible;
        inaccessible.set("PICOCLAW_MODELS_PATH", inaccessible.root + "/not-a-directory/models.conf");
        std::ofstream(inaccessible.root + "/not-a-directory") << "occupied";
        assert(!model.merge_profiles(complete, error));
        assert(model.profiles().size() == 8);
    }
    assert(PicoClawModel().profiles().size() == 8);
}

void test_model_settings_persistence()
{
    TestEnvironment environment;
    const std::string path = environment.root + "/ui-settings.conf";
    (void)::unlink(path.c_str());
    // Release files may contain a selectable theme. It must be ignored while
    // the remaining settings continue to load and migrate on the next save.
    std::ofstream(path) << "host=legacy.local\nport=19999\npublic_mode=true\n"
                           "theme=dark\nlanguage=zh\n";
    PicoClawModel model;
    assert(model.settings().host == "legacy.local");
    assert(model.settings().port == 19999);
    assert(model.settings().public_mode);
    assert(model.settings().language == "zh");

    PicoClawSettings settings = model.settings();
    settings.host             = "cm0.local";
    settings.arguments        = "--no-browser";
    std::string error;
    assert(model.update_settings(settings, error));
    assert(model.settings().host == "0.0.0.0");
    assert(model.runtime().web_url == "http://0.0.0.0:19999");

    PicoClawModel loaded;
    assert(loaded.settings().port == 19999);
    assert(loaded.settings().public_mode);
    assert(loaded.settings().language == "zh");
    std::ifstream migrated(path);
    const std::string migrated_text((std::istreambuf_iterator<char>(migrated)), {});
    assert(migrated_text.find("theme=") == std::string::npos);
    assert(migrated_text.find("host=0.0.0.0") != std::string::npos);
    (void)::unlink(path.c_str());
}

void test_settings_save_respects_update_lock()
{
    TestEnvironment environment;
    const auto runtime_home = environment.root + "/runtime";
    environment.set("PICOCLAW_HOME", runtime_home);
    PicoClawModel model;
    auto original = model.settings();
    std::string error;
    assert(model.update_settings(original, error));
    const auto file = environment.root + "/ui-settings.conf";
    const auto read_settings = [&]() {
        std::ifstream input(file);
        return std::string((std::istreambuf_iterator<char>(input)), {});
    };
    const auto saved = read_settings();
    const int lock = ::open((runtime_home + "/.update.lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0);
    auto modified = original;
    modified.binary_path = "/tmp/a-different-runtime";
    modified.arguments = "--debug";
    assert(!model.update_settings(modified, error));
    assert(error.find("updating") != std::string::npos);
    assert(model.settings().binary_path == original.binary_path);
    assert(read_settings() == saved);
    ::close(lock);
    assert(model.update_settings(modified, error));
    assert(model.settings().binary_path == modified.binary_path);
}

void test_model_conversation_and_status()
{
    PicoClawModel model;
    std::string error;
    assert(!model.begin_prompt("  ", error));
    assert(model.begin_prompt(" Hello PicoClaw ", error));
    assert(model.messages().size() == 2);
    assert(model.messages()[0].text == "Hello PicoClaw");
    assert(model.messages()[1].pending);
    model.complete_prompt(" Hello from the assistant.\n", true);
    assert(!model.messages()[1].pending);
    assert(model.messages()[1].text == "Hello from the assistant.");

    // A new chat preserves the old transcript and keeps model/runtime settings.
    assert(model.new_conversation());
    assert(model.messages().empty());
    assert(!model.new_conversation());

    for (std::size_t index = 0; index < PicoClawModel::maximum_messages; ++index) {
        assert(model.begin_prompt("next", error));
        model.complete_prompt("done", true);
    }
    assert(model.messages().size() == 2 * PicoClawModel::maximum_messages);

    model.apply_runtime_status(
        "installed=yes\nversion=picoclaw 0.3.1\ngateway=running\nnetwork=ready\n"
        "config=ready\nmodel_key=ready\nurl=http://192.0.2.5:18790\n"
        "web_url=http://192.0.2.5:18800\n"
        "model_name=gpt-4o-mini\nmodel_id=openai/gpt-4o-mini\n"
        "api_base=https://api.openai.com/v1\n");
    assert(model.runtime().installed);
    assert(model.runtime().gateway_running);
    assert(model.runtime().network_ready);
    assert(model.runtime().model_key_ready);
    assert(model.runtime().version == "picoclaw 0.3.1");
    assert(model.runtime().web_url == "http://192.0.2.5:18800");
    assert(model.active_profile_index() == 2);

    model.apply_runtime_status(
        "installed=yes\nnetwork=missing\nconfig=ready\nmodel_key=ready\n"
        "model_name=llama3.2\nmodel_id=ollama/llama3.2\n"
        "api_base=http://127.0.0.1:11434/v1\n");
    assert(model.runtime().network_ready);
    assert(model.runtime().model_key_ready);
}

void test_websocket_status_uses_configured_gateway_host()
{
    TestEnvironment environment;
    environment.set("PICOCLAW_TRANSPORT", "websocket");
    PicoClawModel model;
    auto settings = model.settings();
    settings.host = "192.168.8.197";
    std::string error;
    assert(model.update_settings(settings, error));

    // The helper may report its local discovery placeholder while the SDL
    // simulator connects to a remote WebSocket gateway. The UI endpoint must
    // follow the configured host in that transport mode.
    model.apply_runtime_status("installed=yes\ngateway=running\nurl=http://device:18790\n");
    assert(model.runtime().url == "http://192.168.8.197:18790");
}

void test_model_settings_use_runtime_config_directory()
{
    TestEnvironment environment;
    environment.unset("PICOCLAW_UI_SETTINGS");
    const auto config_directory = environment.root + "/board-config";
    environment.set("PICOCLAW_CONFIG_DIR", config_directory);
    environment.set("XDG_CONFIG_HOME", environment.root + "/unused-xdg-config");
    std::filesystem::create_directory(config_directory);
    const auto settings_path = config_directory + "/ui-settings.conf";
    std::ofstream(settings_path) << "host=board.local\nport=18801\nlanguage=zh\n";
    PicoClawModel model;
    assert(model.settings().host == "board.local");
    assert(model.settings().port == 18801);
    PicoClawSettings settings = model.settings();
    settings.arguments = "--debug";
    std::string error;
    assert(model.update_settings(settings, error));
    PicoClawModel reloaded;
    assert(reloaded.settings().arguments == "--debug");
    assert(!std::filesystem::exists(environment.root + "/unused-xdg-config"));

    const auto explicit_path = environment.root + "/explicit-settings.conf";
    std::ofstream(explicit_path) << "host=override.local\nport=18802\n";
    environment.set("PICOCLAW_UI_SETTINGS", explicit_path);
    PicoClawModel overridden;
    assert(overridden.settings().host == "override.local");
    assert(overridden.settings().port == 18802);
}

void test_async_client()
{
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        PicoClawRuntimeResult result;
        result.action    = request.action;
        result.exit_code = 0;
        result.output    = request.arguments.empty() ? "status" : request.arguments.front();
        return result;
    });
    assert(client.submit(PicoClawRuntimeAction::Ask, {"hello"}));
    const PicoClawRuntimeResult result = wait_for_result(client);
    assert(result.action == PicoClawRuntimeAction::Ask);
    assert(result.output == "hello");
    assert(!client.busy());
}

void test_client_preserves_completed_results_until_polled()
{
    PicoClawClient client([](const PicoClawRuntimeRequest &request) {
        return PicoClawRuntimeResult{request.action, 0, "Completed chat response"};
    });
    assert(client.submit(PicoClawRuntimeAction::Ask, {"hello"}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(client.busy());
    assert(!client.submit(PicoClawRuntimeAction::CheckUpdates));
    const auto result = wait_for_result(client);
    assert(result.action == PicoClawRuntimeAction::Ask && result.output == "Completed chat response");
}

void test_history_persistence()
{
    TestEnvironment environment;
    std::string error;
    PicoClawModel model;
    const auto first_id = model.active_conversation().id;
    assert(model.begin_prompt("第一段对话\nwith quotes \" and \\", error));
    assert(!model.new_conversation());
    model.complete_prompt("## Markdown\n\n```sh\necho 'hello'\n```", true);
    const auto first_text = model.messages()[1].text;
    assert(model.new_conversation());
    const auto second_id = model.active_conversation().id;
    assert(first_id != second_id);
    assert(model.conversations().size() == 2);
    assert(!model.new_conversation()); // Do not accumulate empty sessions.
    assert(model.begin_prompt("Second question", error));
    assert(!model.select_conversation(0));
    model.complete_prompt("Second answer", true);
    assert(model.select_conversation(0));
    assert(model.messages()[1].text == first_text);
    assert(model.active_conversation().id == first_id);

    PicoClawModel loaded;
    assert(loaded.history_error().empty());
    assert(loaded.conversations().size() == 2);
    assert(loaded.active_conversation().id == first_id);
    assert(loaded.messages()[1].text == first_text);
    assert(loaded.select_conversation(1));
    assert(loaded.messages()[1].text == "Second answer");
    assert(loaded.active_conversation().id == second_id);
    assert(loaded.begin_prompt("Pending at shutdown", error));
    PicoClawModel interrupted;
    assert(!interrupted.messages().back().pending);
    assert(interrupted.messages().back().text.find("interrupted") != std::string::npos);

    const auto path = environment.root + "/history/conversations.dat";
    struct stat mode{};
    assert(::stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
    std::ofstream(path) << "broken archive";
    PicoClawModel corrupt;
    assert(!corrupt.history_error().empty());
    assert(!corrupt.begin_prompt("Do not overwrite old records", error));
    std::ifstream input(path);
    std::string text;
    std::getline(input, text);
    assert(text == "broken archive");
}

void test_history_save_failure_preserves_active_conversation()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    assert(model.begin_prompt("Keep this conversation", error));
    model.complete_prompt("Saved answer", true);
    const auto id = model.active_conversation().id;
    const auto blocked = environment.root + "/not-a-directory";
    std::ofstream(blocked) << "blocked";
    ::setenv("PICOCLAW_HISTORY_DIR", blocked.c_str(), 1);
    assert(!model.new_conversation());
    assert(model.active_conversation().id == id);
    assert(model.conversations().size() == 1);
    assert(model.messages().back().text == "Saved answer");
    assert(!model.begin_prompt("Must not discard saved messages", error));
    assert(model.messages().size() == 2);
    assert(!error.empty());
}

void test_client_session_and_prompt_transport()
{
    TestEnvironment environment;
    const auto helper = environment.root + "/helper.sh";
    std::ofstream(helper) << "#!/bin/sh\nprintf '%s\\n' \"$@\"\ncat\n";
    ::setenv("LILYGO_PICOCLAW_CONTROL", helper.c_str(), 1);
    {
        PicoClawClient client;
        assert(client.submit(PicoClawRuntimeAction::Ask, {"literal $value and `code`", "agent:main:ui:session-test"}));
        const auto result = wait_for_result(client);
        assert(result.exit_code == 0);
        assert(result.output == "ask\n--prompt-stdin\nagent:main:ui:session-test\nliteral $value and `code`");
        assert(client.submit(PicoClawRuntimeAction::Ask,
                             {"literal $value and `code`", "agent:main:ui:session-test", "selected-model"}));
        const auto selected = wait_for_result(client);
        assert(selected.exit_code == 0);
        assert(selected.output == "ask\n--prompt-stdin\nagent:main:ui:session-test\nselected-model\nliteral $value and `code`");
    }
    ::unsetenv("LILYGO_PICOCLAW_CONTROL");
}

void test_concurrent_clients_do_not_inherit_each_others_pipes()
{
    TestEnvironment environment;
    const auto helper = environment.root + "/concurrent-helper.sh";
    const auto started = environment.root + "/started";
    const auto release = environment.root + "/release";
    std::ofstream(helper) << "#!/bin/sh\nif [ \"$1\" = status ]; then\n"
                         << "touch '" << started << "'\nwhile [ ! -f '" << release << "' ]; do sleep 0.01; done\n"
                         << "else\nexec python3 -c '\n"
                         << "import os, stat, sys\n"
                         << "pipes = []\n"
                         << "for name in os.listdir(\"/dev/fd\"):\n"
                         << "    fd = int(name)\n"
                         << "    if fd <= 2: continue\n"
                         << "    try:\n"
                         << "        if stat.S_ISFIFO(os.fstat(fd).st_mode): pipes.append(fd)\n"
                         << "    except OSError: pass\n"
                         << "print(\"isolated\" if not pipes else \"inherited pipe\")\n"
                         << "sys.exit(bool(pipes))\n'\nfi\n";
    environment.set("LILYGO_PICOCLAW_CONTROL", helper);
    PicoClawClient background, foreground;
    assert(background.submit(PicoClawRuntimeAction::Status));
    for (int attempt = 0; attempt < 200 && !std::filesystem::exists(started); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(std::filesystem::exists(started));
    assert(foreground.submit(PicoClawRuntimeAction::Logs));
    const auto result = wait_for_result(foreground);
    std::ofstream(release) << "done";
    assert(wait_for_result(background).exit_code == 0);
    assert(result.exit_code == 0 && result.output == "isolated\n");
}

void test_release_list_is_complete_and_preserves_selection()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    const std::string header = "updates=ready\nlatest_stable=v0.3.1\nupdate_status=available\n"
                               "update_message=A new stable release is available.\n";
    const std::string records = "release=nightly\tNightly\tnightly\t2026-09-14\tyes\n"
                                "release=v0.3.1\tStable release\tstable\t2026-09-12\tyes\n"
                                "release=v0.2.0-rc1\tPreview\tprerelease\t2026-08-01\tno\n";
    assert(!model.selected_release());
    assert(model.apply_update_result(header + records + "end_releases=yes\n", error));
    assert(model.updates_loaded() && model.releases().size() == 3);
    assert(model.selected_release()->tag == "v0.3.1");
    assert(!model.releases()[2].installable);
    assert(model.select_release(0));
    assert(!model.select_release(99));
    const auto revision = model.revision();
    for (const auto &invalid : {
             header + records,
             header + records + "release=bad\tIncomplete\nend_releases=yes\n",
             header + records + "release=nightly\tDuplicate\tnightly\t2026-09-14\tyes\nend_releases=yes\n",
             header + records + "end_releases=yes\nrelease=extra\tLate\tstable\t2026-01-01\tyes\n",
         }) {
        assert(!model.apply_update_result(invalid, error));
        assert(!error.empty());
        assert(model.revision() == revision);
        assert(model.releases().size() == 3 && model.selected_release()->tag == "nightly");
    }
    const std::string reordered = "release=v0.3.1\tStable\tstable\t2026-09-12\tyes\n"
                                  "release=nightly\tNew nightly\tnightly\t2026-09-15\tyes\n";
    assert(model.apply_update_result(header + reordered + "end_releases=yes\n", error));
    assert(model.selected_release_index() == 1 && model.selected_release()->tag == "nightly");
    assert(model.apply_update_result("updates=ready\nlatest_stable=\nupdate_status=unknown\n"
                                     "update_message=No published releases.\nend_releases=yes\n", error));
    assert(model.updates_loaded() && model.releases().empty() && !model.selected_release());
}

void test_update_client_commands_and_output_limit()
{
    TestEnvironment environment;
    const auto helper = environment.root + "/updates.sh";
    const auto data = environment.root + "/response.txt";
    std::ofstream(helper) << "#!/bin/sh\nif [ \"$1\" = install-release ] && [ \"$2\" != long-output ]; then\nprintf '%s\\n' \"$@\"\n"
                            "else\ncat '" << data << "'\nfi\n";
    environment.set("LILYGO_PICOCLAW_CONTROL", helper);
    PicoClawClient client;
    assert(client.submit(PicoClawRuntimeAction::InstallRelease, {"v0.3.1"}));
    auto result = wait_for_result(client);
    assert(result.output == "install-release\nv0.3.1\n");
    std::ofstream(data) << std::string(20000, 'x');
    assert(client.submit(PicoClawRuntimeAction::CheckUpdates));
    result = wait_for_result(client);
    assert(result.exit_code == 0 && result.output.size() == 20000);
    assert(client.submit(PicoClawRuntimeAction::Logs));
    result = wait_for_result(client);
    assert(result.output.size() == 16U * 1024U);
    std::ofstream(data) << std::string(1024U * 1024U + 1, 'x');
    assert(client.submit(PicoClawRuntimeAction::CheckUpdates));
    result = wait_for_result(client);
    assert(result.exit_code != 0 && result.output.find("exceeded the 1 MiB limit") != std::string::npos);
    std::ofstream(data) << std::string(60000, 'x') << "\nRollback failed: manual recovery required.\n";
    assert(client.submit(PicoClawRuntimeAction::InstallRelease, {"long-output"}));
    result = wait_for_result(client);
    assert(result.output.size() == 16U * 1024U);
    assert(result.output.find("Rollback failed") != std::string::npos);
}


std::string channel_fixture()
{
    return "PICOCLAW_CHANNELS 1 2\n"
           "\"telegram\" 0 4\n"
           "\"token\" \"\"\n\"base_url\" \"\"\n\"proxy\" \"\"\n\"allow_from\" \"12345,67890\"\n"
           "\"discord\" 0 4\n"
           "\"token\" \"saved-discord-secret\"\n\"proxy\" \"\"\n\"mention_only\" \"true\"\n\"allow_from\" \"\"\n"
           "END_CHANNELS\n";
}

void test_channels_load_atomically_and_keep_editor_drafts()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error, payload;
    model.set_output("Existing runtime log");
    assert(!model.channels_loaded() && !model.channel_draft());
    assert(!model.channel_save_payload(payload, error));
    assert(model.apply_channels_result(channel_fixture(), error));
    assert(model.channels_loaded() && model.channels().size() == 2);
    assert(model.channels()[0].id == "discord" && model.channels()[1].id == "telegram");
    assert(model.channels()[0].fields[2].kind == PicoClawChannelFieldKind::Boolean);
    assert(model.edit_channel(1));
    assert(!model.channel_dirty());
    assert(model.set_channel_enabled(true));
    assert(!model.channel_save_payload(payload, error));
    assert(error == "Bot token is required to enable this channel.");
    const std::string secret = "bot-secret-\"quoted\\value";
    assert(model.set_channel_field(0, secret));
    assert(model.channel_dirty());
    assert(model.channel_save_payload(payload, error));
    assert(payload.find("\"enabled\":true") != std::string::npos);
    assert(payload.find("bot-secret-\\\"quoted\\\\value") != std::string::npos);
    assert(payload.find("\"allow_from\":\"12345,67890\"") != std::string::npos);
    assert(model.show(PicoClawScreen::Channels));
    assert(model.show(PicoClawScreen::Chat));
    assert(model.show(PicoClawScreen::Channels));
    assert(model.channel_draft()->fields[0].value == secret);
    const auto saved = *model.channel_draft();
    assert(model.set_channel_field(0, "newer-secret"));
    model.complete_channel_save(saved);
    assert(model.channels()[1].enabled && model.channels()[1].fields[0].value == secret);
    assert(model.channel_draft()->fields[0].value == "newer-secret" && model.channel_dirty());
    assert(model.channels_restart_required());
    assert(model.channels_status().find("Newer edits are unsaved") != std::string::npos);
    const auto previous_revision = model.channels_revision();
    for (const auto &bad : {channel_fixture().substr(0, channel_fixture().size() - 5),
                           channel_fixture() + "secret garbage", std::string("PICOCLAW_CHANNELS 1 0\nEND_CHANNELS\n"),
                           std::string("PICOCLAW_CHANNELS 1 1\n\"telegram\" 0 0\nEND_CHANNELS\n")}) {
        assert(!model.apply_channels_result(bad, error));
        assert(model.channels()[1].fields[0].value == secret);
        assert(model.channel_draft()->fields[0].value == "newer-secret");
        assert(model.channels_revision() == previous_revision);
    }
    assert(model.apply_channels_result(channel_fixture(), error));
    assert(model.channel_draft()->fields[0].value == "newer-secret");
    assert(model.channel_dirty());
    assert(model.runtime().output == "Existing runtime log");
    model.close_channel();
    assert(!model.channel_draft() && !model.channel_dirty());
    assert(model.edit_channel(1));
    assert(model.channel_draft()->fields[0].value.empty());
    assert(model.channel_save_payload(payload, error)); // Disabled channels may omit credentials.
    assert(!model.edit_channel(99));
}

void test_channel_validation_and_wire_integrity()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error, payload;
    assert(model.apply_channels_result(channel_fixture(), error));
    assert(model.edit_channel(0));
    assert(model.set_channel_field(2, "yes"));
    assert(!model.channel_save_payload(payload, error));
    assert(model.set_channel_field(2, "false"));
    assert(model.set_channel_field(0, "first\nsecond"));
    assert(!model.channel_save_payload(payload, error));
    assert(model.set_channel_field(0, std::string(4097, 'x')));
    assert(!model.channel_save_payload(payload, error));
    model.close_channel();
    assert(model.edit_channel(1));
    assert(model.set_channel_enabled(true));
    assert(model.set_channel_field(0, "token"));
    assert(model.set_channel_field(1, "ftp://example.test"));
    assert(!model.channel_save_payload(payload, error));
    assert(model.set_channel_field(1, "https://example.test/bot"));
    assert(model.channel_save_payload(payload, error));
    auto corrupt = channel_fixture();
    const auto true_at = corrupt.find("\"true\"");
    corrupt.replace(true_at, 6, "\"yes\"");
    assert(!model.apply_channels_result(corrupt, error));
    corrupt = channel_fixture();
    const auto proxy_at = corrupt.find("\"proxy\"");
    corrupt.replace(proxy_at, 7, "\"token\"");
    assert(!model.apply_channels_result(corrupt, error));
    const std::string line = "PICOCLAW_CHANNELS 1 1\n\"line\" 0 6\n"
        "\"channel_secret\" \"\"\n\"channel_access_token\" \"\"\n\"webhook_host\" \"0.0.0.0\"\n"
        "\"webhook_port\" \"18791\"\n\"webhook_path\" \"/webhook/line\"\n\"allow_from\" \"\"\nEND_CHANNELS\n";
    model.close_channel();
    assert(model.apply_channels_result(line, error));
    assert(model.edit_channel(0));
    for (const auto *port : {"", "0", "65536", "abc", "-1", "9999999999999"}) {
        assert(model.set_channel_field(3, port));
        assert(!model.channel_save_payload(payload, error));
    }
    assert(model.set_channel_field(3, "65535"));
    assert(model.channel_save_payload(payload, error));
    assert(model.set_channel_field(4, "relative/webhook"));
    assert(!model.channel_save_payload(payload, error));
    assert(error == "Webhook path must start with /.");
}


std::string weixin_channel_fixture()
{
    return "PICOCLAW_CHANNELS 1 1\n\"weixin\" 0 6\n"
        "\"token\" \"old-private-token\"\n\"account_id\" \"old-account\"\n"
        "\"base_url\" \"https://ilinkai.weixin.qq.com/\"\n\"cdn_base_url\" \"https://novac2c.cdn.weixin.qq.com/c2c\"\n"
        "\"proxy\" \"http://127.0.0.1:7890\"\n\"allow_from\" \"12345\"\nEND_CHANNELS\n";
}

std::string binding_response(std::initializer_list<std::pair<std::string, std::string>> fields)
{
    std::ostringstream output;
    output << "PICOCLAW_WEIXIN_BINDING 1\n";
    for (const auto &field : fields) output << field.first << ' ' << std::quoted(field.second) << '\n';
    output << "END_PICOCLAW_WEIXIN_BINDING\n";
    return output.str();
}

void test_weixin_binding_state_and_private_draft_update()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error, payload;
    assert(!model.begin_weixin_binding());
    assert(model.apply_channels_result(weixin_channel_fixture(), error));
    assert(model.edit_channel(0));
    model.set_output("Existing safe activity");
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Idle);
    assert(model.weixin_binding().account_id == "old-account");
    assert(model.begin_weixin_binding());
    assert(model.weixin_binding().active());
    assert(model.weixin_binding_start_payload().find("http://127.0.0.1:7890") != std::string::npos);
    const auto start = binding_response({{"status", "wait"}, {"qrcode", "opaque-session"},
        {"qr_url", "https://weixin.qq.com/qr/real-login"}, {"base_url", "https://ilinkai.weixin.qq.com/"}});
    assert(model.apply_weixin_binding_result(start, error));
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Waiting);
    assert(model.weixin_binding().qr_url == "https://weixin.qq.com/qr/real-login");
    assert(model.weixin_binding_poll_payload().find("\"qrcode\":\"opaque-session\"") != std::string::npos);
    assert(model.apply_weixin_binding_result(binding_response({{"status", "scaned"}}), error));
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Scanned);
    assert(model.apply_weixin_binding_result(binding_response({{"status", "scaned_but_redirect"},
        {"base_url", "https://redirect.weixin.qq.com/"}, {"redirect_host", "redirect.weixin.qq.com"}}), error));
    assert(model.weixin_binding_poll_payload().find("https://redirect.weixin.qq.com/") != std::string::npos);
    assert(model.set_channel_field(5, "draft-edited-user"));
    const std::string token = "new-private-\"token\\value";
    assert(!model.apply_weixin_binding_result(binding_response({{"status", "confirmed"}, {"token", token}}), error));
    assert(model.channel_draft()->fields[0].value == "old-private-token");
    assert(model.apply_weixin_binding_result(binding_response({{"status", "confirmed"}, {"token", token},
        {"account_id", "new-account"}}), error));
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Confirmed && !model.weixin_binding().active());
    assert(model.weixin_binding().qr_url.empty() && model.weixin_binding().account_id == "new-account");
    assert(model.channel_draft()->fields[0].value == token);
    assert(model.channel_draft()->fields[1].value == "new-account");
    assert(model.channel_draft()->fields[2].value == "https://redirect.weixin.qq.com/");
    assert(model.channel_draft()->fields[5].value == "draft-edited-user");
    assert(model.channels()[0].fields[0].value == "old-private-token" && model.channel_dirty());
    assert(model.runtime().output == "Existing safe activity");
    assert(model.weixin_binding().message.find(token) == std::string::npos);
    assert(model.channels_status().find(token) == std::string::npos);
    assert(model.channel_save_payload(payload, error));
    assert(payload.find("new-private-\\\"token\\\\value") != std::string::npos);
    assert(!model.apply_weixin_binding_result(start, error));
    model.close_channel();
    assert(model.edit_channel(0));
    assert(model.channel_draft()->fields[0].value == "old-private-token");
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Idle);
    assert(model.weixin_binding().account_id == "old-account");
}

void test_weixin_binding_expiry_cancel_and_malformed_responses()
{
    TestEnvironment environment;
    PicoClawModel model;
    std::string error;
    assert(model.apply_channels_result(weixin_channel_fixture(), error) && model.edit_channel(0));
    assert(model.begin_weixin_binding());
    const auto start = binding_response({{"status", "wait"}, {"qrcode", "opaque"},
        {"qr_url", "https://weixin.qq.com/qr/login"}});
    for (const auto &bad : {start.substr(0, start.size() - 5), start + "unexpected",
             binding_response({{"status", "wait"}, {"qrcode", "opaque"}, {"qr_url", "file:///private"}}),
             binding_response({{"status", "confirmed"}, {"token", "secret"}, {"account_id", "account"}}),
             binding_response({{"status", "wait"}, {"status", "wait"}})}) {
        assert(!model.apply_weixin_binding_result(bad, error));
        assert(model.weixin_binding().state == PicoClawWeixinBindingState::Starting);
        assert(model.channel_draft()->fields[0].value == "old-private-token");
    }
    assert(model.apply_weixin_binding_result(start, error));
    assert(!model.apply_weixin_binding_result(binding_response({{"status", "wait"}, {"qrcode", "other-session"}}), error));
    assert(model.apply_weixin_binding_result(binding_response({{"status", "expired"}}), error));
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Expired && model.weixin_binding().qr_url.empty());
    assert(model.begin_weixin_binding());
    assert(model.apply_weixin_binding_result(start, error));
    model.cancel_weixin_binding();
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Cancelled && !model.weixin_binding().active());
    assert(model.weixin_binding().qr_url.empty());
    assert(!model.apply_weixin_binding_result(binding_response({{"status", "confirmed"}, {"token", "late-secret"},
        {"account_id", "late-account"}}), error));
    assert(model.channel_draft()->fields[0].value == "old-private-token");
    assert(model.begin_weixin_binding());
    model.fail_weixin_binding();
    assert(model.weixin_binding().state == PicoClawWeixinBindingState::Failed);
    assert(model.channel_draft()->fields[0].value == "old-private-token");
    assert(model.set_channel_field(0, ""));
    assert(model.set_channel_enabled(true));
    std::string payload;
    assert(!model.channel_save_payload(payload, error));
    assert(error == "Bind your Weixin account before enabling this channel.");
}

}  // namespace

int main()
{
    TestEnvironment environment;
    environment.set("PICOCLAW_HOME", environment.root + "/runtime");
    test_model_settings_persistence();
    test_settings_save_respects_update_lock();
    test_model_settings_use_runtime_config_directory();
    test_model_profiles();
    test_model_navigation();
    test_channels_load_atomically_and_keep_editor_drafts();
    test_channel_validation_and_wire_integrity();
    test_weixin_binding_state_and_private_draft_update();
    test_weixin_binding_expiry_cancel_and_malformed_responses();
    test_model_registry_persistence_and_failure_recovery();
    test_model_registry_validation();
    test_generated_model_aliases_survive_provider_edits_and_persistence();
    test_generated_model_aliases_remain_valid_at_field_limits();
    test_runtime_status_preserves_conflicting_saved_model();
    test_internal_alias_collisions_can_be_saved();
    test_raw_model_ids_and_provider_metadata_survive_registry_round_trip();
    test_registry_v1_migration_and_import_infer_only_provider_metadata();
    test_model_registry_import_is_atomic_and_preserves_existing_models();
    test_model_conversation_and_status();
    test_websocket_status_uses_configured_gateway_host();
    test_async_client();
    test_client_preserves_completed_results_until_polled();
    test_history_persistence();
    test_history_save_failure_preserves_active_conversation();
    test_client_session_and_prompt_transport();
    test_concurrent_clients_do_not_inherit_each_others_pipes();
    test_release_list_is_complete_and_preserves_selection();
    test_update_client_commands_and_output_limit();
    return 0;
}
