#include "prefs.hpp"

#include <filesystem>
#include <fstream>
#include <vector>
#include <glib.h>
#include <tether/log.hpp>
#include <tether/paths.hpp>

namespace tether::ui {

    namespace {

        std::string prefs_path() {
            const std::filesystem::path dir = tether::paths::config_dir();
            if (dir.empty())
                return {};
            return (dir / "gtk.json").string();
        }

    } // namespace

    nlohmann::json& prefs() {
        static nlohmann::json loaded = [] {
            const std::string path = prefs_path();
            if (!path.empty()) {
                try {
                    std::ifstream in(path);
                    if (in) {
                        nlohmann::json parsed = nlohmann::json::parse(in);
                        if (parsed.is_object())
                            return parsed;
                    }
                } catch (const std::exception& e) {
                    debug::log(WARN, "prefs: ignoring {} ({})", path, e.what());
                }
            }
            return nlohmann::json::object();
        }();
        return loaded;
    }

    void prefs_save() {
        const std::string path = prefs_path();
        if (path.empty())
            return;
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            std::ofstream out(path);
            out << prefs().dump(2) << "\n";
        } catch (const std::exception& e) {
            debug::log(ERR, "prefs: could not write {} ({})", path, e.what());
        }
    }

    namespace {
        std::vector<void (*)()>& pin_listeners() {
            static std::vector<void (*)()> listeners;
            return listeners;
        }

        nlohmann::json& pins() {
            auto& p = prefs();
            if (!p.contains("pinned") || !p["pinned"].is_array())
                p["pinned"] = nlohmann::json::array();
            return p["pinned"];
        }
    } // namespace

    int pin_index(const std::string& key) {
        const auto& list = pins();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].is_string() && list[i].get<std::string>() == key)
                return static_cast<int>(i);
        return -1;
    }

    bool is_pinned(const std::string& key) { return !key.empty() && pin_index(key) >= 0; }

    void set_pinned(const std::string& key, bool pinned) {
        if (key.empty() || is_pinned(key) == pinned)
            return;
        auto& list = pins();
        if (pinned) {
            list.push_back(key);
        } else {
            nlohmann::json kept = nlohmann::json::array();
            for (const auto& k : list)
                if (!(k.is_string() && k.get<std::string>() == key))
                    kept.push_back(k);
            list = kept;
        }
        prefs_save();
        for (auto callback : pin_listeners())
            callback();
    }

    void on_pins_changed(void (*callback)()) { pin_listeners().push_back(callback); }

} // namespace tether::ui
