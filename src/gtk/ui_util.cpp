#include "ui_util.hpp"

#include "tray.hpp"

#include <tether/i18n.hpp>
#include <tether/version.hpp>

#include <cmath>

namespace tether::ui {

    namespace {
        GtkWidget* g_window = nullptr;
        GtkWidget* g_header_bar = nullptr;

        struct RouteIndicator {
            GtkWidget* box = nullptr;
            GtkWidget* icon = nullptr;
            GtkWidget* label = nullptr;
            const char* name = nullptr;
            const char* icon_ok = nullptr;
            const char* icon_off = nullptr;
        };

        RouteIndicator g_routes[2];

        RouteIndicator& indicator(Route route) { return g_routes[route == Route::WiFi ? 0 : 1]; }

        constexpr const char* STYLE = R"CSS(
.muted {
    opacity: 0.75;
    font-size: 90%;
}

/* Navbar in the title bar: flat items, the current page underlined in blue. */
.tether-navbar {
    margin: 0 8px;
}

button.tether-nav-item {
    background: none;
    border: none;
    box-shadow: none;
    border-radius: 8px;
    padding: 4px 12px;
    border-bottom: 2px solid transparent;
    opacity: 0.7;
    transition: all 150ms ease-out;
}

button.tether-nav-item:hover {
    background-color: alpha(@theme_fg_color, 0.07);
    opacity: 1;
}

button.tether-nav-item:checked {
    opacity: 1;
    border-radius: 8px 8px 2px 2px;
    border-bottom: 2px solid #0a84ff;
    background-color: alpha(#0a84ff, 0.12);
}

/* Call controls. */
.tether-call-banner {
    background-color: alpha(#23a55a, 0.22);
    border-bottom: 1px solid alpha(#23a55a, 0.45);
}

button.tether-dial-key {
    min-width: 64px;
    min-height: 64px;
    border-radius: 32px;
    padding: 0;
    background-image: none;
    background-color: alpha(@theme_fg_color, 0.08);
    border: none;
    box-shadow: none;
    transition: background-color 100ms ease-out;
}

button.tether-dial-key:hover {
    background-color: alpha(@theme_fg_color, 0.14);
}

button.tether-dial-key:active {
    background-color: alpha(@theme_fg_color, 0.24);
}

.tether-dial-digit {
    font-size: 160%;
}

.tether-dial-letters {
    font-size: 62%;
    letter-spacing: 2px;
    opacity: 0.6;
}

/* Messages, styled after iMessage. */
.tether-bubble {
    padding: 7px 13px;
    border-radius: 18px;
    font-size: 105%;
}

.tether-bubble-in {
    background-color: alpha(@theme_fg_color, 0.13);
}

.tether-bubble-out {
    background-color: #0a84ff;
    color: #ffffff;
}

.tether-bubble-out link {
    color: #ffffff;
}

.tether-bubble-emoji {
    background-color: transparent;
    font-size: 300%;
    padding: 0 2px;
}

.tether-pending .tether-bubble {
    opacity: 0.6;
}

.tether-search-hit {
    background-color: alpha(#0a84ff, 0.22);
}

.tether-section-label {
    font-size: 78%;
    font-weight: bold;
    opacity: 0.6;
}

.tether-stamp {
    font-size: 78%;
    opacity: 0.55;
}

.tether-reply-quote {
    font-size: 88%;
    opacity: 0.7;
    padding: 4px 10px;
    border-radius: 12px;
    border: 1px solid alpha(@theme_fg_color, 0.18);
}

.tether-reaction {
    font-size: 95%;
    padding: 1px 6px;
    border-radius: 12px;
    background-color: alpha(@theme_fg_color, 0.13);
    border: 2px solid @theme_base_color;
    margin-bottom: -6px;
}

.tether-message-row {
    background: none;
    transition: background-color 120ms ease-out;
}

/* Discord's faint highlight under the message the pointer is on. */
.tether-message-row:hover {
    background-color: alpha(@theme_fg_color, 0.035);
}

button.tether-reply-count {
    font-size: 82%;
    padding: 0 6px;
    min-height: 18px;
    color: #0a84ff;
}

button.tether-message-action {
    min-width: 24px;
    min-height: 24px;
    padding: 2px;
    border-radius: 12px;
    opacity: 0.7;
}

button.tether-message-action:hover {
    opacity: 1;
}

.tether-conversation-header {
    border-bottom: 1px solid alpha(@theme_fg_color, 0.10);
}

.tether-composer {
    transition: border-color 150ms ease-out;
    border-radius: 19px;
    border: 1px solid alpha(@theme_fg_color, 0.22);
    padding: 2px 3px 2px 3px;
    background-color: @theme_base_color;
}

.tether-composer textview,
.tether-composer textview text,
.tether-composer scrolledwindow {
    background: transparent;
    border: none;
    box-shadow: none;
}

.tether-composer:focus-within {
    border-color: alpha(#0a84ff, 0.7);
}

.tether-placeholder {
    opacity: 0.45;
}

button.tether-composer-button {
    min-width: 30px;
    min-height: 30px;
    padding: 0;
    border-radius: 15px;
}

button.tether-composer-button,
button.tether-send,
button.tether-message-action {
    transition: all 120ms ease-out;
}

button.tether-send:hover {
    background-color: #3d9bff;
}

button.tether-send {
    min-width: 30px;
    min-height: 30px;
    padding: 0;
    border-radius: 15px;
    border: none;
    background-image: none;
    background-color: #0a84ff;
    color: #ffffff;
    -gtk-icon-shadow: none;
}

button.tether-send:disabled {
    background-color: alpha(@theme_fg_color, 0.18);
    color: alpha(#ffffff, 0.8);
}

.tether-attachment image {
    border-radius: 8px;
    background-color: alpha(@theme_fg_color, 0.06);
}

button.tether-attachment-remove {
    min-width: 20px;
    min-height: 20px;
    padding: 0;
    margin: 2px;
    border-radius: 10px;
    background: alpha(black, 0.6);
    color: white;
    border: none;
}

button.tether-gif-button {
    font-weight: 800;
    font-size: 72%;
    padding: 0 4px;
}

.tether-link-card {
    border-radius: 14px;
    background-color: alpha(@theme_fg_color, 0.07);
    border: 1px solid alpha(@theme_fg_color, 0.10);
    transition: background-color 120ms ease-out;
}

.tether-link-card:hover {
    background-color: alpha(@theme_fg_color, 0.11);
}

.tether-link-card image {
    border-radius: 14px 14px 0 0;
}

.tether-link-site {
    font-size: 78%;
    opacity: 0.6;
}

.tether-link-title {
    font-weight: bold;
}

.tether-link-description {
    font-size: 88%;
    opacity: 0.75;
}

.tether-media image {
    border-radius: 14px;
}

.tether-reply-bar {
    font-size: 90%;
    opacity: 0.8;
}

/* Conversation list: rounded rows with a blue selection, as in Messages. */
.tether-thread-list row {
    border-radius: 10px;
    margin: 1px 8px;
    transition: background-color 120ms ease-out;
}

.tether-thread-list row:hover:not(:selected) {
    background-color: alpha(@theme_fg_color, 0.06);
}

.tether-thread-list row:selected {
    background-color: #0a84ff;
    color: #ffffff;
}

.tether-thread-list row:selected .muted {
    opacity: 0.9;
}

.tether-search {
    border-radius: 9px;
}

.tether-route-bar {
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-route-off {
    opacity: 0.75;
}

.tether-setup {
    background-color: alpha(@theme_fg_color, 0.07);
    border: 1px solid alpha(@theme_fg_color, 0.18);
    border-radius: 8px;
    padding: 12px;
}

.tether-setup-command {
    font-family: monospace;
    font-size: 92%;
}

.tether-thread-unread {
    opacity: 1;
    font-weight: bold;
}

.tether-send-error {
    background-color: alpha(#e5a50a, 0.20);
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-badge {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
    border-radius: 10px;
    padding: 0 8px;
}

.tether-dropzone {
    border: 2px dashed alpha(@theme_fg_color, 0.28);
    border-radius: 12px;
    padding: 20px 28px;
}

.tether-dropzone-active {
    border-color: @theme_selected_bg_color;
    background-color: alpha(@theme_selected_bg_color, 0.12);
}
)CSS";
        // xdg-desktop-portal Settings: 0 = no preference, 1 = dark, 2 = light.
        void apply_color_scheme(guint32 scheme) {
            GtkSettings* settings = gtk_settings_get_default();
            if (!settings)
                return;
            if (scheme == 1 || scheme == 2)
                g_object_set(settings, "gtk-application-prefer-dark-theme", scheme == 1 ? TRUE : FALSE, nullptr);
        }

        void read_color_scheme(GDBusProxy* proxy) {
            GError* error = nullptr;
            GVariant* result =
                g_dbus_proxy_call_sync(proxy,
                                       "Read",
                                       g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       nullptr,
                                       &error);
            if (!result) {
                g_clear_error(&error);
                return;
            }
            // Read returns the value boxed twice: (v) holding a v holding the uint32.
            GVariant* outer = nullptr;
            g_variant_get(result, "(v)", &outer);
            GVariant* inner = g_variant_get_variant(outer);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32))
                apply_color_scheme(g_variant_get_uint32(inner));
            g_variant_unref(inner);
            g_variant_unref(outer);
            g_variant_unref(result);
        }

        void on_setting_changed(GDBusProxy*, const gchar*, const gchar* signal_name, GVariant* params, gpointer) {
            if (g_strcmp0(signal_name, "SettingChanged") != 0)
                return;
            const gchar* nspace = nullptr;
            const gchar* key = nullptr;
            GVariant* value = nullptr;
            g_variant_get(params, "(&s&sv)", &nspace, &key, &value);
            if (g_strcmp0(nspace, "org.freedesktop.appearance") == 0 && g_strcmp0(key, "color-scheme") == 0 &&
                g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
                apply_color_scheme(g_variant_get_uint32(value));
            g_variant_unref(value);
        }
    } // namespace

    void follow_system_color_scheme() {
        // An explicit GTK_THEME overrides.
        if (g_getenv("GTK_THEME"))
            return;

        static GDBusProxy* proxy = nullptr;
        if (proxy)
            return;

        GError* error = nullptr;
        proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                              nullptr,
                                              "org.freedesktop.portal.Desktop",
                                              "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.Settings",
                                              nullptr,
                                              &error);
        if (!proxy) {
            // no portal
            g_clear_error(&error);
            return;
        }

        g_signal_connect(proxy, "g-signal", G_CALLBACK(on_setting_changed), nullptr);
        read_color_scheme(proxy);
    }

    void install_style() {
        GdkScreen* screen = gdk_screen_get_default();
        if (!screen)
            return;

        GtkCssProvider* provider = gtk_css_provider_new();
        GError* error = nullptr;
        if (!gtk_css_provider_load_from_data(provider, STYLE, -1, &error)) {
            g_warning("tether: stylesheet rejected: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        }
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
    }

    std::string fold(const std::string& text) {
        gchar* normalized = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_ALL);
        gchar* folded = g_utf8_casefold(normalized ? normalized : text.c_str(), -1);
        std::string out = folded ? folded : "";
        g_free(normalized);
        g_free(folded);
        return out;
    }

    std::string escape_markup(const std::string& text) {
        gchar* escaped = g_markup_escape_text(text.c_str(), -1);
        std::string result = escaped ? escaped : "";
        g_free(escaped);
        return result;
    }

    void set_markup(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_markup(GTK_LABEL(label), text.c_str());
        }
    }

    void set_accessible_name(GtkWidget* widget, const std::string& name) {
        if (widget) {
            atk_object_set_name(gtk_widget_get_accessible(widget), name.c_str());
        }
    }

    void set_text(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
        }
    }

    void clear_list_box(GtkWidget* list_box) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(list_box));
        for (GList* iter = children; iter; iter = g_list_next(iter)) {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }

    void set_main_window(GtkWidget* window) { g_window = window; }

    GtkWidget* main_window() { return g_window; }

    void set_header_bar(GtkWidget* bar) { g_header_bar = bar; }

    void set_status_main(const std::string& text) {
        if (g_header_bar) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(g_header_bar), text.c_str());
        }
    }

    GtkWidget* create_route_bar() {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(bar), "tether-route-bar");

        auto build = [](RouteIndicator& route, const char* name, const char* icon_ok, const char* icon_off) {
            route.icon_ok = icon_ok;
            route.icon_off = icon_off;
            route.name = name;
            route.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            route.icon = gtk_image_new_from_icon_name(icon_off, GTK_ICON_SIZE_MENU);
            gtk_box_pack_start(GTK_BOX(route.box), route.icon, FALSE, FALSE, 0);
            route.label = gtk_label_new(nullptr);
            gtk_label_set_ellipsize(GTK_LABEL(route.label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(route.box), route.label, FALSE, FALSE, 0);
        };

        build(indicator(Route::WiFi),
              "Wi-Fi",
              "network-wireless-signal-excellent-symbolic",
              "network-wireless-offline-symbolic");
        build(indicator(Route::Bluetooth), "Bluetooth", "bluetooth-active-symbolic", "bluetooth-disabled-symbolic");

        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::WiFi).box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::Bluetooth).box, FALSE, FALSE, 0);

