#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace tether::ui {

    // Mirrored iPhone notifications, delivered over ANCS.
    GtkWidget* notifications_view_new();

    bool notifications_view_handle_event(const nlohmann::json& event);

    // Notifications are only re-read while the view is on screen.
    void notifications_view_set_visible(bool visible);

    // Reflects the daemon's desktop popup setting in the tab's mute switch.
    void notifications_view_set_muted(bool muted);
    void notifications_view_set_message_popups(bool on);

} // namespace tether::ui
