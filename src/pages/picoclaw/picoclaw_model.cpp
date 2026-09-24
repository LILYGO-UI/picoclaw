#include "pages/picoclaw/picoclaw_model.hpp"
#include "domain/picoclaw_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sstream>
#include <set>
#include <utility>

namespace {

std::string trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U]))) --last;
    return std::string(value.substr(first, last - first));
}

std::string status_value(std::string_view output, std::string_view key)
{
    std::size_t offset = 0;
    while (offset < output.size()) {
        const std::size_t end = output.find('\n', offset);
        const std::string_view line =
            output.substr(offset, end == std::string_view::npos ? output.size() - offset : end - offset);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
            return std::string(line.substr(key.size() + 1U));
        }
        if (end == std::string_view::npos) break;
        offset = end + 1U;
    }
    return {};
}

bool valid_model_name(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) || character == '-' || character == '_' || character == '.';
    });
}

bool valid_provider(std::string_view provider)
{
    return provider == "openai" || provider == "anthropic" || provider == "gemini" || provider == "deepseek" ||
           provider == "ollama";
}

bool equal_ascii_case(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

bool normalize_profile(PicoClawProfile &profile, std::string &error)
{
    if (!profile.provider.empty() && !valid_provider(profile.provider)) {
        error = "Choose a supported model provider.";
        return false;
    }
    for (const auto *field : {&profile.title, &profile.model_name, &profile.model_id, &profile.api_base, &profile.api_key}) {
        if (std::any_of(field->begin(), field->end(), [](char character) {
                return character == '\r' || character == '\n' || character == '\0';
            })) {
            error = "Model fields cannot contain line breaks or null characters.";
            return false;
        }
    }
    if (profile.title.size() > 255U || profile.model_name.size() > 255U || profile.model_id.size() > 255U ||
        profile.api_base.size() > 255U || profile.api_key.size() > 511U) {
        error = "Model fields are limited to 255 bytes; API keys are limited to 511 bytes.";
        return false;
    }
    profile.title      = trim(profile.title);
    profile.model_name = trim(profile.model_name);
    profile.model_id   = trim(profile.model_id);
    profile.api_base   = trim(profile.api_base);
    profile.api_key    = trim(profile.api_key);
    if (profile.title.empty() || profile.model_id.empty() || profile.api_base.empty()) {
        error = "Provider name, model ID, and API base are required.";
        return false;
    }
    if (!valid_model_name(profile.model_name)) {
        error = "Model alias may contain only letters, numbers, dots, dashes, and underscores.";
        return false;
    }
    if (profile.api_base.rfind("https://", 0) != 0 && profile.api_base.rfind("http://", 0) != 0) {
        error = "API base must start with http:// or https://.";
        return false;
    }
    if (profile.provider.empty()) profile.provider = PicoClawModel::profile_provider(profile);
    return true;
}

bool unique_profile_alias(const std::vector<PicoClawProfile> &profiles, const std::string &alias,
                          std::size_t excluded_index, std::string &error)
{
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (index != excluded_index && profiles[index].model_name == alias) {
            error = "Model alias is already in use. Choose a unique alias.";
            return false;
        }
    }
    return true;
}

std::string generated_profile_alias(std::string_view model_id, const std::vector<PicoClawProfile> &profiles)
{
    std::string base;
    for (const unsigned char character : trim(model_id)) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.') {
            base.push_back(static_cast<char>(character));
        } else if (!base.empty() && base.back() != '-') {
            base.push_back('-');
        }
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "model";
    if (base.size() > 255U) base.resize(255U);

    std::string alias = base;
    for (std::size_t suffix = 2; std::any_of(profiles.begin(), profiles.end(), [&alias](const PicoClawProfile &profile) {
             return profile.model_name == alias;
         }); ++suffix) {
        const std::string ending = "-" + std::to_string(suffix);
        alias = base.substr(0, 255U - ending.size()) + ending;
    }
    return alias;
}

bool local_endpoint(std::string_view value)
{
    return value.find("://localhost") != std::string_view::npos ||
           value.find("://127.0.0.1") != std::string_view::npos || value.find("://[::1]") != std::string_view::npos ||
           value.find("://ollama") != std::string_view::npos;
}

std::filesystem::path settings_path()
{
    if (const char *override_path = std::getenv("PICOCLAW_UI_SETTINGS"); override_path && *override_path)
        return override_path;
    if (const char *config_dir = std::getenv("PICOCLAW_CONFIG_DIR"); config_dir && *config_dir)
        return std::filesystem::path(config_dir) / "ui-settings.conf";
    const char *config_home = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path root;
    if (config_home && *config_home) root = config_home;
    if (root.empty()) {
        const char *home = std::getenv("HOME");
        root = home && *home ? std::filesystem::path(home) / ".config" : std::filesystem::path("/tmp");
    }
    return root / "lilygo" / "ui" / "picoclaw" / "ui-settings.conf";
}

std::filesystem::path profiles_path()
{
    if (const char *override_path = std::getenv("PICOCLAW_MODELS_PATH"); override_path && *override_path)
        return override_path;
    if (const char *settings_override = std::getenv("PICOCLAW_UI_SETTINGS"); settings_override && *settings_override)
        return std::filesystem::path(settings_override).parent_path() / "models.conf";
    if (const char *config_dir = std::getenv("PICOCLAW_CONFIG_DIR"); config_dir && *config_dir)
        return std::filesystem::path(config_dir) / "models.conf";
    const char *config_home = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path root;
    if (config_home && *config_home) root = config_home;
    if (root.empty()) {
        const char *home = std::getenv("HOME");
        root = home && *home ? std::filesystem::path(home) / ".config" : std::filesystem::path("/tmp");
    }
    return root / "lilygo" / "ui" / "picoclaw" / "models.conf";
}

