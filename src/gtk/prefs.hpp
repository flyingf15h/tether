#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace tether::ui {

    // ~/.config/tether/gtk.json, read once on first use. Unreadable or corrupt means defaults.
    nlohmann::json& prefs();

    void prefs_save();

    // Pinned conversations and contacts, by thread key ("tel:+1555…"), in pin order.
    bool is_pinned(const std::string& key);
    // Position among pins, or -1.
    int pin_index(const std::string& key);
    void set_pinned(const std::string& key, bool pinned);
    // Called whenever a pin changes, so lists can re-sort.
    void on_pins_changed(void (*callback)());

} // namespace tether::ui
