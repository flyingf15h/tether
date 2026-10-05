#include "calls_view.hpp"
#include "contact_completion.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <string>
#include <algorithm>
#include <memory>
#include <set>
#include <cstdlib>
#include <cctype>
#include <cstring>

namespace tether::ui {

    namespace {

        struct CallsState {
            GtkWidget* status_label = nullptr;
            GtkWidget* list = nullptr;
            GtkWidget* stack = nullptr;
            GtkWidget* entry = nullptr;
            GtkWidget* dial_button = nullptr;
            GtkWidget* dial_phone_button = nullptr;
            GtkWidget* laptop_switch = nullptr;
            // "On a call on your iPhone" strip shown over every tab.
            GtkWidget* banner = nullptr;
            GtkWidget* banner_label = nullptr;
            std::string banner_call;          // path of the call the strip is about
            std::set<std::string> notified;   // calls already announced on the desktop
            nlohmann::json live = nlohmann::json::array();
            GtkWidget* network_label = nullptr;
            bool visible = false;
            bool available = false;
            bool audio_routable = false;
            // The voice link is up on this computer.
            bool audio_here = false;
        };

        CallsState g_calls;

        void request_calls() {
            nlohmann::json j;
            j["command"] = "bt_list_calls";
            daemon_send(j);
        }

        void send_action(const std::string& action, const std::string& path) {
            nlohmann::json j;
            j["command"] = "bt_call_action";
            j["action"] = action;
            if (!path.empty())
                j["path"] = path;
            daemon_send(j);
        }