bool parse_bool(std::string_view value)
{
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::filesystem::path history_path()
{
    if (const char *path = std::getenv("PICOCLAW_HISTORY_DIR"); path && *path)
        return std::filesystem::path(path) / "conversations.dat";
    if (const char *path = std::getenv("PICOCLAW_STATE_DIR"); path && *path)
        return std::filesystem::path(path) / "conversations.dat";
    std::filesystem::path root;
    if (const char *path = std::getenv("XDG_STATE_HOME"); path && *path) root = path;
    if (root.empty()) {
        const char *home = std::getenv("HOME");
        root = home && *home ? std::filesystem::path(home) / ".local/state" : std::filesystem::path("/tmp");
    }
    return root / "lilygo/ui/picoclaw/conversations.dat";
}

bool write_history_atomic(const std::string &data)
{
    const auto path = history_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::string temporary = path.string() + ".XXXXXX";
    const int fd = ::mkstemp(temporary.data()); // private mode 0600 from creation
    if (fd < 0) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool synced = offset == data.size() && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (synced && closed) std::filesystem::rename(temporary, path, ec);
    if (!synced || !closed || ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool write_profiles_atomic(const std::string &data)
{
    const auto path = profiles_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::string temporary = path.string() + ".XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool synced = offset == data.size() && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (synced && closed) std::filesystem::rename(temporary, path, ec);
    if (!synced || !closed || ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

void load_settings(PicoClawSettings &settings)
{
    std::ifstream input(settings_path());
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = trim(std::string_view(line).substr(0, split));
        const std::string value = trim(std::string_view(line).substr(split + 1U));
        if (key == "host" && !value.empty()) settings.host = value;
        else if (key == "port") {
            try { settings.port = std::stoi(value); } catch (...) {}
        } else if (key == "binary_path") settings.binary_path = value;
        else if (key == "arguments") settings.arguments = value;
        else if (key == "public_mode") settings.public_mode = parse_bool(value);
        // Theme keys from older releases are intentionally ignored. The UI
        // now has one fixed iOS light appearance; all other settings migrate.
        else if (key == "language" && !value.empty()) settings.language = value;
    }
    if (settings.port < 1 || settings.port > 65535) settings.port = 18800;
}

bool write_settings(const PicoClawSettings &settings, std::string &error)
{
    const auto path = settings_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Could not create settings directory.";
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        error = "Could not open settings file.";
        return false;
    }
    output << "host=" << settings.host << '\n'
           << "port=" << settings.port << '\n'
           << "binary_path=" << settings.binary_path << '\n'
           << "arguments=" << settings.arguments << '\n'
           << "public_mode=" << (settings.public_mode ? "true" : "false") << '\n'
           << "language=" << settings.language << '\n';
    output.close();
    if (!output) {
        error = "Could not write settings file.";
        return false;
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        error = "Could not commit settings file.";
        return false;
    }
    return true;
}

class RuntimeSettingsLock {
public:
    RuntimeSettingsLock() = default;
    RuntimeSettingsLock(const RuntimeSettingsLock &) = delete;
    RuntimeSettingsLock &operator=(const RuntimeSettingsLock &) = delete;
    ~RuntimeSettingsLock()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    bool acquire(std::string &error)
    {
        std::filesystem::path runtime_home;
        if (const char *path = std::getenv("PICOCLAW_HOME"); path && *path) runtime_home = path;
        if (runtime_home.empty()) {
            std::filesystem::path data_home;
            if (const char *path = std::getenv("XDG_DATA_HOME"); path && *path) data_home = path;
            if (data_home.empty()) {
                const char *home = std::getenv("HOME");
                data_home = std::filesystem::path(home && *home ? home : "/tmp") / ".local/share";
            }
            runtime_home = data_home / "lilygo/ui/picoclaw";
        }
        std::error_code ec;
        std::filesystem::create_directories(runtime_home, ec);
        if (ec) {
            error = "Could not create the PicoClaw runtime directory.";
            return false;
        }
        fd_ = ::open((runtime_home / ".update.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            error = "Could not open the PicoClaw update lock.";
            return false;
        }
        int result;
        do {
            result = ::flock(fd_, LOCK_EX | LOCK_NB);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            error = errno == EWOULDBLOCK || errno == EAGAIN
                        ? "PicoClaw is updating. Save settings after the update finishes."
                        : "Could not lock PicoClaw settings.";
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

bool valid_host(std::string_view host)
{
    if (host.empty() || host.size() > 253) return false;
    return std::all_of(host.begin(), host.end(), [](char c) {
        return !std::isspace(static_cast<unsigned char>(c)) && c != '=';
    });
}


PicoClawChannelField channel_field(std::string key, std::string label, PicoClawChannelFieldKind kind,
                                  bool required = false, std::string help = {},
                                  std::string section = "Configuration")
{
    return {std::move(key), std::move(label), std::move(help), {}, kind, required, std::move(section)};
}

std::vector<PicoClawChannel> channel_catalog()
{
    using Kind = PicoClawChannelFieldKind;
    const auto allow = channel_field("allow_from", "Allowed users", Kind::List, false,
                                    "Separate user IDs with commas. Leave empty to use the default access policy.", "access");
    return {
        {"feishu", "Feishu / Lark", "Connect a Feishu or Lark bot using your app credentials.", false,
            {channel_field("app_id", "App ID", Kind::Text, true, {}, "credentials"),
             channel_field("app_secret", "App secret", Kind::Secret, true, {}, "credentials"),
             channel_field("encrypt_key", "Encrypt key", Kind::Secret, false, {}, "security"),
             channel_field("verification_token", "Verification token", Kind::Secret, false, {}, "security"),
             channel_field("is_lark", "Use Lark", Kind::Boolean, false, {}, "behavior"), allow}},
        {"weixin", "Weixin", "Scan a QR code with Weixin to connect your account.", false,
            {channel_field("token", "Token", Kind::Secret, true, {}, "credentials"),
             channel_field("account_id", "Account ID", Kind::Text, false, {}, "credentials"),
             channel_field("base_url", "API base URL", Kind::Text, false, {}, "connection"),
             channel_field("cdn_base_url", "CDN base URL", Kind::Text, false, {}, "connection"),
             channel_field("proxy", "Proxy", Kind::Text, false, {}, "connection"), allow}},
        {"discord", "Discord", "Connect a Discord bot to your server.", false,
            {channel_field("token", "Bot token", Kind::Secret, true, {}, "credentials"),
             channel_field("proxy", "Proxy", Kind::Text, false, {}, "connection"),
             channel_field("mention_only", "Mention only", Kind::Boolean, false, {}, "behavior"), allow}},
        {"telegram", "Telegram", "Create a bot with BotFather, then enter its token.", false,
            {channel_field("token", "Bot token", Kind::Secret, true, {}, "credentials"),
             channel_field("base_url", "API base URL", Kind::Text, false, {}, "connection"),
             channel_field("proxy", "Proxy", Kind::Text, false, {}, "connection"), allow}},
        {"slack", "Slack", "Connect a Slack app using bot and app-level tokens.", false,
            {channel_field("bot_token", "Bot token", Kind::Secret, true, {}, "credentials"),
             channel_field("app_token", "App token", Kind::Secret, true, {}, "credentials"), allow}},
        {"dingtalk", "DingTalk", "Connect your DingTalk robot with app credentials.", false,
            {channel_field("client_id", "Client ID", Kind::Text, true, {}, "credentials"),
             channel_field("client_secret", "Client secret", Kind::Secret, true, {}, "credentials"), allow}},
        {"qq", "QQ", "Connect your QQ bot using its app credentials.", false,
            {channel_field("app_id", "App ID", Kind::Text, true, {}, "credentials"),
             channel_field("app_secret", "App secret", Kind::Secret, true, {}, "credentials"), allow}},
        {"whatsapp", "WhatsApp", "Connect to an existing WhatsApp bridge.", false,
            {channel_field("bridge_url", "Bridge URL", Kind::Text, true, {}, "connection"), allow}},
        {"line", "LINE", "Connect a LINE Messaging API channel and webhook.", false,
            {channel_field("channel_secret", "Channel secret", Kind::Secret, true, {}, "credentials"),
             channel_field("channel_access_token", "Access token", Kind::Secret, true, {}, "credentials"),
             channel_field("webhook_host", "Webhook host", Kind::Text, false, {}, "webhook"),
             channel_field("webhook_port", "Webhook port", Kind::Integer, false, {}, "webhook"),
             channel_field("webhook_path", "Webhook path", Kind::Text, false, {}, "webhook"), allow}},
        {"onebot", "OneBot", "Connect a OneBot-compatible WebSocket service.", false,
            {channel_field("ws_url", "WebSocket URL", Kind::Text, true, {}, "connection"),
             channel_field("access_token", "Access token", Kind::Secret, false, {}, "credentials"), allow}},
    };
}

bool same_channel_values(const PicoClawChannel &left, const PicoClawChannel &right)
{
    if (left.id != right.id || left.enabled != right.enabled || left.fields.size() != right.fields.size()) return false;
    for (std::size_t i = 0; i < left.fields.size(); ++i)
        if (left.fields[i].key != right.fields[i].key || left.fields[i].value != right.fields[i].value) return false;
    return true;
}

bool valid_channel_field(const PicoClawChannelField &field)
{
    if (field.value.size() > 4096 || std::any_of(field.value.begin(), field.value.end(), [](unsigned char character) {
            return character < 0x20;
        })) return false;
    if (field.kind == PicoClawChannelFieldKind::Boolean)
        return field.value == "true" || field.value == "false";
    if (field.kind == PicoClawChannelFieldKind::Integer && !field.value.empty()) {
        if (field.value.size() > 5 || !std::all_of(field.value.begin(), field.value.end(), [](unsigned char character) {
                return std::isdigit(character);
            })) return false;
        const auto port = std::stoi(field.value);
        return port > 0 && port <= 65535;
    }
    return true;
}

std::string channel_json_string(std::string_view input)
{
    std::string result = "\"";
    for (const auto character : input) {
        if (character == '\\' || character == '"') result.push_back('\\');
        if (static_cast<unsigned char>(character) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00";
            result.push_back(hex[(static_cast<unsigned char>(character) >> 4) & 15]);
            result.push_back(hex[static_cast<unsigned char>(character) & 15]);
        } else result.push_back(character);
    }
    result.push_back('"');
    return result;
}

}  // namespace

std::string_view PicoClawModel::profile_provider(const PicoClawProfile &profile) noexcept
{
    for (const std::string_view provider : {"openai", "anthropic", "gemini", "deepseek", "ollama"})
        if (profile.provider == provider) return provider;
    const std::string_view id = profile.model_id;
    const auto slash = id.find('/');
    if (slash != std::string_view::npos) {
        const auto prefix = id.substr(0, slash);
        for (const std::string_view provider : {"openai", "anthropic", "gemini", "deepseek", "ollama"})
            if (prefix == provider) return provider;
    }
    std::string_view authority = profile.api_base;
    const auto scheme = authority.find("://");
    if (scheme != std::string_view::npos) authority.remove_prefix(scheme + 3U);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    const auto host = authority.substr(0, authority.find(':'));
    if (equal_ascii_case(host, "api.anthropic.com")) return "anthropic";
    if (equal_ascii_case(host, "generativelanguage.googleapis.com")) return "gemini";
    if (equal_ascii_case(host, "api.deepseek.com")) return "deepseek";
    if (equal_ascii_case(host, "ollama") || equal_ascii_case(authority, "localhost:11434") ||
        equal_ascii_case(authority, "127.0.0.1:11434") || equal_ascii_case(authority, "[::1]:11434")) return "ollama";
    return "openai";
}

PicoClawModel::PicoClawModel()
    : profiles_({
          {"DeepSeek Chat", "deepseek-chat", "deepseek/deepseek-chat", "https://api.deepseek.com/v1", ""},
          {"DeepSeek Reasoner", "deepseek-reasoner", "deepseek/deepseek-reasoner", "https://api.deepseek.com/v1", ""},
          {"GPT-4o mini", "gpt-4o-mini", "openai/gpt-4o-mini", "https://api.openai.com/v1", ""},
          {"Anthropic Claude", "claude-3-5-sonnet", "anthropic/claude-3-5-sonnet-latest",
           "https://api.anthropic.com/v1", ""},
          {"Google Gemini", "gemini-2-flash", "gemini/gemini-2.0-flash",
           "https://generativelanguage.googleapis.com/v1beta", ""},
          {"Ollama Llama 3.2", "llama3.2", "ollama/llama3.2", "http://127.0.0.1:11434/v1", ""},
          {"OpenAI compatible", "custom-agent", "openai/gpt-4o-mini", "https://api.openai.com/v1", ""},
      })
{
    for (auto &profile : profiles_) profile.provider = profile_provider(profile);
    load_settings(settings_);
    load_profiles();
    load_history();
    runtime_.web_url = "http://" + settings_.host + ":" + std::to_string(settings_.port);
    runtime_.url = "http://" + settings_.host + ":18790";
}

void PicoClawModel::load_profiles()
{
    const auto path = profiles_path();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (!exists && !ec) return;
    profiles_readable_ = false;
    profiles_error_ = "Could not load saved models. Existing API keys have been preserved.";
    if (ec || std::filesystem::file_size(path, ec) > 1024U * 1024U || ec) return;
    std::ifstream input(path);
    std::string magic;
    unsigned version = 0;
    std::size_t count = 0;
    if (!(input >> magic >> version >> count) || magic != "PICOCLAW_MODELS" || (version != 1 && version != 2) || count == 0 ||
        count > 128)
        return;
    std::vector<PicoClawProfile> loaded;
    loaded.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        PicoClawProfile profile;
        if (!(input >> std::quoted(profile.title) >> std::quoted(profile.model_name) >> std::quoted(profile.model_id) >>
              std::quoted(profile.api_base) >> std::quoted(profile.api_key)))
            return;
        if (version == 2 && (!(input >> std::quoted(profile.provider)) ||
                            (!profile.provider.empty() && !valid_provider(profile.provider)))) return;
        if (profile.title.empty() || profile.model_name.empty() || profile.model_id.empty() || profile.api_base.empty() ||
            !valid_model_name(profile.model_name) ||
            (profile.api_base.rfind("https://", 0) != 0 && profile.api_base.rfind("http://", 0) != 0))
            return;
        if (profile.provider.empty()) profile.provider = profile_provider(profile);
        loaded.push_back(std::move(profile));
    }
    input >> std::ws;
    if (!input.eof()) return;
    profiles_ = std::move(loaded);
    active_profile_index_ = 0;
    profiles_readable_ = true;
    profiles_error_.clear();
}

bool PicoClawModel::save_profiles()
{
    if (!profiles_readable_) return false;
    if (profiles_.empty() || profiles_.size() > 128) {
        profiles_error_ = "Model profiles are full. Remove one before saving.";
        return false;
    }
    std::ostringstream output;
    output << "PICOCLAW_MODELS 2 " << profiles_.size() << '\n';
    for (const auto &profile : profiles_)
        output << std::quoted(profile.title) << ' ' << std::quoted(profile.model_name) << ' '
               << std::quoted(profile.model_id) << ' ' << std::quoted(profile.api_base) << ' '
               << std::quoted(profile.api_key) << ' ' << std::quoted(std::string(profile_provider(profile))) << '\n';
    const auto data = output.str();
    if (data.size() > 1024U * 1024U || !write_profiles_atomic(data)) {
        profiles_error_ = "Could not save model profiles. Check storage and try again.";
        return false;
    }
    profiles_error_.clear();
    return true;
}

PicoClawScreen PicoClawModel::screen() const noexcept
{
    return screen_;
}
const std::vector<PicoClawProfile> &PicoClawModel::profiles() const noexcept
{
    return profiles_;
}
std::size_t PicoClawModel::active_profile_index() const noexcept
{
    return active_profile_index_;
}
const PicoClawProfile &PicoClawModel::active_profile() const noexcept
{
    return profiles_[active_profile_index_];
}
const std::string &PicoClawModel::profiles_error() const noexcept
{
    return profiles_error_;
}
const std::vector<PicoClawMessage> &PicoClawModel::messages() const noexcept
{
    return active_conversation().messages;
}
const std::vector<PicoClawConversation> &PicoClawModel::conversations() const noexcept
{
    return conversations_;
}
std::size_t PicoClawModel::active_conversation_index() const noexcept
{
    return active_conversation_index_;
}
const PicoClawConversation &PicoClawModel::active_conversation() const noexcept
{
    return conversations_[active_conversation_index_];
}
const std::string &PicoClawModel::history_error() const noexcept
{
    return history_error_;
}

void PicoClawModel::load_history()
{
    const auto id = PicoClawProtocol().session_id();
    conversations_ = {{id, "New conversation", "agent:main:ui:" + id, {}}};
    const auto path = history_path();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (!exists && !ec) return;
    history_readable_ = false;
    history_error_ = "Could not load chat history. Existing records have been preserved.";
    if (ec || std::filesystem::file_size(path, ec) > 32U * 1024U * 1024U || ec) return;
    std::ifstream input(path);
    std::string magic;
    unsigned version = 0;
    std::size_t active = 0, count = 0;
    if (!(input >> magic >> version >> active >> count) || magic != "PICOCLAW_CHAT_HISTORY" ||
        version != 1 || count == 0 || count > 10000 || active >= count) return;
    std::vector<PicoClawConversation> loaded;
    for (std::size_t index = 0; index < count; ++index) {
        PicoClawConversation conversation;
        std::size_t messages = 0;
        if (!(input >> std::quoted(conversation.id) >> std::quoted(conversation.title) >>
              std::quoted(conversation.cli_session) >> messages) || messages > 100000 ||
            !valid_model_name(conversation.id) || conversation.cli_session.empty()) return;
        for (std::size_t message_index = 0; message_index < messages; ++message_index) {
            int role = 0, pending = 0;
            PicoClawMessage message;
            if (!(input >> role >> pending >> std::quoted(message.text)) || (role != 0 && role != 1) ||
                (pending != 0 && pending != 1)) return;
            message.role = role == 0 ? PicoClawMessageRole::User : PicoClawMessageRole::Assistant;
            if (pending) message.text = "Response interrupted. Please send your message again.";
            conversation.messages.push_back(std::move(message));
        }
        loaded.push_back(std::move(conversation));
    }
    input >> std::ws;
    if (!input.eof()) return;
    conversations_ = std::move(loaded);
    active_conversation_index_ = active;
    history_readable_ = true;
    history_error_.clear();
}

bool PicoClawModel::save_history()
{
    if (!history_readable_) return false;
    if (conversations_.size() > 10000 || std::any_of(conversations_.begin(), conversations_.end(),
            [](const auto &conversation) { return conversation.messages.size() > 100000; })) {
        history_error_ = "Chat history is full. Existing records have been preserved.";
        return false;
    }
    std::ostringstream output;
    output << "PICOCLAW_CHAT_HISTORY 1\n" << active_conversation_index_ << ' ' << conversations_.size() << '\n';
    for (const auto &conversation : conversations_) {
        output << std::quoted(conversation.id) << ' ' << std::quoted(conversation.title) << ' '
               << std::quoted(conversation.cli_session) << ' ' << conversation.messages.size() << '\n';
        for (const auto &message : conversation.messages) {
            output << (message.role == PicoClawMessageRole::User ? 0 : 1) << ' ' << (message.pending ? 1 : 0)
                   << ' ' << std::quoted(message.text) << '\n';
        }
    }
    const auto data = output.str();
    if (data.size() > 32U * 1024U * 1024U || !write_history_atomic(data)) {
        history_error_ = "Could not save chat history. Keep this window open and check storage.";
        return false;
    }
    history_error_.clear();
    return true;
}
const PicoClawRuntimeStatus &PicoClawModel::runtime() const noexcept
{
    return runtime_;
}
const PicoClawSettings &PicoClawModel::settings() const noexcept
{
    return settings_;
}
const std::vector<PicoClawRelease> &PicoClawModel::releases() const noexcept
{
    return releases_;
}
std::size_t PicoClawModel::selected_release_index() const noexcept
{
    return selected_release_index_;
}
const PicoClawRelease *PicoClawModel::selected_release() const noexcept
{
    return selected_release_index_ < releases_.size() ? &releases_[selected_release_index_] : nullptr;
}
bool PicoClawModel::updates_loaded() const noexcept
{
    return updates_loaded_;
}
const std::string &PicoClawModel::update_message() const noexcept
{
    return update_message_;
}
const std::string &PicoClawModel::update_status() const noexcept
{
    return update_status_;
}
const std::vector<PicoClawChannel> &PicoClawModel::channels() const noexcept { return channels_; }
bool PicoClawModel::channels_loaded() const noexcept { return channels_loaded_; }
const std::string &PicoClawModel::channels_error() const noexcept { return channels_error_; }
const std::string &PicoClawModel::channels_status() const noexcept { return channels_status_; }
const PicoClawChannel *PicoClawModel::channel_draft() const noexcept
{
    return channel_draft_ ? &*channel_draft_ : nullptr;
}
bool PicoClawModel::channel_dirty() const noexcept
{
    if (!channel_draft_) return false;
    const auto saved = std::find_if(channels_.begin(), channels_.end(), [&](const PicoClawChannel &channel) {
        return channel.id == channel_draft_->id;
    });
    return saved == channels_.end() || !same_channel_values(*saved, *channel_draft_);
}
bool PicoClawModel::channels_restart_required() const noexcept { return channels_restart_required_; }
std::uint64_t PicoClawModel::channels_revision() const noexcept { return channels_revision_; }
const PicoClawWeixinBinding &PicoClawModel::weixin_binding() const noexcept { return weixin_binding_; }

std::uint64_t PicoClawModel::revision() const noexcept
{
    return revision_;
}
std::uint64_t PicoClawModel::message_revision() const noexcept
{
    return message_revision_;
}

bool PicoClawModel::show(PicoClawScreen screen) noexcept
{
    const auto screen_value = static_cast<unsigned>(screen);
    if (screen_value > static_cast<unsigned>(PicoClawScreen::Logs)) return false;
    if (screen_ == screen) return false;
    screen_ = screen;
    touch();
    return true;
}

bool PicoClawModel::back() noexcept
{
    if (screen_ == PicoClawScreen::Logs) return show(PicoClawScreen::Settings);
    return screen_ != PicoClawScreen::Chat && show(PicoClawScreen::Chat);
}

bool PicoClawModel::select_profile(std::size_t index) noexcept
{
    if (index >= profiles_.size() || index == active_profile_index_) return false;
    active_profile_index_ = index;
    touch();
    return true;
}

bool PicoClawModel::update_active_profile(PicoClawProfile profile, std::string &error)
{
    if (profile.model_name.empty()) {
        profile.model_name = profiles_[active_profile_index_].model_name;
        if (!unique_profile_alias(profiles_, profile.model_name, active_profile_index_, error))
            profile.model_name = generated_profile_alias(profile.model_name, profiles_);
    }
    if (profile.provider.empty()) profile.provider = profile_provider(profiles_[active_profile_index_]);
    if (!normalize_profile(profile, error) ||
        !unique_profile_alias(profiles_, profile.model_name, active_profile_index_, error)) return false;
    profiles_[active_profile_index_] = std::move(profile);
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::add_profile(PicoClawProfile profile, std::string &error)
{
    if (profile.model_name.empty()) profile.model_name = generated_profile_alias(profile.model_id, profiles_);
    if (!normalize_profile(profile, error) ||
        !unique_profile_alias(profiles_, profile.model_name, profiles_.size(), error)) return false;
    if (profiles_.size() >= 128U) {
        error = "Too many model profiles. Remove one before adding another.";
        return false;
    }
    profiles_.push_back(std::move(profile));
    if (!save_profiles()) {
        profiles_.pop_back();
        error = profiles_error_;
        return false;
    }
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::update_profile(std::size_t index, PicoClawProfile profile, std::string &error)
{
    if (index >= profiles_.size()) {
        error = "Model profile does not exist.";
        return false;
    }
    if (profile.model_name.empty()) {
        profile.model_name = profiles_[index].model_name;
        if (!unique_profile_alias(profiles_, profile.model_name, index, error))
            profile.model_name = generated_profile_alias(profile.model_name, profiles_);
    }
    if (profile.provider.empty()) profile.provider = profile_provider(profiles_[index]);
    if (!normalize_profile(profile, error) ||
        !unique_profile_alias(profiles_, profile.model_name, index, error)) return false;
    const auto previous = profiles_[index];
    profiles_[index] = std::move(profile);
    if (!save_profiles()) {
        profiles_[index] = previous;
        error = profiles_error_;
        return false;
    }
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::remove_profile(std::size_t index, std::string &error)
{
    if (index >= profiles_.size()) {
        error = "Model profile does not exist.";
        return false;
    }
    if (profiles_.size() <= 1U) {
        error = "At least one model profile is required.";
        return false;
    }
    const auto previous_profiles = profiles_;
    const auto previous_active = active_profile_index_;
    profiles_.erase(profiles_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_profile_index_ > index) --active_profile_index_;
    else if (active_profile_index_ >= profiles_.size()) active_profile_index_ = profiles_.size() - 1U;
    if (!save_profiles()) {
        profiles_ = previous_profiles;
        active_profile_index_ = previous_active;
        error = profiles_error_;
        return false;
    }
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::merge_profiles(std::string_view output, std::string &error)
{
    const auto invalid = [&error]() {
        error = "The model import was incomplete or invalid. Existing API keys have been preserved.";
        return false;
    };
    if (output.size() > 1024U * 1024U) return invalid();
    std::istringstream input{std::string(output)};
    std::string magic;
    unsigned version = 0;
    std::size_t count = 0;
    if (!(input >> magic >> version >> count) || magic != "PICOCLAW_MODELS" || version != 1 || count > 128U)
        return invalid();
    std::vector<PicoClawProfile> imported;
    std::set<std::string> aliases;
    for (std::size_t index = 0; index < count; ++index) {
        PicoClawProfile profile;
        for (auto *field : {&profile.title, &profile.model_name, &profile.model_id, &profile.api_base, &profile.api_key}) {
            input >> std::ws;
            if (input.peek() != '"' || !(input >> std::quoted(*field))) return invalid();
        }
        std::string validation_error;
        if (!normalize_profile(profile, validation_error) || !aliases.insert(profile.model_name).second)
            return invalid();
        imported.push_back(std::move(profile));
    }
    input >> std::ws;
    if (!input.eof()) return invalid();
    if (!profiles_readable_) {
        error = profiles_error_;
        return false;
    }
    auto merged = profiles_;
    for (auto &profile : imported) {
        auto found = std::find_if(merged.begin(), merged.end(), [&profile](const PicoClawProfile &existing) {
            return existing.model_name == profile.model_name;
        });
        if (found == merged.end()) {
            if (merged.size() >= 128U) {
                error = "Too many model profiles to import. Remove unused models and try again.";
                return false;
            }
            merged.push_back(std::move(profile));
        } else if (found->model_id == profile.model_id && found->api_base == profile.api_base && found->api_key.empty()) {
            found->api_key = std::move(profile.api_key);
        }
    }
    const auto previous = profiles_;
    profiles_ = std::move(merged);
    if (!save_profiles()) {
        profiles_ = previous;
        error = profiles_error_;
        return false;
    }
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::update_settings(PicoClawSettings settings, std::string &error)
{
    settings.host        = trim(settings.host);
    settings.binary_path = trim(settings.binary_path);
    settings.arguments   = trim(settings.arguments);
    settings.language    = trim(settings.language);
    if (!valid_host(settings.host)) {
        error = "Host must be a valid name or address.";
        return false;
    }
    if (settings.port < 1 || settings.port > 65535) {
        error = "Port must be between 1 and 65535.";
        return false;
    }
    if (settings.public_mode) settings.host = "0.0.0.0";
    if (settings.language.empty() || settings.language.size() > 12) {
        error = "Language code is invalid.";
        return false;
    }
    RuntimeSettingsLock update_lock;
    if (!update_lock.acquire(error) || !write_settings(settings, error)) return false;
    settings_        = std::move(settings);
    runtime_.web_url = "http://" + settings_.host + ":" + std::to_string(settings_.port);
    runtime_.url     = "http://" + settings_.host + ":18790";
    error.clear();
    touch();
    return true;
}

bool PicoClawModel::begin_prompt(std::string_view prompt, std::string &error)
{
    std::string value = trim(prompt);
    if (value.empty()) {
        error = "Type a message first.";
        return false;
    }
    if (value.size() > maximum_prompt_length) {
        error = "Message is too long.";
        return false;
    }
    auto &conversation = conversations_[active_conversation_index_];
    if (!conversation.messages.empty() && conversation.messages.back().pending) {
        error = "Wait for the current reply to finish.";
        return false;
    }
    const auto previous_title = conversation.title;
    if (conversation.messages.empty()) {
        conversation.title = value;
        std::replace(conversation.title.begin(), conversation.title.end(), '\n', ' ');
        std::replace(conversation.title.begin(), conversation.title.end(), '\r', ' ');
        std::size_t end = std::min<std::size_t>(conversation.title.size(), 72);
        while (end < conversation.title.size() &&
               (static_cast<unsigned char>(conversation.title[end]) & 0xc0U) == 0x80U)
            --end;
        conversation.title.resize(end);
    }
    conversation.messages.push_back({PicoClawMessageRole::User, std::move(value), false});
    conversation.messages.push_back({PicoClawMessageRole::Assistant, "Working on your request...", true});
    if (!save_history()) {
        conversation.messages.resize(conversation.messages.size() - 2);
        conversation.title = previous_title;
        error              = history_error_;
        return false;
    }
    error.clear();
    touch(true);
    return true;
}

bool PicoClawModel::new_conversation()
{
    if (messages().empty()) return false;
    if (messages().back().pending) {
        history_error_ = "Wait for the current reply to finish.";
        return false;
    }
    const auto previous = active_conversation_index_;
    const auto id = PicoClawProtocol().session_id();
    conversations_.push_back({id, "New conversation", "agent:main:ui:" + id, {}});
    active_conversation_index_ = conversations_.size() - 1;
    if (!save_history()) {
        conversations_.pop_back();
        active_conversation_index_ = previous;
        return false;
    }
    touch(true);
    return true;
}

bool PicoClawModel::select_conversation(std::size_t index)
{
    if (index >= conversations_.size() || index == active_conversation_index_) return false;
    if (!messages().empty() && messages().back().pending) {
        history_error_ = "Wait for the current reply to finish.";
        return false;
    }
    const auto previous = active_conversation_index_;
    active_conversation_index_ = index;
    if (!save_history()) {
        active_conversation_index_ = previous;
        return false;
    }
    touch(true);
    return true;
}

void PicoClawModel::update_prompt(std::string text)
{
    auto &messages = conversations_[active_conversation_index_].messages;
    for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
        if (message->role == PicoClawMessageRole::Assistant && message->pending) {
            message->text = trim(text);
            if (message->text.empty()) message->text = "Working on your request...";
            touch(true);
            return;
        }
    }
}

void PicoClawModel::complete_prompt(std::string reply, bool success)
{
    auto &messages = conversations_[active_conversation_index_].messages;
    for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
        if (message->role == PicoClawMessageRole::Assistant && message->pending) {
            message->pending = false;
            message->text    = trim(reply);
            if (message->text.empty())
                message->text = success ? "PicoClaw returned an empty response." : "Request failed.";
            (void)save_history();
            touch(true);
            return;
        }
    }
}

void PicoClawModel::apply_runtime_status(std::string_view output)
{
    const std::string installed = status_value(output, "installed");
    const std::string gateway   = status_value(output, "gateway");
    const std::string network   = status_value(output, "network");
    const std::string config    = status_value(output, "config");
    const std::string model_key = status_value(output, "model_key");
    const std::string version   = status_value(output, "version");
    const std::string url       = status_value(output, "url");
    const std::string web_url   = status_value(output, "web_url");
    const std::string update_supported = status_value(output, "update_supported");
    if (!installed.empty()) runtime_.installed = installed == "yes";
    if (!gateway.empty()) runtime_.gateway_running = gateway == "running";
    if (!network.empty()) runtime_.network_ready = network == "ready";
    if (!config.empty()) runtime_.config_ready = config == "ready";
    if (!model_key.empty()) runtime_.model_key_ready = model_key == "ready";
    if (!version.empty()) runtime_.version = version;
    if (!url.empty()) runtime_.url = url;
    if (!web_url.empty()) runtime_.web_url = web_url;
    if (!update_supported.empty()) runtime_.update_supported = update_supported == "yes";

    const std::string configured_name = status_value(output, "model_name");
    const std::string configured_id   = status_value(output, "model_id");
    const std::string configured_base = status_value(output, "api_base");
    // A local Ollama endpoint does not depend on an Internet route. Treat it as
    // network-ready so the UI can be used on an offline device.
    if (local_endpoint(configured_base) || configured_id.rfind("ollama/", 0) == 0) runtime_.network_ready = true;
    if (!configured_name.empty() && !configured_id.empty() && !configured_base.empty()) {
        auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const PicoClawProfile &profile) {
            return profile.model_name == configured_name && profile.model_id == configured_id &&
                   profile.api_base == configured_base;
        });
        if (found == profiles_.end()) {
            // Runtime imports must never replace a saved model or carry its
            // API key across to a different endpoint. A conflicting alias
            // belongs to the saved configuration until it is explicitly applied.
            const bool alias_exists = std::any_of(profiles_.begin(), profiles_.end(), [&](const PicoClawProfile &profile) {
                return profile.model_name == configured_name;
            });
            if (!alias_exists) {
                profiles_.push_back({configured_name, configured_name, configured_id, configured_base, {}});
                profiles_.back().provider = profile_provider(profiles_.back());
                active_profile_index_ = profiles_.size() - 1U;
            }
        } else {
            active_profile_index_ = static_cast<std::size_t>(std::distance(profiles_.begin(), found));
        }
    }
    touch();
}

bool PicoClawModel::apply_update_result(std::string_view output, std::string &error)
{
    error.clear();
    const auto fail = [&error]() {
        error = "The release list was incomplete or invalid. Check for updates again.";
        return false;
    };
    std::vector<PicoClawRelease> releases;
    std::set<std::string> tags;
    bool ready = false;
    bool complete = false;
    std::string latest_stable;
    std::string message;
    std::string status;
    std::istringstream input{std::string(output)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (complete) return fail();
        if (line == "updates=ready") ready = true;
        else if (line == "end_releases=yes") complete = true;
        else if (line.rfind("latest_stable=", 0) == 0) latest_stable = line.substr(14);
        else if (line.rfind("update_status=", 0) == 0) status = line.substr(14);
        else if (line.rfind("update_message=", 0) == 0) message = line.substr(15);
        else if (line.rfind("release=", 0) == 0) {
            std::vector<std::string> fields;
            std::size_t offset = 8;
            for (;;) {
                const auto end = line.find('\t', offset);
                fields.push_back(line.substr(offset, end == std::string::npos ? end : end - offset));
                if (end == std::string::npos) break;
                offset = end + 1;
            }
            if (fields.size() != 5 || fields[0].empty() || fields[0].size() > 256 ||
                !tags.insert(fields[0]).second ||
                (fields[2] != "stable" && fields[2] != "prerelease" && fields[2] != "nightly") ||
                (fields[4] != "yes" && fields[4] != "no")) return fail();
            for (const auto &field : fields)
                if (std::any_of(field.begin(), field.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
                    return fail();
            releases.push_back({fields[0], fields[1], fields[2], fields[3], fields[4] == "yes"});
        } else {
            return fail();
        }
    }
    if (!ready || !complete || message.empty() ||
        (status != "available" && status != "current" && status != "unknown" &&
         status != "not-installed" && status != "custom" && status != "unsupported")) return fail();

    const std::string selected_tag = selected_release() ? selected_release()->tag : std::string{};
    auto selected = std::find_if(releases.begin(), releases.end(), [&](const PicoClawRelease &release) {
        return release.tag == selected_tag;
    });
    if (selected == releases.end()) {
        selected = std::find_if(releases.begin(), releases.end(), [&](const PicoClawRelease &release) {
            return release.tag == latest_stable && release.installable;
        });
    }
    if (selected == releases.end())
        selected = std::find_if(releases.begin(), releases.end(), [](const PicoClawRelease &release) {
            return release.installable;
        });
    selected_release_index_ = selected == releases.end() ? 0 : static_cast<std::size_t>(selected - releases.begin());
    releases_ = std::move(releases);
    update_message_ = std::move(message);
    update_status_ = std::move(status);
    updates_loaded_ = true;
    touch();
    return true;
}

bool PicoClawModel::select_release(std::size_t index) noexcept
{
    if (index >= releases_.size()) return false;
    if (index == selected_release_index_) return true;
    selected_release_index_ = index;
    touch();
    return true;
}

void PicoClawModel::invalidate_update_status()
{
    update_status_ = "unknown";
    update_message_ = "Runtime changed. Check for updates to refresh availability.";
    touch();
}

bool PicoClawModel::apply_channels_result(std::string_view output, std::string &error)
{
    error = "Channel configuration is invalid or incomplete. Existing changes were preserved.";
    if (output.size() > 1024U * 1024U) return false;
    std::istringstream input{std::string(output)};
    std::string marker;
    unsigned version = 0;
    std::size_t count = 0;
    const auto catalog = channel_catalog();
    if (!(input >> marker >> version >> count) || marker != "PICOCLAW_CHANNELS" || version != 1 ||
        count == 0 || count > catalog.size()) return false;
    std::vector<PicoClawChannel> loaded;
    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        std::string id;
        int enabled = -1;
        std::size_t fields = 0;
        if (!(input >> std::quoted(id) >> enabled >> fields) || (enabled != 0 && enabled != 1) ||
            !ids.insert(id).second) return false;
        const auto schema = std::find_if(catalog.begin(), catalog.end(), [&](const PicoClawChannel &channel) {
            return channel.id == id;
        });
        if (schema == catalog.end() || fields != schema->fields.size()) return false;
        auto channel = *schema;
        channel.enabled = enabled == 1;
        std::set<std::string> keys;
        for (std::size_t j = 0; j < fields; ++j) {
            std::string key, value;
            if (!(input >> std::quoted(key) >> std::quoted(value)) || !keys.insert(key).second) return false;
            const auto field = std::find_if(channel.fields.begin(), channel.fields.end(), [&](const PicoClawChannelField &entry) {
                return entry.key == key;
            });
            if (field == channel.fields.end()) return false;
            field->value = std::move(value);
            if (!valid_channel_field(*field)) return false;
        }
        loaded.push_back(std::move(channel));
    }
    if (!(input >> marker) || marker != "END_CHANNELS") return false;
    input >> std::ws;
    if (!input.eof()) return false;
    // Reuse the catalog order so backend map order cannot reorder the UI.
    std::sort(loaded.begin(), loaded.end(), [&](const PicoClawChannel &a, const PicoClawChannel &b) {
        const auto position = [&](const std::string &id) {
            return std::find_if(catalog.begin(), catalog.end(), [&](const PicoClawChannel &channel) { return channel.id == id; });
        };
        return position(a.id) < position(b.id);
    });
    channels_ = std::move(loaded);
    channels_loaded_ = true;
    error.clear();
    set_channels_status(channels_restart_required_ ? "Channels loaded. Restart the gateway to apply saved changes."
                                                   : "Choose a channel to configure.");
    return true;
}

bool PicoClawModel::edit_channel(std::size_t index)
{
    if (!channels_loaded_ || index >= channels_.size()) return false;
    if (channel_draft_ && channel_draft_->id == channels_[index].id) return true;
    cancel_weixin_binding();
    channel_draft_ = channels_[index];
    weixin_binding_ = {};
    if (channel_draft_->id == "weixin") {
        const auto account = std::find_if(channel_draft_->fields.begin(), channel_draft_->fields.end(),
            [](const PicoClawChannelField &field) { return field.key == "account_id"; });
        if (account != channel_draft_->fields.end()) weixin_binding_.account_id = account->value;
    }
    set_channels_status("Edit the channel, then save your changes.");
    return true;
}

void PicoClawModel::close_channel()
{
    cancel_weixin_binding();
    channel_draft_.reset();
    set_channels_status(channels_restart_required_ ? "Restart the gateway to apply saved channel changes."
                                                   : "Choose a channel to configure.");
}

bool PicoClawModel::set_channel_enabled(bool enabled)
{
    if (!channel_draft_) return false;
    if (channel_draft_->enabled == enabled) return true;
    channel_draft_->enabled = enabled;
    set_channels_status("Unsaved channel changes.");
    return true;
}

bool PicoClawModel::set_channel_field(std::size_t index, std::string value)
{
    if (!channel_draft_ || index >= channel_draft_->fields.size()) return false;
    if (channel_draft_->fields[index].value == value) return true;
    channel_draft_->fields[index].value = std::move(value);
    set_channels_status("Unsaved channel changes.");
    return true;
}

bool PicoClawModel::channel_save_payload(std::string &payload, std::string &error) const
{
    payload.clear();
    if (!channel_draft_ || !channels_loaded_) {
        error = "Load and select a channel before saving.";
        return false;
    }
    const auto &channel = *channel_draft_;
    for (const auto &field : channel.fields) {
        if (!valid_channel_field(field) || (field.kind == PicoClawChannelFieldKind::Integer && field.value.empty())) {
            error = field.kind == PicoClawChannelFieldKind::Integer ? "Enter a webhook port between 1 and 65535."
                                                                  : "Check the channel fields. Values must fit on one line.";
            return false;
        }
        if (channel.enabled && field.required && trim(field.value).empty()) {
            error = channel.id == "weixin" && field.key == "token"
                ? "Bind your Weixin account before enabling this channel."
                : field.label + " is required to enable this channel.";
            return false;
        }
        if (!field.value.empty() &&
            (field.key == "base_url" || field.key == "cdn_base_url" || field.key == "bridge_url" ||
             field.key == "ws_url" || field.key == "proxy")) {
            const bool websocket = field.key == "bridge_url" || field.key == "ws_url";
            const bool proxy = field.key == "proxy";
            const auto first = websocket ? std::string_view("ws://") : std::string_view("http://");
            const auto second = websocket ? std::string_view("wss://") : std::string_view("https://");
            const auto valid_prefix = [&](std::string_view prefix) {
                return field.value.size() > prefix.size() && field.value.compare(0, prefix.size(), prefix) == 0;
            };
            if ((!valid_prefix(first) && !valid_prefix(second) && !(proxy && (valid_prefix("socks5://") || valid_prefix("socks5h://")))) ||
                field.value.find_first_of(" \t") != std::string::npos) {
                error = field.label + (websocket ? " must start with ws:// or wss://." : proxy
                    ? " must use http://, https://, socks5:// or socks5h://." : " must start with http:// or https://.");
                return false;
            }
        }
        if (field.key == "webhook_path" && !field.value.empty() && field.value.front() != '/') {
            error = "Webhook path must start with /.";
            return false;
        }
    }
    payload = "{\"enabled\":" + std::string(channel.enabled ? "true" : "false") + ",\"fields\":{";
    for (std::size_t i = 0; i < channel.fields.size(); ++i) {
        if (i) payload += ',';
        payload += channel_json_string(channel.fields[i].key) + ':' + channel_json_string(channel.fields[i].value);
    }
    payload += "}}";
    error.clear();
    return true;
}

void PicoClawModel::complete_channel_save(const PicoClawChannel &saved)
{
    const auto current = std::find_if(channels_.begin(), channels_.end(), [&](const PicoClawChannel &channel) {
        return channel.id == saved.id;
    });
    if (current != channels_.end()) *current = saved;
    channels_restart_required_ = true;
    if (saved.id == "weixin" && weixin_binding_.state == PicoClawWeixinBindingState::Confirmed)
        weixin_binding_.message = "Weixin account saved. Restart the gateway to apply changes.";
    set_channels_status(channel_dirty() ? "Channel saved. Newer edits are unsaved. Restart the gateway to apply saved changes."
                                        : "Channel saved. Restart the gateway to apply changes.");
}

void PicoClawModel::set_channels_status(std::string message, bool error)
{
    channels_status_ = std::move(message);
    channels_error_ = error ? channels_status_ : std::string{};
    touch_channels();
}

bool PicoClawModel::begin_weixin_binding()
{
    if (!channel_draft_ || channel_draft_->id != "weixin") return false;
    weixin_binding_.state = PicoClawWeixinBindingState::Starting;
    weixin_binding_.qr_url.clear();
    weixin_binding_.account_id.clear();
    weixin_binding_session_.clear();
    weixin_binding_base_url_ = "https://ilinkai.weixin.qq.com/";
    weixin_binding_proxy_.clear();
    for (const auto &field : channel_draft_->fields) {
        if (field.key == "base_url" && !field.value.empty()) weixin_binding_base_url_ = field.value;
        if (field.key == "proxy") weixin_binding_proxy_ = field.value;
    }
    weixin_binding_.message = "Creating a Weixin QR code...";
    touch_channels();
    return true;
}

void PicoClawModel::cancel_weixin_binding()
{
    if (!weixin_binding_.active()) return;
    weixin_binding_.state = PicoClawWeixinBindingState::Cancelled;
    weixin_binding_.qr_url.clear();
    weixin_binding_session_.clear();
    weixin_binding_.message = "Binding cancelled. Your account settings were preserved.";
    touch_channels();
}

void PicoClawModel::fail_weixin_binding()
{
    weixin_binding_.state = PicoClawWeixinBindingState::Failed;
    weixin_binding_.qr_url.clear();
    weixin_binding_session_.clear();
    weixin_binding_.message = "Could not bind Weixin. Refresh the QR code to try again.";
    touch_channels();
}

void PicoClawModel::expire_weixin_binding()
{
    if (!weixin_binding_.active()) return;
    weixin_binding_.state = PicoClawWeixinBindingState::Expired;
    weixin_binding_.qr_url.clear();
    weixin_binding_session_.clear();
    weixin_binding_.message = "This QR code expired. Refresh it to try again.";
    touch_channels();
}

std::string PicoClawModel::weixin_binding_start_payload() const
{
    return "{\"base_url\":" + channel_json_string(weixin_binding_base_url_) +
           ",\"proxy\":" + channel_json_string(weixin_binding_proxy_) + "}";
}

std::string PicoClawModel::weixin_binding_poll_payload() const
{
    return "{\"base_url\":" + channel_json_string(weixin_binding_base_url_) +
           ",\"proxy\":" + channel_json_string(weixin_binding_proxy_) +
           ",\"qrcode\":" + channel_json_string(weixin_binding_session_) + "}";
}

bool PicoClawModel::apply_weixin_binding_result(std::string_view output, std::string &error)
{
    error = "Invalid Weixin binding response. Refresh the QR code to try again.";
    if (!channel_draft_ || channel_draft_->id != "weixin" || !weixin_binding_.active() || output.size() > 64U * 1024U)
        return false;
    std::istringstream input{std::string(output)};
    std::string marker;
    int version = 0;
    if (!(input >> marker >> version) || marker != "PICOCLAW_WEIXIN_BINDING" || version != 1) return false;
    std::vector<std::pair<std::string, std::string>> values;
    const std::set<std::string> allowed{"status", "qrcode", "qr_url", "base_url", "token", "account_id",
                                        "user_id", "redirect_host", "error"};
    bool complete = false;
    while (input >> marker) {
        if (marker == "END_PICOCLAW_WEIXIN_BINDING") {
            complete = true;
            break;
        }
        if (!allowed.count(marker) || std::any_of(values.begin(), values.end(), [&](const auto &entry) {
                return entry.first == marker;
            })) return false;
        std::string value;
        input >> std::ws;
        if (input.peek() != '"' || !(input >> std::quoted(value)) || value.size() > 4096 ||
            std::any_of(value.begin(), value.end(), [](unsigned char character) { return character < 0x20; })) return false;
        values.emplace_back(marker, std::move(value));
    }
    input >> std::ws;
    if (!complete || !input.eof()) return false;
    const auto value = [&](std::string_view key) -> std::string {
        for (const auto &entry : values) if (entry.first == key) return entry.second;
        return {};
    };
    const auto http_url = [](const std::string &url) {
        const bool prefix = (url.rfind("https://", 0) == 0 && url.size() > 8) ||
                            (url.rfind("http://", 0) == 0 && url.size() > 7);
        return prefix && url.find_first_of(" \t") == std::string::npos;
    };
    const auto status = value("status");
    const auto session = value("qrcode");
    const auto qr_url = value("qr_url");
    const auto base_url = value("base_url");
    if ((!qr_url.empty() && !http_url(qr_url)) || (!base_url.empty() && !http_url(base_url))) return false;
    if (weixin_binding_.state == PicoClawWeixinBindingState::Starting) {
        if (status != "wait" || session.empty() || qr_url.empty()) return false;
    } else if (!session.empty() && session != weixin_binding_session_) return false;
    if (status == "confirmed") {
        const auto token = value("token");
        const auto account = value("account_id");
        if (token.empty() || account.empty()) return false;
        // Apply the confirmed credentials atomically. Unrelated channel fields
        // and edits remain untouched until the user explicitly saves.
        auto draft = *channel_draft_;
        bool has_token = false, has_account = false;
        for (auto &field : draft.fields) {
            if (field.key == "token") { field.value = token; has_token = true; }
            if (field.key == "account_id") { field.value = account; has_account = true; }
            if (field.key == "base_url") field.value = base_url.empty() ? weixin_binding_base_url_ : base_url;
        }
        if (!has_token || !has_account) return false;
        channel_draft_ = std::move(draft);
        weixin_binding_.state = PicoClawWeixinBindingState::Confirmed;
        weixin_binding_.account_id = account;
        weixin_binding_.qr_url.clear();
        weixin_binding_session_.clear();
        weixin_binding_.message = "Weixin account connected. Save to apply this account.";
        set_channels_status(weixin_binding_.message);
    } else if (status == "expired") {
        expire_weixin_binding();
    } else if (status == "wait" || status == "scaned" || status == "scaned_but_redirect") {
        if (status == "scaned_but_redirect" && base_url.empty()) return false;
        if (!session.empty()) weixin_binding_session_ = session;
        if (!qr_url.empty()) weixin_binding_.qr_url = qr_url;
        if (!base_url.empty()) weixin_binding_base_url_ = base_url;
        const bool scanned = status != "wait" || weixin_binding_.state == PicoClawWeixinBindingState::Scanned;
        weixin_binding_.state = scanned ? PicoClawWeixinBindingState::Scanned : PicoClawWeixinBindingState::Waiting;
        weixin_binding_.message = scanned ? "QR code scanned. Confirm the connection in Weixin."
                                         : "Scan this QR code with Weixin.";
        touch_channels();
    } else return false;
    error.clear();
    return true;
}

void PicoClawModel::touch_channels() noexcept
{
    ++channels_revision_;
    touch();
}

void PicoClawModel::set_output(std::string output)
{
    constexpr std::size_t maximum_activity_length = 4096;
    if (output.size() > maximum_activity_length) output.erase(0, output.size() - maximum_activity_length);
    runtime_.output = trim(output);
    touch();
}

void PicoClawModel::touch(bool messages) noexcept
{
    ++revision_;
    if (messages) ++message_revision_;
}
