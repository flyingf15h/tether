#include "media.hpp"
#include "prefs.hpp"
#include "ui_util.hpp"

#include <tether/i18n.hpp>

#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

namespace tether::ui {

    namespace {

        constexpr int MEDIA_MAX_WIDTH = 260;
        constexpr const char* GIPHY_API = "https://api.giphy.com/v1/gifs/";
        constexpr const char* GIPHY_KEY_PREF = "giphy_api_key";

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        }

        std::string cache_dir() {
            const std::string dir = std::string(g_get_user_cache_dir()) + "/tether/media";
            g_mkdir_with_parents(dir.c_str(), 0700);
            return dir;
        }

        std::string cache_path(const std::string& url) {
            gchar* digest = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url.c_str(), -1);
            const std::string path = cache_dir() + "/" + digest;
            g_free(digest);
            return path;
        }

        struct Fetch {
            std::string path;
            std::string tmp;
            std::function<void(bool ok, const std::string& path, const std::string& body)> done;
            bool to_memory = false;
        };

        void on_fetch_done(GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Fetch> fetch(static_cast<Fetch*>(data));
            GSubprocess* proc = G_SUBPROCESS(source);
            gchar* out = nullptr;
            GError* error = nullptr;
            const bool ran = g_subprocess_communicate_utf8_finish(proc, result, &out, nullptr, &error);
            const bool ok = ran && g_subprocess_get_successful(proc);
            const std::string body = out ? out : "";
            g_free(out);
            g_clear_error(&error);
            g_object_unref(proc);
            if (!fetch->to_memory) {
                if (ok)
                    g_rename(fetch->tmp.c_str(), fetch->path.c_str());
                else
                    g_remove(fetch->tmp.c_str());
            }
            fetch->done(ok, fetch->path, body);
        }

        // curl in the background: into the media cache, or into memory for API calls.
        void fetch(const std::string& url,
                   bool to_memory,
                   std::function<void(bool ok, const std::string& path, const std::string& body)> done) {
            auto* job = new Fetch{to_memory ? "" : cache_path(url), "", std::move(done), to_memory};
            if (!to_memory && g_file_test(job->path.c_str(), G_FILE_TEST_EXISTS)) {
                std::unique_ptr<Fetch> owned(job);
                owned->done(true, owned->path, "");
                return;
            }
            std::vector<const gchar*> argv = {"curl", "-sSL", "--fail", "--max-time", "30"};
            if (!to_memory) {
                job->tmp = job->path + ".part";
                argv.push_back("-o");
                argv.push_back(job->tmp.c_str());
            }
            argv.push_back(url.c_str());
            argv.push_back(nullptr);
            GError* error = nullptr;
            GSubprocess* proc = g_subprocess_newv(
                argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &error);
            if (!proc) {
                g_clear_error(&error);
                std::unique_ptr<Fetch> owned(job);
                owned->done(false, owned->path, "");
                return;
            }
            g_subprocess_communicate_utf8_async(proc, nullptr, nullptr, on_fetch_done, job);
        }

        // GIPHY serves a 200px-wide copy of every GIF, much lighter in a chat.
        std::string display_url(const std::string& url) {
            const std::string l = lower(url);
            if (l.find("giphy.com/media/") != std::string::npos) {
                const size_t slash = url.rfind('/');
                if (slash != std::string::npos)
                    return url.substr(0, slash) + "/200w.gif";
            }
            return url;
        }

        // Fills `image` from a downloaded file: animated if it is a GIF, scaled otherwise.
        bool show_file(GtkWidget* image, const std::string& path) {
            GdkPixbufAnimation* animation = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
            if (!animation)
                return false;
            if (!gdk_pixbuf_animation_is_static_image(animation) &&
                gdk_pixbuf_animation_get_width(animation) <= MEDIA_MAX_WIDTH * 2) {
                gtk_image_set_from_animation(GTK_IMAGE(image), animation);
            } else {
                GdkPixbuf* still = gdk_pixbuf_animation_get_static_image(animation);
                const int w = gdk_pixbuf_get_width(still), h = gdk_pixbuf_get_height(still);
                const double scale = w > MEDIA_MAX_WIDTH ? double(MEDIA_MAX_WIDTH) / w : 1.0;
                GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
                    still, std::max(1, int(w * scale)), std::max(1, int(h * scale)), GDK_INTERP_BILINEAR);
                gtk_image_set_from_pixbuf(GTK_IMAGE(image), scaled);
                g_object_unref(scaled);
            }
            g_object_unref(animation);
            return true;
        }

        gboolean open_link(GtkWidget* widget, GdkEventButton* event, gpointer) {
            if (event->button != 1)
                return FALSE;
            const char* url = static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "url"));
            GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
            if (url)
                gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr, url, GDK_CURRENT_TIME, nullptr);
            return TRUE;
        }

    } // namespace

    bool is_media_url(const std::string& text) {
        if (text.find_first_of(" \n\t") != std::string::npos || text.rfind("https://", 0) != 0)
            return false;
        const std::string l = lower(text);
        const std::string path = l.substr(0, l.find('?'));
        for (const char* ext : {".gif", ".png", ".jpg", ".jpeg", ".webp"})
            if (path.size() > strlen(ext) && path.compare(path.size() - strlen(ext), strlen(ext), ext) == 0)
                return true;
        return l.find("giphy.com/media/") != std::string::npos || l.rfind("https://i.giphy.com/", 0) == 0 ||
               l.find(".tenor.com/") != std::string::npos;
    }

    GtkWidget* media_bubble_new(const std::string& url) {
        GtkWidget* box = gtk_event_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(box), "tether-media");
        g_object_set_data_full(G_OBJECT(box), "url", g_strdup(url.c_str()), g_free);
        gtk_widget_set_tooltip_text(box, url.c_str());
        g_signal_connect(box, "button-press-event", G_CALLBACK(open_link), nullptr);

        GtkWidget* image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_size_request(image, 120, 80);
        gtk_container_add(GTK_CONTAINER(box), image);

        // The row may be gone (conversation switched) before the download lands.
        GWeakRef* weak = g_new0(GWeakRef, 1);
        g_weak_ref_init(weak, image);
        fetch(display_url(url), false, [weak, url](bool ok, const std::string& path, const std::string&) {
            GtkWidget* image = GTK_WIDGET(g_weak_ref_get(weak));
            g_weak_ref_clear(weak);
            g_free(weak);
            if (!image)
                return;
            if (!ok || !show_file(image, path)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(image), "image-missing-symbolic", GTK_ICON_SIZE_DIALOG);
            } else {
                gtk_widget_set_size_request(image, -1, -1);
            }
            g_object_unref(image);
        });
        return box;
    }

    namespace {

        struct Picker {
            GtkWidget* popover = nullptr;
            GtkWidget* stack = nullptr;
            GtkWidget* search = nullptr;
            GtkWidget* grid = nullptr;
            GtkWidget* status = nullptr;
            std::function<void(const std::string&)> on_pick;
            unsigned generation = 0;
        };

        std::string giphy_key() {
            auto& p = prefs();
            return p.contains(GIPHY_KEY_PREF) && p[GIPHY_KEY_PREF].is_string() ? p[GIPHY_KEY_PREF].get<std::string>() : "";
        }

        void load_results(Picker* picker, const std::string& query) {
            const unsigned generation = ++picker->generation;
            gchar* escaped = g_uri_escape_string(query.c_str(), nullptr, FALSE);
            const std::string key = giphy_key();
            const std::string url = query.empty()
                                        ? std::string(GIPHY_API) + "trending?limit=24&rating=pg-13&api_key=" + key
                                        : std::string(GIPHY_API) + "search?limit=24&rating=pg-13&api_key=" + key +
                                              "&q=" + escaped;
            g_free(escaped);
            gtk_label_set_text(GTK_LABEL(picker->status), _("Loading…"));
            gtk_widget_show(picker->status);

            // The picker owns its popover; a weak ref drops answers after it closes.
            GWeakRef* weak = g_new0(GWeakRef, 1);
            g_weak_ref_init(weak, picker->popover);
            fetch(url, true, [weak, picker, generation](bool ok, const std::string&, const std::string& body) {
                GObject* alive = static_cast<GObject*>(g_weak_ref_get(weak));
                g_weak_ref_clear(weak);
                g_free(weak);
                if (!alive)
                    return;
                g_object_unref(alive);
                if (generation != picker->generation)
                    return;
                clear_list_box(picker->grid);
                auto json = nlohmann::json::parse(body, nullptr, false);
                if (!ok || json.is_discarded() || !json.contains("data")) {
                    gtk_label_set_text(GTK_LABEL(picker->status),
                                       _("GIPHY did not answer. Check the API key in the menu (⋯) or your connection."));
                    return;
                }
                gtk_widget_hide(picker->status);
                for (const auto& gif : json["data"]) {
                    const auto& images = gif.value("images", nlohmann::json::object());
                    const std::string preview =
                        images.value("fixed_width_small", nlohmann::json::object()).value("url", "");
                    std::string full = images.value("original", nlohmann::json::object()).value("url", "");
                    full = full.substr(0, full.find('?'));
                    if (preview.empty() || full.empty())
                        continue;
                    GtkWidget* button = gtk_button_new();
                    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
                    gtk_widget_set_tooltip_text(button, gif.value("title", "").c_str());
                    GtkWidget* image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
                    gtk_widget_set_size_request(image, 100, 80);
                    gtk_container_add(GTK_CONTAINER(button), image);
                    g_object_set_data_full(G_OBJECT(button), "url", g_strdup(full.c_str()), g_free);
                    g_signal_connect(button,
                                     "clicked",
                                     G_CALLBACK(+[](GtkButton* b, gpointer data) {
                                         auto* picker = static_cast<Picker*>(data);
                                         const std::string url = (const char*)g_object_get_data(G_OBJECT(b), "url");
                                         auto pick = picker->on_pick;
                                         gtk_popover_popdown(GTK_POPOVER(picker->popover));
                                         pick(url);
                                     }),
                                     picker);
                    gtk_flow_box_insert(GTK_FLOW_BOX(picker->grid), button, -1);
                    GWeakRef* image_ref = g_new0(GWeakRef, 1);
                    g_weak_ref_init(image_ref, image);
                    fetch(preview, false, [image_ref](bool ok, const std::string& path, const std::string&) {
                        GtkWidget* image = GTK_WIDGET(g_weak_ref_get(image_ref));
                        g_weak_ref_clear(image_ref);
                        g_free(image_ref);
                        if (!image)
                            return;
                        if (ok)
                            show_file(image, path);
                        g_object_unref(image);
                    });
                }
                gtk_widget_show_all(picker->grid);
            });
        }

        GtkWidget* build_key_page(Picker* picker) {
            GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(page), 12);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), (std::string("<b>") + _("Connect GIPHY") + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
            GtkWidget* hint = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(hint),
                                 _("GIF search needs a free API key. Create one at "
                                   "<a href=\"https://developers.giphy.com/dashboard/\">developers.giphy.com</a> "
                                   "and paste it here."));
            gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(hint), 40);
            gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
            gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);
            GtkWidget* entry = gtk_entry_new();
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("GIPHY API key"));
            gtk_box_pack_start(GTK_BOX(page), entry, FALSE, FALSE, 0);
            GtkWidget* save = gtk_button_new_with_label(_("Save"));
            gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
            gtk_widget_set_halign(save, GTK_ALIGN_END);
            g_object_set_data(G_OBJECT(save), "entry", entry);
            auto on_save = +[](GtkWidget* w, gpointer data) {
                auto* picker = static_cast<Picker*>(data);
                GtkWidget* entry = GTK_IS_ENTRY(w) ? w : GTK_WIDGET(g_object_get_data(G_OBJECT(w), "entry"));
                std::string key = gtk_entry_get_text(GTK_ENTRY(entry));
                key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }),
                          key.end());
                if (key.empty())
                    return;
                prefs()[GIPHY_KEY_PREF] = key;
                prefs_save();
                gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "gifs");
                gtk_widget_grab_focus(picker->search);
                load_results(picker, "");
            };
            g_signal_connect(save, "clicked", G_CALLBACK(on_save), picker);
            g_signal_connect(entry, "activate", G_CALLBACK(on_save), picker);
            gtk_box_pack_start(GTK_BOX(page), save, FALSE, FALSE, 0);
            return page;
        }

    } // namespace

    void gif_picker_open(GtkWidget* relative_to, std::function<void(const std::string& url)> on_pick) {
        auto* picker = new Picker;
        picker->on_pick = std::move(on_pick);
        picker->popover = gtk_popover_new(relative_to);
        gtk_style_context_add_class(gtk_widget_get_style_context(picker->popover), "tether-gif-picker");
        g_object_set_data_full(G_OBJECT(picker->popover), "picker", picker, [](gpointer p) { delete static_cast<Picker*>(p); });

        picker->stack = gtk_stack_new();
        gtk_stack_add_named(GTK_STACK(picker->stack), build_key_page(picker), "key");

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 8);
        GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        picker->search = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search), _("Search GIPHY"));
        g_signal_connect(picker->search,
                         "search-changed",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                             load_results(static_cast<Picker*>(data), gtk_entry_get_text(GTK_ENTRY(entry)));
                         }),
                         picker);
        gtk_box_pack_start(GTK_BOX(top), picker->search, TRUE, TRUE, 0);
        GtkWidget* change_key = gtk_button_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(change_key), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(change_key, _("Change GIPHY API key"));
        g_signal_connect(change_key,
                         "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer data) {
                             gtk_stack_set_visible_child_name(GTK_STACK(static_cast<Picker*>(data)->stack), "key");
                         }),
                         picker);
        gtk_box_pack_start(GTK_BOX(top), change_key, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

        picker->status = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(picker->status), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(picker->status), 40);
        gtk_style_context_add_class(gtk_widget_get_style_context(picker->status), "muted");
        gtk_box_pack_start(GTK_BOX(page), picker->status, FALSE, FALSE, 0);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, 360, 340);
        picker->grid = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(picker->grid), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(picker->grid), 3);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(picker->grid), TRUE);
        gtk_container_add(GTK_CONTAINER(scroll), picker->grid);
        gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
        GtkWidget* credit = gtk_label_new(_("Powered by GIPHY"));
        gtk_style_context_add_class(gtk_widget_get_style_context(credit), "muted");
        gtk_widget_set_halign(credit, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(page), credit, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(picker->stack), page, "gifs");

        gtk_container_add(GTK_CONTAINER(picker->popover), picker->stack);
        gtk_widget_show_all(picker->stack);
        g_signal_connect(picker->popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) { gtk_widget_destroy(GTK_WIDGET(p)); }), nullptr);

        if (giphy_key().empty()) {
            gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "key");
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "gifs");
            load_results(picker, "");
        }
        gtk_popover_popup(GTK_POPOVER(picker->popover));
        if (!giphy_key().empty())
            gtk_widget_grab_focus(picker->search);
    }

} // namespace tether::ui
