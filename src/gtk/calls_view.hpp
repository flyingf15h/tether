#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace tether::ui {

    // Calls on the iPhone, over Bluetooth Hands-Free.
    GtkWidget* calls_view_new();

    bool calls_view_handle_event(const nlohmann::json& event);

    // The call list is only re-read while the view is on screen. A ringing call
    // still arrives, because the daemon pushes those unasked.
    void calls_view_set_visible(bool visible);

    // The "on a call on your iPhone" strip with its Move to laptop button,
    // packed above every tab by the window.
    GtkWidget* calls_banner_new();

    // Reflects the daemon's "always use laptop for calls" setting.
    void calls_view_set_on_laptop(bool on);

} // namespace tether::ui