        GtkWidget* version = gtk_label_new("v" TETHER_VERSION);
        gtk_style_context_add_class(gtk_widget_get_style_context(version), "muted");
        gtk_box_pack_end(GTK_BOX(bar), version, FALSE, FALSE, 0);

        set_route_status(Route::WiFi, false, _("Waiting for the Tether daemon."));
        set_route_status(Route::Bluetooth, false, _("Waiting for the Tether daemon."));
        return bar;
    }

    void set_route_status(Route route, bool ok, const std::string& detail) {
        RouteIndicator& r = indicator(route);
        if (!r.box)
            return;

        gtk_image_set_from_icon_name(GTK_IMAGE(r.icon), ok ? r.icon_ok : r.icon_off, GTK_ICON_SIZE_MENU);
        GtkStyleContext* context = gtk_widget_get_style_context(r.box);
        if (ok)
            gtk_style_context_remove_class(context, "tether-route-off");
        else
            gtk_style_context_add_class(context, "tether-route-off");

        // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth".
        const std::string state = tr_format(_("{}: {}"), r.name, ok ? _("connected") : _("not connected"));
        set_text(r.label, state);
        // The reason can be a sentence or two, which would push the other route
        // off the strip, so it lives in the tooltip. The Devices page shows it
        // in full.
        gtk_widget_set_tooltip_text(r.box, detail.empty() ? state.c_str() : (state + "\n" + detail).c_str());
        atk_object_set_description(gtk_widget_get_accessible(r.label), detail.c_str());

        tray_set_route(route, ok, detail);
    }

    namespace {

        struct Avatar {
            GdkPixbuf* pixbuf = nullptr;
            std::string initials;
            double r = 0, g = 0, b = 0;
            int size = 0;
        };

        void free_avatar(gpointer data) {
            auto* avatar = static_cast<Avatar*>(data);
            if (avatar->pixbuf)
                g_object_unref(avatar->pixbuf);
            delete avatar;
        }

        std::string initials_of(const std::string& name) {
            std::string out;
            bool at_word = true;
            for (const char* p = name.c_str(); *p && out.size() < 8;) {
                const gunichar c = g_utf8_get_char(p);
                if (g_unichar_isspace(c)) {
                    at_word = true;
                } else if (at_word) {
                    at_word = false;
                    if (g_unichar_isalnum(c)) {
                        char buf[6];
                        out.append(buf, g_unichar_to_utf8(g_unichar_toupper(c), buf));
                        if (g_utf8_strlen(out.c_str(), -1) == 2)
                            break;
                    }
                }
                p = g_utf8_next_char(p);
            }
            return out.empty() ? "#" : out;
        }

        gboolean draw_avatar(GtkWidget* widget, cairo_t* cr, gpointer data) {
            const auto* avatar = static_cast<Avatar*>(data);
            const double size = avatar->size;
            cairo_arc(cr, size / 2, size / 2, size / 2, 0, 2 * M_PI);
            cairo_clip(cr);
            if (avatar->pixbuf) {
                const int w = gdk_pixbuf_get_width(avatar->pixbuf);
                const int h = gdk_pixbuf_get_height(avatar->pixbuf);
                gdk_cairo_set_source_pixbuf(cr, avatar->pixbuf, (size - w) / 2.0, (size - h) / 2.0);
                cairo_paint(cr);
                return TRUE;
            }
            cairo_set_source_rgb(cr, avatar->r, avatar->g, avatar->b);
            cairo_paint(cr);

            PangoLayout* layout = gtk_widget_create_pango_layout(widget, avatar->initials.c_str());
            PangoFontDescription* font = pango_font_description_new();
            pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
            pango_font_description_set_absolute_size(font, size * 0.4 * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, (size - tw) / 2.0, (size - th) / 2.0);
            pango_cairo_show_layout(cr, layout);
            pango_font_description_free(font);
            g_object_unref(layout);
            return TRUE;
        }

    } // namespace

    GtkWidget* avatar_new(const std::string& photo_path, const std::string& name, int size) {
        auto* avatar = new Avatar;
        avatar->size = size;
        avatar->initials = initials_of(name);

        if (!photo_path.empty()) {
            if (GdkPixbuf* raw = gdk_pixbuf_new_from_file(photo_path.c_str(), nullptr)) {
                // Cover the circle: scale the short side to fit, crop the long one.
                const double w = gdk_pixbuf_get_width(raw), h = gdk_pixbuf_get_height(raw);
                const double scale = size / std::min(w, h);
                avatar->pixbuf = gdk_pixbuf_scale_simple(raw,
                                                         std::max(1, static_cast<int>(std::lround(w * scale))),
                                                         std::max(1, static_cast<int>(std::lround(h * scale))),
                                                         GDK_INTERP_BILINEAR);
                g_object_unref(raw);
            }
        }

        // A stable colour per name, from a small palette that holds white text.
        static const double palette[][3] = {{0.35, 0.40, 0.95},
                                            {0.23, 0.65, 0.36},
                                            {0.98, 0.65, 0.10},
                                            {0.93, 0.26, 0.27},
                                            {0.92, 0.27, 0.62},
                                            {0.00, 0.66, 0.99},
                                            {0.61, 0.52, 0.93},
                                            {0.10, 0.74, 0.61}};
        const auto& colour = palette[g_str_hash(name.c_str()) % G_N_ELEMENTS(palette)];
        avatar->r = colour[0];
        avatar->g = colour[1];
        avatar->b = colour[2];

        GtkWidget* area = gtk_drawing_area_new();
        gtk_widget_set_size_request(area, size, size);
        gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
        g_signal_connect_data(area, "draw", G_CALLBACK(draw_avatar), avatar, (GClosureNotify)free_avatar, GConnectFlags(0));
        return area;
    }

} // namespace tether::ui