        // The row's call path, owned by the button.
        std::string button_path(GtkButton* button) {
            const char* path = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "call-path"));
            return path ? path : "";
        }

        void on_answer_clicked(GtkButton* button, gpointer) { send_action("answer", button_path(button)); }

        void on_answer_here_clicked(GtkButton* button, gpointer) { send_action("answer_here", button_path(button)); }

        void on_audio_here_clicked(GtkButton*, gpointer) { send_action("audio_here", ""); }

        void on_hangup_clicked(GtkButton* button, gpointer) { send_action("hangup", button_path(button)); }


        void on_hold_clicked(GtkButton*, gpointer) { send_action("hold", ""); }

        // The caller's voice plays through the echo canceller's sink; its volume is the call volume.
        int call_volume() {
            gchar* out = nullptr;
            int percent = 100;
            if (g_spawn_command_line_sync("pactl get-sink-volume tether_call_speaker", &out, nullptr, nullptr, nullptr) && out) {
                if (const char* pct = std::strchr(out, '%')) {
                    const char* start = pct;
                    while (start > out && std::isdigit(static_cast<unsigned char>(start[-1])))
                        --start;
                    percent = std::atoi(start);
                }
            }
            g_free(out);
            return percent;
        }

        void on_volume_changed(GtkRange* range, gpointer) {
            const int percent = static_cast<int>(gtk_range_get_value(range));
            const std::string cmd = "pactl set-sink-volume tether_call_speaker " + std::to_string(percent) + "%";
            g_spawn_command_line_async(cmd.c_str(), nullptr);
        }

        void on_audio_controls_clicked(GtkButton* button, gpointer) {
            GtkWidget* popover = gtk_popover_new(GTK_WIDGET(button));
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);

            GtkWidget* volume_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_box_pack_start(GTK_BOX(volume_row),
                               gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_BUTTON),
                               FALSE, FALSE, 0);
            GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 5);
            gtk_widget_set_size_request(scale, 220, -1);
            gtk_scale_add_mark(GTK_SCALE(scale), 100, GTK_POS_BOTTOM, nullptr);
            gtk_range_set_value(GTK_RANGE(scale), call_volume());
            gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
            g_signal_connect(scale, "value-changed", G_CALLBACK(on_volume_changed), nullptr);
            gtk_box_pack_start(GTK_BOX(volume_row), scale, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(box), volume_row, FALSE, FALSE, 0);

            GtkWidget* mute_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_box_pack_start(GTK_BOX(mute_row), gtk_label_new(_("Mute microphone")), FALSE, FALSE, 0);
            GtkWidget* mute = gtk_switch_new();
            gtk_switch_set_active(GTK_SWITCH(mute), g_object_get_data(G_OBJECT(button), "muted") != nullptr);
            g_signal_connect(mute,
                             "notify::active",
                             G_CALLBACK(+[](GtkSwitch* sw, GParamSpec*, gpointer) {
                                 send_action(gtk_switch_get_active(sw) ? "mute" : "unmute", "");
                             }),
                             nullptr);
            gtk_box_pack_end(GTK_BOX(mute_row), mute, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), mute_row, FALSE, FALSE, 0);

            gtk_container_add(GTK_CONTAINER(popover), box);
            gtk_widget_show_all(box);
            g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) { gtk_widget_destroy(GTK_WIDGET(p)); }), nullptr);
            gtk_popover_popup(GTK_POPOVER(popover));
        }

        void on_tone_clicked(GtkButton* button, gpointer) {
            nlohmann::json j;
            j["command"] = "bt_call_tones";
            j["tones"] = gtk_button_get_label(button);
            daemon_send(j);
        }

        // Touch tones for menus ("press 1 for..."), sent through the phone.
        void on_keypad_clicked(GtkButton* button, gpointer) {
            GtkWidget* popover = gtk_popover_new(GTK_WIDGET(button));
            GtkWidget* grid = gtk_grid_new();
            gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
            gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
            gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
            const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
            for (int i = 0; i < 12; ++i) {
                GtkWidget* key = gtk_button_new_with_label(keys[i]);
                gtk_widget_set_size_request(key, 48, 40);
                g_signal_connect(key, "clicked", G_CALLBACK(on_tone_clicked), nullptr);
                gtk_grid_attach(GTK_GRID(grid), key, i % 3, i / 3, 1, 1);
            }
            gtk_container_add(GTK_CONTAINER(popover), grid);
            gtk_widget_show_all(grid);
            g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) { gtk_widget_destroy(GTK_WIDGET(p)); }), nullptr);
            gtk_popover_popup(GTK_POPOVER(popover));
        }

        GtkWidget* icon_button(const char* icon, const char* tip) {
            GtkWidget* button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
            gtk_widget_set_tooltip_text(button, tip);
            gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
            return button;
        }

        void dial(bool audio_here) {
            const gchar* text = gtk_entry_get_text(GTK_ENTRY(g_calls.entry));
            const std::string number = text ? text : "";
            if (number.empty())
                return;
            nlohmann::json j;
            j["command"] = "bt_call_dial";
            j["number"] = number;
            if (audio_here)
                j["audio"] = "here";
            daemon_send(j);
            gtk_entry_set_text(GTK_ENTRY(g_calls.entry), "");
        }

        // Enter in the number field calls from the laptop when it can carry the
        // audio, and on the iPhone otherwise.
        void on_dial_clicked(GtkButton*, gpointer) { dial(g_calls.audio_routable); }

        void on_dial_phone_clicked(GtkButton*, gpointer) { dial(false); }

        void attach_path(GtkWidget* button, const std::string& path) {
            g_object_set_data_full(G_OBJECT(button), "call-path", g_strdup(path.c_str()), g_free);
        }

        // The phone's own words for a state, kept short enough for a row.
        const char* state_text(const std::string& state) {
            if (state == "incoming")
                return _("Incoming");
            if (state == "waiting")
                return _("Call waiting");
            if (state == "dialing")
                return _("Dialing");
            if (state == "alerting")
                return _("Ringing");
            if (state == "active")
                return _("On call");
            if (state == "held")
                return _("On hold");
            if (state == "disconnected")
                return _("Ended");
            return "";
        }

        // What HFP reports about the phone's cellular link. "spoken" replaces the
        // bar glyphs and bare percentage with words for screen readers.
        std::string network_text(const nlohmann::json& calls, bool spoken = false) {
            if (!calls.is_object())
                return {};
            if (!calls.value("indicators", true))
                return {};
            const std::string gap = spoken ? ", " : "  ";
            std::string out = calls.value("operator", "");
            if (!calls.value("service", false))
                out = out.empty() ? _("No service") : out + (spoken ? gap : "  -  ") + _("No service");
            const int signal = calls.value("signal", 0);
            if (calls.value("service", false)) {
                std::string bars;
                if (spoken) {
                    // TRANSLATORS: Cellular signal strength read aloud, {} is 0 to 5.
                    bars = tether::tr_format(_("signal {} of 5"), signal);
                } else {
                    for (int i = 0; i < 5; ++i)
                        bars += i < signal ? "\u2586" : "\u2581";
                }
                out += out.empty() ? bars : gap + bars;
            }
            if (calls.value("roaming", false))
                out += gap + _("roaming");
            if (const int battery = calls.value("battery", 0); battery > 0) {
                const std::string level = std::to_string(battery * 20) + "%";
                // TRANSLATORS: The iPhone's battery level read aloud, {} is like "80%".
                out += gap + (spoken ? tether::tr_format(_("battery {}"), level) : level);
            }
            return out;
        }

        GtkWidget* build_row(const nlohmann::json& call) {
            const std::string number = call.value("number", "");
            const std::string name = call.value("name", "");
            const std::string state = call.value("state", "");
            const std::string path = call.value("path", "");
            const bool ringing = call.value("ringing", false);

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            gtk_box_pack_start(
                GTK_BOX(box),
                gtk_image_new_from_icon_name(ringing ? "call-incoming" : "call-start", GTK_ICON_SIZE_LARGE_TOOLBAR),
                FALSE,
                FALSE,
                0);

            GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            const std::string primary = !name.empty() ? name : (!number.empty() ? number : _("Unknown caller"));
            GtkWidget* primary_label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(primary_label), ("<b>" + escape_markup(primary) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(primary_label), 0.0);
            gtk_box_pack_start(GTK_BOX(text), primary_label, FALSE, FALSE, 0);

            std::string secondary = state_text(state);
            if (!name.empty() && !number.empty())
                secondary += secondary.empty() ? number : "  -  " + number;

            GtkWidget* secondary_label = gtk_label_new(secondary.c_str());
            gtk_label_set_xalign(GTK_LABEL(secondary_label), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(secondary_label), "muted");
            gtk_box_pack_start(GTK_BOX(text), secondary_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

            if (ringing && !call.value("outgoing", false)) {
                if (g_calls.audio_routable) {
                    GtkWidget* here = gtk_button_new_with_label(_("Answer on laptop"));
                    gtk_style_context_add_class(gtk_widget_get_style_context(here), "suggested-action");
                    attach_path(here, path);
                    g_signal_connect(here, "clicked", G_CALLBACK(on_answer_here_clicked), nullptr);
                    gtk_widget_set_valign(here, GTK_ALIGN_CENTER);
                    gtk_box_pack_start(GTK_BOX(box), here, FALSE, FALSE, 0);
                }
                GtkWidget* answer =
                    gtk_button_new_with_label(g_calls.audio_routable ? _("Answer on iPhone") : _("Answer"));
                if (!g_calls.audio_routable)
                    gtk_style_context_add_class(gtk_widget_get_style_context(answer), "suggested-action");
                attach_path(answer, path);
                g_signal_connect(answer, "clicked", G_CALLBACK(on_answer_clicked), nullptr);
                gtk_widget_set_valign(answer, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), answer, FALSE, FALSE, 0);
            }

            if (call.value("connected", false) && g_calls.audio_here) {
                // PipeWire can only gate the next voice link, not release this one,
                // so moving it back is the iPhone's call.
                GtkWidget* where = gtk_label_new(_("Audio on this computer"));
                gtk_widget_set_tooltip_text(where, _("To move it back, pick iPhone under Audio on the phone's call screen."));
                gtk_style_context_add_class(gtk_widget_get_style_context(where), "muted");
                gtk_widget_set_valign(where, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), where, FALSE, FALSE, 0);
            } else if (call.value("connected", false) && g_calls.audio_routable) {
                GtkWidget* audio = gtk_button_new_with_label(_("Move to laptop"));
                gtk_widget_set_tooltip_text(audio, _("Play this call on the laptop's speakers and mic."));
                g_signal_connect(audio, "clicked", G_CALLBACK(on_audio_here_clicked), nullptr);
                gtk_widget_set_valign(audio, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), audio, FALSE, FALSE, 0);
            }

            // In-call controls, so nothing about a call needs the phone in hand.
            if (call.value("connected", false)) {
                GtkWidget* audio_controls = icon_button(call.value("muted", false) ? "microphone-sensitivity-muted-symbolic"
                                                                                   : "audio-volume-high-symbolic",
                                                        _("Call volume and mute"));
                gtk_widget_set_sensitive(audio_controls, g_calls.audio_here);
                g_object_set_data(G_OBJECT(audio_controls), "muted", GINT_TO_POINTER(call.value("muted", false) ? 1 : 0));
                g_signal_connect(audio_controls, "clicked", G_CALLBACK(on_audio_controls_clicked), nullptr);
                gtk_box_pack_start(GTK_BOX(box), audio_controls, FALSE, FALSE, 0);

                GtkWidget* hold = icon_button(state == "held" ? "media-playback-start-symbolic"
                                                              : "media-playback-pause-symbolic",
                                              state == "held" ? _("Resume") : _("Hold"));
                g_signal_connect(hold, "clicked", G_CALLBACK(on_hold_clicked), nullptr);
                gtk_box_pack_start(GTK_BOX(box), hold, FALSE, FALSE, 0);

                GtkWidget* keypad = icon_button("input-dialpad-symbolic", _("Keypad"));
                g_signal_connect(keypad, "clicked", G_CALLBACK(on_keypad_clicked), nullptr);
                gtk_box_pack_start(GTK_BOX(box), keypad, FALSE, FALSE, 0);
            }

            if (state != "disconnected") {
                GtkWidget* hangup = gtk_button_new_with_label(ringing ? _("Decline") : _("Hang up"));
                gtk_style_context_add_class(gtk_widget_get_style_context(hangup), "destructive-action");
                attach_path(hangup, path);
                g_signal_connect(hangup, "clicked", G_CALLBACK(on_hangup_clicked), nullptr);
                gtk_widget_set_valign(hangup, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), hangup, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        void update_banner();

        void show_calls(const nlohmann::json& event) {
            g_calls.live = event.contains("calls") && event["calls"].is_array() ? event["calls"] : nlohmann::json::array();
            for (auto it = g_calls.notified.begin(); it != g_calls.notified.end();) {
                const bool alive = std::any_of(g_calls.live.begin(), g_calls.live.end(), [&](const auto& c) {
                    return c.value("path", "") == *it;
                });
                it = alive ? std::next(it) : g_calls.notified.erase(it);
            }
            update_banner();
            clear_list_box(g_calls.list);
            const bool empty = !event.contains("calls") || event["calls"].empty();
            if (!empty) {
                for (const auto& call : event["calls"])
                    gtk_list_box_insert(GTK_LIST_BOX(g_calls.list), build_row(call), -1);
            }
            gtk_widget_show_all(g_calls.list);
            gtk_stack_set_visible_child_name(GTK_STACK(g_calls.stack), empty ? "status" : "list");
        }

        // ---- call-on-phone banner ----

        const nlohmann::json* call_on_phone() {
            if (!g_calls.audio_routable || g_calls.audio_here)
                return nullptr;
            for (const auto& call : g_calls.live)
                if (call.value("connected", false))
                    return &call;
            return nullptr;
        }

        std::string caller_name(const nlohmann::json& call) {
            const std::string name = call.value("name", "");
            const std::string number = call.value("number", "");
            return !name.empty() ? name : (!number.empty() ? number : std::string(_("Unknown caller")));
        }

        // If the call is still on the phone a few seconds in (so not one the
        // laptop is about to take over), say so on the desktop with a button.
        gboolean notify_call_on_phone(gpointer data) {
            std::unique_ptr<std::string> path(static_cast<std::string*>(data));
            const nlohmann::json* call = call_on_phone();
            if (!call || call->value("path", "") != *path || g_calls.notified.count(*path))
                return G_SOURCE_REMOVE;
            g_calls.notified.insert(*path);
            GtkWidget* window = main_window();
            if (window && gtk_window_is_active(GTK_WINDOW(window)))
                return G_SOURCE_REMOVE;  // the banner is right there
            GApplication* app = g_application_get_default();
            if (!app)
                return G_SOURCE_REMOVE;
            GNotification* note = g_notification_new(_("You're on a call on your iPhone"));
            g_notification_set_body(note, caller_name(*call).c_str());
            g_notification_set_icon(note, g_themed_icon_new("call-start-symbolic"));
            g_notification_add_button(note, _("Move to laptop"), "app.move-call-to-laptop");
            g_application_send_notification(app, "call-on-phone", note);
            g_object_unref(note);
            return G_SOURCE_REMOVE;
        }

        void update_banner() {
            if (!g_calls.banner)
                return;
            const nlohmann::json* call = call_on_phone();
            if (!call) {
                gtk_revealer_set_reveal_child(GTK_REVEALER(g_calls.banner), FALSE);
                g_calls.banner_call.clear();
                if (GApplication* app = g_application_get_default())
                    g_application_withdraw_notification(app, "call-on-phone");
                return;
            }
            const std::string path = call->value("path", "");
            gtk_label_set_markup(GTK_LABEL(g_calls.banner_label),
                                 tether::tr_format(_("On a call with <b>{}</b> on your iPhone"),
                                                   escape_markup(caller_name(*call)))
                                     .c_str());
            gtk_revealer_set_reveal_child(GTK_REVEALER(g_calls.banner), TRUE);
            if (g_calls.banner_call != path) {
                g_calls.banner_call = path;
                g_timeout_add_seconds(3, notify_call_on_phone, new std::string(path));
            }
        }

        // ---- number pad ----

        void on_key_clicked(GtkButton* button, gpointer) {
            const char* digit = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "digit"));
            if (!digit || !g_calls.entry)
                return;
            gint position = gtk_editable_get_position(GTK_EDITABLE(g_calls.entry));
            gtk_editable_insert_text(GTK_EDITABLE(g_calls.entry), digit, -1, &position);
            gtk_editable_set_position(GTK_EDITABLE(g_calls.entry), position);
        }

        GtkWidget* build_keypad() {
            GtkWidget* grid = gtk_grid_new();
            gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
            gtk_grid_set_column_spacing(GTK_GRID(grid), 22);
            gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
            static const std::pair<const char*, const char*> keys[] = {
                {"1", ""},     {"2", "ABC"}, {"3", "DEF"},  {"4", "GHI"}, {"5", "JKL"}, {"6", "MNO"},
                {"7", "PQRS"}, {"8", "TUV"}, {"9", "WXYZ"}, {"*", ""},    {"0", "+"},   {"#", ""}};
            for (int i = 0; i < 12; ++i) {
                GtkWidget* key = gtk_button_new();
                gtk_style_context_add_class(gtk_widget_get_style_context(key), "tether-dial-key");
                GtkWidget* face = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_set_valign(face, GTK_ALIGN_CENTER);
                GtkWidget* digit = gtk_label_new(keys[i].first);
                gtk_style_context_add_class(gtk_widget_get_style_context(digit), "tether-dial-digit");
                gtk_box_pack_start(GTK_BOX(face), digit, FALSE, FALSE, 0);
                GtkWidget* letters = gtk_label_new(keys[i].second);
                gtk_style_context_add_class(gtk_widget_get_style_context(letters), "tether-dial-letters");
                gtk_box_pack_start(GTK_BOX(face), letters, FALSE, FALSE, 0);
                gtk_container_add(GTK_CONTAINER(key), face);
                g_object_set_data(G_OBJECT(key), "digit", const_cast<char*>(keys[i].first));
                g_signal_connect(key, "clicked", G_CALLBACK(on_key_clicked), nullptr);
                if (i == 10) {
                    // Holding 0 gives +, as on a phone.
                    GtkGesture* hold = gtk_gesture_long_press_new(key);
                    g_object_set_data_full(G_OBJECT(key), "hold", hold, g_object_unref);
                    g_signal_connect(hold,
                                     "pressed",
                                     G_CALLBACK(+[](GtkGestureLongPress*, gdouble, gdouble, gpointer) {
                                         gint position = gtk_editable_get_position(GTK_EDITABLE(g_calls.entry));
                                         gtk_editable_insert_text(GTK_EDITABLE(g_calls.entry), "+", -1, &position);
                                         gtk_editable_set_position(GTK_EDITABLE(g_calls.entry), position);
                                     }),
                                     nullptr);
                }
                gtk_grid_attach(GTK_GRID(grid), key, i % 3, i / 3, 1, 1);
            }
            return grid;
        }

    } // namespace

    void calls_view_set_on_laptop(bool on) {
        if (!g_calls.laptop_switch || gtk_switch_get_active(GTK_SWITCH(g_calls.laptop_switch)) == on)
            return;
        g_signal_handlers_block_matched(g_calls.laptop_switch, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, &g_calls);
        gtk_switch_set_active(GTK_SWITCH(g_calls.laptop_switch), on);
        g_signal_handlers_unblock_matched(g_calls.laptop_switch, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, &g_calls);
    }

    void calls_view_set_visible(bool visible) {
        g_calls.visible = visible;
        if (!visible)
            return;
        request_calls();
        // Nothing else pulls the address book when this is the first view opened.
        contact_completion_request();
    }

    bool calls_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");

        if (command == "bt_calls") {
            show_calls(event);
            return true;
        }
        if (command == "bt_call_result") {
            if (!event.value("success", false))
                set_status_main(event.value("message", _("The call could not be placed.")));
            return true;
        }
        if (command == "bt_connection_changed") {
            const auto& calls = event.contains("calls") ? event["calls"] : nlohmann::json();
            g_calls.available = calls.is_object() && calls.value("available", false);

            gtk_widget_set_sensitive(g_calls.entry, g_calls.available);
            gtk_widget_set_sensitive(g_calls.dial_button, g_calls.available);
            gtk_widget_set_sensitive(g_calls.dial_phone_button, g_calls.available);
            set_text(g_calls.network_label, g_calls.available ? network_text(calls) : "");
            set_accessible_name(g_calls.network_label, g_calls.available ? network_text(calls, true) : "");

            const std::string reason = calls.is_object() ? calls.value("reason", "") : "";
            const std::string audio = calls.is_object() ? calls.value("audio", "") : "";
            // Any transport state means PipeWire holds Hands-Free and can move the audio either way.
            g_calls.audio_routable = !audio.empty();
            g_calls.audio_here = audio == "active";
            gtk_widget_set_visible(g_calls.dial_button, g_calls.audio_routable);
            update_banner();
            set_text(g_calls.status_label,
                     g_calls.available
                         ? std::string(_("No calls.")) + (reason.empty() ? "" : "\n" + reason)
                         : (reason.empty() ? _("Call control is off. Turn it on with 'tether --bt-calls-enable on'.")
                                           : reason));
            // Always, not only while the tab is open: the banner needs to know.
            request_calls();
            return false;
        }
        return false;
    }

    GtkWidget* calls_banner_new() {
        g_calls.banner = gtk_revealer_new();
        gtk_revealer_set_transition_type(GTK_REVEALER(g_calls.banner), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(box), 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(box), "tether-call-banner");
        gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name("call-start-symbolic", GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
        g_calls.banner_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(g_calls.banner_label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(g_calls.banner_label), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(box), g_calls.banner_label, TRUE, TRUE, 0);
        GtkWidget* move = gtk_button_new_with_label(_("Move to laptop"));
        gtk_style_context_add_class(gtk_widget_get_style_context(move), "suggested-action");
        gtk_widget_set_valign(move, GTK_ALIGN_CENTER);
        g_signal_connect(move, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { send_action("audio_here", ""); }), nullptr);
        gtk_box_pack_start(GTK_BOX(box), move, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(g_calls.banner), box);
        gtk_widget_show_all(box);

        // The desktop notification's button lands here.
        if (GApplication* app = g_application_get_default()) {
            if (!g_action_map_lookup_action(G_ACTION_MAP(app), "move-call-to-laptop")) {
                GSimpleAction* action = g_simple_action_new("move-call-to-laptop", nullptr);
                g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer) {
                                     send_action("audio_here", "");
                                 }),
                                 nullptr);
                g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
                g_object_unref(action);
            }
        }
        return g_calls.banner;
    }

    GtkWidget* calls_view_new() {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget* dial_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(dial_bar), 10);
        g_calls.entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_calls.entry), _("Number to call"));
        set_accessible_name(g_calls.entry, _("Number to call"));
        gtk_entry_set_input_purpose(GTK_ENTRY(g_calls.entry), GTK_INPUT_PURPOSE_PHONE);
        attach_contact_completion(g_calls.entry, ContactKind::Tel);
        gtk_widget_set_sensitive(g_calls.entry, FALSE);
        g_signal_connect(g_calls.entry, "activate", G_CALLBACK(on_dial_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.entry, TRUE, TRUE, 0);

        g_calls.dial_button = gtk_button_new_with_label(_("Call from laptop"));
        gtk_widget_set_tooltip_text(g_calls.dial_button, _("Place the call with its audio on this computer."));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.dial_button), "suggested-action");
        gtk_widget_set_sensitive(g_calls.dial_button, FALSE);
        gtk_widget_set_no_show_all(g_calls.dial_button, TRUE);
        g_signal_connect(g_calls.dial_button, "clicked", G_CALLBACK(on_dial_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.dial_button, FALSE, FALSE, 0);

        g_calls.dial_phone_button = gtk_button_new_with_label(_("Call on iPhone"));
        gtk_widget_set_tooltip_text(g_calls.dial_phone_button, _("Place the call with its audio on the iPhone."));
        gtk_widget_set_sensitive(g_calls.dial_phone_button, FALSE);
        g_signal_connect(g_calls.dial_phone_button, "clicked", G_CALLBACK(on_dial_phone_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.dial_phone_button, FALSE, FALSE, 0);

        g_calls.network_label = gtk_label_new("");
        gtk_widget_set_tooltip_text(g_calls.network_label, _("The iPhone's carrier, signal, and battery."));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.network_label), "muted");
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.network_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), dial_bar, FALSE, FALSE, 0);

        GtkWidget* laptop_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(laptop_row), 10);
        GtkWidget* laptop_label = gtk_label_new(_("Always use laptop for calls"));
        gtk_label_set_xalign(GTK_LABEL(laptop_label), 0.0);
        gtk_widget_set_tooltip_text(laptop_label, _("Every call plays on this computer, even ones answered on the iPhone."));
        gtk_box_pack_start(GTK_BOX(laptop_row), laptop_label, TRUE, TRUE, 0);
        g_calls.laptop_switch = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(g_calls.laptop_switch), TRUE);
        g_signal_connect(g_calls.laptop_switch,
                         "notify::active",
                         G_CALLBACK(+[](GtkSwitch* sw, GParamSpec*, gpointer) {
                             daemon_send({{"command", "bt_set_calls_on_laptop"}, {"enabled", gtk_switch_get_active(sw) == TRUE}});
                         }),
                         &g_calls);
        gtk_box_pack_start(GTK_BOX(laptop_row), g_calls.laptop_switch, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), laptop_row, FALSE, FALSE, 0);

        GtkWidget* keypad = build_keypad();
        gtk_widget_set_margin_top(keypad, 6);
        gtk_widget_set_margin_bottom(keypad, 10);
        GtkWidget* backspace = gtk_button_new_from_icon_name("edit-clear-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(backspace), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(backspace, _("Delete"));
        g_signal_connect(backspace,
                         "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer) {
                             gint position = gtk_editable_get_position(GTK_EDITABLE(g_calls.entry));
                             if (position > 0)
                                 gtk_editable_delete_text(GTK_EDITABLE(g_calls.entry), position - 1, position);
                         }),
                         nullptr);
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(g_calls.entry), GTK_ENTRY_ICON_SECONDARY, "edit-clear-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(g_calls.entry), GTK_ENTRY_ICON_SECONDARY, _("Delete"));
        g_signal_connect(g_calls.entry,
                         "icon-press",
                         G_CALLBACK(+[](GtkEntry* entry, GtkEntryIconPosition, GdkEvent*, gpointer) {
                             gint position = gtk_editable_get_position(GTK_EDITABLE(entry));
                             if (position > 0)
                                 gtk_editable_delete_text(GTK_EDITABLE(entry), position - 1, position);
                         }),
                         nullptr);
        g_object_ref_sink(backspace);
        g_object_unref(backspace);
        gtk_box_pack_start(GTK_BOX(root), keypad, FALSE, FALSE, 0);

        g_calls.stack = gtk_stack_new();

        GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
        gtk_box_pack_start(
            GTK_BOX(status_box), gtk_image_new_from_icon_name("call-start", GTK_ICON_SIZE_DIALOG), FALSE, FALSE, 0);
        g_calls.status_label = gtk_label_new(_("Waiting for the iPhone."));
        gtk_label_set_line_wrap(GTK_LABEL(g_calls.status_label), TRUE);
        gtk_label_set_justify(GTK_LABEL(g_calls.status_label), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.status_label), "muted");
        gtk_box_pack_start(GTK_BOX(status_box), g_calls.status_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_calls.stack), status_box, "status");

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_calls.list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_calls.list), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), g_calls.list);
        gtk_stack_add_named(GTK_STACK(g_calls.stack), scroll, "list");

        gtk_stack_set_visible_child_name(GTK_STACK(g_calls.stack), "status");
        gtk_box_pack_start(GTK_BOX(root), g_calls.stack, TRUE, TRUE, 0);
        return root;
    }

} // namespace tether::ui
