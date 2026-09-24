#pragma once

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

// All UI/model tests own their state; never read or overwrite developer chats.
class TestEnvironment {
public:
    TestEnvironment()
    {
        char pattern[] = "/tmp/picoclaw-test.XXXXXX";
        const int fd = ::mkstemp(pattern);
        assert(fd >= 0);
        ::close(fd);
        root = std::string(pattern) + "-dir";
        assert(std::filesystem::create_directory(root));
        ::unlink(pattern);
        set("PICOCLAW_HISTORY_DIR", root + "/history");
        set("PICOCLAW_UI_SETTINGS", root + "/ui-settings.conf");
        set("PICOCLAW_MODELS_PATH", root + "/models.conf");
        set("PICOCLAW_TRANSPORT", "cli");
    }
    ~TestEnvironment()
    {
        for (auto iterator = saved_.rbegin(); iterator != saved_.rend(); ++iterator) {
            const auto &entry = *iterator;
            if (entry.present) ::setenv(entry.name.c_str(), entry.value.c_str(), 1);
            else ::unsetenv(entry.name.c_str());
        }
        std::filesystem::remove_all(root);
    }
    void set(const char *name, const std::string &value)
    {
        const char *previous = std::getenv(name);
        saved_.push_back({name, previous ? previous : "", previous != nullptr});
        ::setenv(name, value.c_str(), 1);
    }
    void unset(const char *name)
    {
        set(name, "");
        ::unsetenv(name);
    }
    std::string root;

private:
    struct Entry { std::string name; std::string value; bool present; };
    std::vector<Entry> saved_;
};
